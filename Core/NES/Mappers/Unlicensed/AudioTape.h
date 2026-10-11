#pragma once
#include "pch.h"
#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "Shared/MessageManager.h"
#include "Utilities/Audio/AudioFileReader.h"
#include "Utilities/FolderUtilities.h"

//A cassette recorded for the audio-visual cards (see PlqAvCard): speech on the left channel,
//the card's data on the right, as a pulse train the card's comparator turns into a single bit.
//The file is decoded once, on a thread of its own, into what the emulation reads - that bit at
//176.4KHz and the speech at 22.05KHz - and the emulation only waits for the thread when it gets
//ahead of it. Everything the emulation reads comes from the decoded file and nothing else, so a
//run replays the same however fast the decoding went.
class AudioTape
{
public:
	static constexpr uint32_t TickRate = 176400;
	static constexpr uint32_t SpeechRate = 22050;
	static constexpr uint32_t TicksPerSpeechSample = TickRate / SpeechRate;

	//The two pulse codes the recordings use: the data line cycles at 42 and 84 ticks on some,
	//and at 48 and 96 on the others
	enum class PulseCode { Unknown, Short, Long };

private:
	//The comparator's level: the data channel reads 1 below it, as the reference emulator has it
	static constexpr int32_t Threshold = -1000;

	//Stored in chunks that are never moved once written, so the emulation can read what the
	//decoding thread has published while it goes on writing past it
	static constexpr uint32_t ChunkShift = 20;
	static constexpr uint64_t ChunkTicks = 1ull << ChunkShift;
	static constexpr uint64_t MaxTicks = (uint64_t)TickRate * 60 * 120;
	static constexpr uint32_t MaxChunks = (uint32_t)(MaxTicks >> ChunkShift) + 1;

	string _path;
	std::array<unique_ptr<uint64_t[]>, MaxChunks> _bits;
	std::array<unique_ptr<int16_t[]>, MaxChunks> _speech;
	std::atomic<uint64_t> _readyTicks = 0;
	std::atomic<bool> _finished = false;
	std::atomic<bool> _stop = false;
	std::mutex _mutex;
	std::condition_variable _ready;
	std::thread _thread;

	inline static std::mutex _cacheLock;
	inline static shared_ptr<AudioTape> _cache;

	void Publish(uint64_t ticks, bool finished)
	{
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_readyTicks.store(ticks, std::memory_order_release);
			if(finished) {
				_finished = true;
			}
		}
		_ready.notify_all();
	}

	void Decode()
	{
		uint32_t rate = 0;
		uint64_t frame = 0;
		uint64_t tick = 0;
		int16_t lastData = 0;
		uint64_t speechIndex = 0;
		int32_t speechSum = 0;
		uint32_t speechCount = 0;
		int16_t lastSpeech = 0;

		auto ensureChunk = [this](uint64_t t) {
			uint32_t chunk = (uint32_t)(t >> ChunkShift);
			if(!_bits[chunk]) {
				_bits[chunk].reset(new uint64_t[ChunkTicks / 64]());
				_speech[chunk].reset(new int16_t[ChunkTicks / TicksPerSpeechSample]());
			}
		};
		auto putSpeech = [this](uint64_t index, int16_t value) {
			uint64_t t = index * TicksPerSpeechSample;
			_speech[t >> ChunkShift][(t & (ChunkTicks - 1)) / TicksPerSpeechSample] = value;
		};

		auto onFormat = [&](uint32_t sampleRate) {
			rate = sampleRate;
			return !_stop;
		};

		auto onData = [&](const int16_t* frames, uint32_t count) {
			for(uint32_t i = 0; i < count; i++, frame++) {
				int16_t speech = frames[i * 2];
				int16_t data = frames[i * 2 + 1];

				//Every tick that falls between the previous frame and this one, the data channel
				//interpolated across the gap
				if(frame > 0) {
					while(tick * rate < frame * TickRate) {
						if(tick >= MaxTicks) {
							return false;
						}
						ensureChunk(tick);
						int64_t into = (int64_t)(tick * rate - (frame - 1) * TickRate);
						int32_t level = lastData + (int32_t)((int64_t)(data - lastData) * into / TickRate);
						if(level < Threshold) {
							_bits[tick >> ChunkShift][(tick & (ChunkTicks - 1)) >> 6] |= 1ull << (tick & 63);
						}
						tick++;
					}
				}
				lastData = data;

				//The speech averaged over each output sample's span, which keeps a higher-rate
				//file from folding back into the audible range
				uint64_t index = frame * SpeechRate / rate;
				if(index != speechIndex) {
					if(speechCount) {
						lastSpeech = (int16_t)(speechSum / (int32_t)speechCount);
					}
					while(speechIndex < index) {
						if(speechIndex * TicksPerSpeechSample >= MaxTicks) {
							return false;
						}
						ensureChunk(speechIndex * TicksPerSpeechSample);
						putSpeech(speechIndex++, lastSpeech);
					}
					speechSum = 0;
					speechCount = 0;
				}
				speechSum += speech;
				speechCount++;
			}
			Publish(std::min(tick, speechIndex * TicksPerSpeechSample), false);
			return !_stop;
		};

		string error;
		if(!AudioFileReader::Decode(_path, onFormat, onData, error)) {
			MessageManager::DisplayMessage("Tape", "Could not read " + FolderUtilities::GetFilename(_path, true) + ": " + error);
		}
		Publish(std::min(tick, speechIndex * TicksPerSpeechSample), true);
	}

	//The decoded position covers the given tick, or the file ends before it
	bool WaitFor(uint64_t tick)
	{
		if(tick < _readyTicks.load(std::memory_order_acquire)) {
			return true;
		}
		std::unique_lock<std::mutex> lock(_mutex);
		_ready.wait(lock, [this, tick]() { return tick < _readyTicks.load(std::memory_order_acquire) || _finished; });
		return tick < _readyTicks.load(std::memory_order_acquire);
	}

public:
	AudioTape(const string& path) : _path(path)
	{
		_thread = std::thread([this]() { Decode(); });
	}

	~AudioTape()
	{
		_stop = true;
		if(_thread.joinable()) {
			_thread.join();
		}
	}

	//The same file again is the tape already decoded: a power cycle builds the card anew, and
	//decoding half an hour of audio each time would hold every reboot up
	static shared_ptr<AudioTape> Open(const string& path)
	{
		std::lock_guard<std::mutex> lock(_cacheLock);
		if(!_cache || _cache->_path != path) {
			_cache.reset();
			_cache = std::make_shared<AudioTape>(path);
		}
		return _cache;
	}

	const string& GetPath() { return _path; }

	//The data line at a tick; past the end of the recording the line is quiet
	bool GetBit(uint64_t tick)
	{
		if(!WaitFor(tick)) {
			return false;
		}
		return (_bits[tick >> ChunkShift][(tick & (ChunkTicks - 1)) >> 6] >> (tick & 63)) & 1;
	}

	int16_t GetSpeech(uint64_t index)
	{
		uint64_t tick = index * TicksPerSpeechSample;
		if(!WaitFor(tick + TicksPerSpeechSample - 1)) {
			return 0;
		}
		return _speech[tick >> ChunkShift][(tick & (ChunkTicks - 1)) / TicksPerSpeechSample];
	}

	//The recording's length in ticks once the whole file is decoded, 0 until then
	uint64_t GetLength()
	{
		return _finished ? _readyTicks.load(std::memory_order_acquire) : 0;
	}

	bool IsPast(uint64_t tick)
	{
		return _finished && tick >= _readyTicks.load(std::memory_order_acquire);
	}

	//Which code the recording's data is in, from its first stretch of data: once enough short
	//and long cycles have gone by, the long ones say which (84 or 96 ticks). Unknown when the
	//first minute holds no data to tell by.
	PulseCode DetectPulseCode()
	{
		uint32_t shortCycles = 0;
		uint32_t long84 = 0;
		uint32_t long96 = 0;
		bool last = false;
		uint64_t lastFall = UINT64_MAX;
		for(uint64_t tick = 0; tick < (uint64_t)TickRate * 60 && WaitFor(tick); tick++) {
			bool bit = (_bits[tick >> ChunkShift][(tick & (ChunkTicks - 1)) >> 6] >> (tick & 63)) & 1;
			if(last && !bit) {
				if(lastFall != UINT64_MAX) {
					uint64_t period = tick - lastFall;
					if(period >= 36 && period < 54) {
						shortCycles++;
					} else if(period >= 78 && period < 90) {
						long84++;
					} else if(period >= 90 && period < 104) {
						long96++;
					}
					if(shortCycles >= 300 && long84 + long96 >= 1000) {
						return long84 > long96 ? PulseCode::Short : PulseCode::Long;
					}
				}
				lastFall = tick;
			}
			last = bit;
		}
		return PulseCode::Unknown;
	}
};
