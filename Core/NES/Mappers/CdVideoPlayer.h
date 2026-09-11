#pragma once
#include "pch.h"
#include <atomic>
#include "NES/Mappers/CdImageFile.h"
#include "NES/Mappers/CdVideoDecoder.h"
#include "NES/Mappers/CdSegmentIndex.h"

//A video off the disc, on the screen.
//
//While one of these plays, the machine draws nothing: it hands the screen to the decoder
//chip beside it and waits to be told the video is over. So this does not composite with the
//emulated picture - it replaces it, which is what the hardware does, and the machine's own
//output comes back when the video ends. Measured on a real disc: the program blanks its
//whole screen, chrome and pointer included, about eighteen frames after it asks.
//
//The stream runs at its own rate - 25 pictures a second on these discs - and the machine at
//about fifty frames a second, so a picture is not decoded every frame. Time is accumulated
//against the console's own rate rather than assuming a ratio, so a machine running at a
//different rate still plays the video at the right speed.
class CdVideoPlayer
{
private:
	//How much sound to keep decoded ahead of the picture. The stream is carried forward once
	//per emulated frame and gives up its sound in lumps of about 26ms, while it is asked for
	//in smaller and unevenly spaced pieces; without a cushion between the two, some of those
	//pieces find nothing waiting. A quarter of a second is far more than the unevenness needs
	//and is not itself a delay: the queue is drained from the front, so what is handed over is
	//still the sound belonging to the picture on screen.
	static constexpr double LeadSeconds = 0.25;

	//The most sound a single request may claim after coming up short, so a long stall cannot
	//be repaid all at once as a burst
	static constexpr double MaxOwedSeconds = 0.1;

	CdVideoDecoder _decoder;

	//Decoded sound waiting its turn, oldest first, and how much of it has gone
	vector<int16_t> _fifo;
	size_t _fifoPos = 0;
	//The block handed over for the stretch of time that has just passed
	vector<int16_t> _block;
	//Sound owed for time already run, carried between requests so the fraction of a sample
	//each one leaves behind is not lost
	double _owed = 0;

	//A front end reads these from its own thread while the machine's thread is decoding, so
	//what it can see is published rather than asked for. Nothing outside may touch the decoder
	//itself: plm_get_duration seeks the demuxer to the end of the stream and back to find the
	//last timestamp, so calling it while a frame is being decoded moves the read position out
	//from under the decode - which cut videos short, stopped later ones playing at all, and
	//brought the emulator down when the bar was dragged.
	std::atomic<bool> _playing{ false };
	//Held rather than stopped: the machine stays in its wait loop, which is what it would be
	//doing anyway, and the stream simply is not carried forward. Its own sound is silent
	//meanwhile, so silence is handed over at the video's rate rather than letting the mixer
	//fall back to the machine's - changing rate mid-play resets the filter that resamples it.
	std::atomic<bool> _paused{ false };
	//Asked to end early, or to move. Both are answered on the machine's own thread at the top
	//of the next frame, so the decoder is only ever touched from the one thread that decodes.
	std::atomic<bool> _skipRequested{ false };
	std::atomic<bool> _seekRequested{ false };
	std::atomic<double> _seekTarget{ 0 };
	//Where the stream has reached and how long it is, as last published by ClockFrame. The
	//length is measured once when the video is opened, because measuring it is a seek.
	std::atomic<double> _position{ 0 };
	std::atomic<double> _duration{ 0 };
	vector<uint32_t> _shown;
	vector<uint32_t> _spare;

public:
	//The stretch of disc to play, counted the way the drive counts it
	bool Start(CdImageFile& image, uint32_t lba, uint32_t sectors)
	{
		Stop();
		if(!_decoder.Open(image, lba, sectors)) {
			return false;
		}

		_decoder.SetAudioLead(LeadSeconds);
		//How long the video runs, taken from how much disc it occupies: a sector is a
		//seventy-fifth of a second on these discs, which is the same arithmetic the drive
		//does. The stream's own timestamps are not usable here - plm_get_duration hunts for
		//the last timestamp near the end of the data and subtracts the first, and on these
		//reels that gave 5.5s for a 7s item, 4.4s for an 85s one, and -74.2s for another.
		//It is also a seek, which is not something to do to a stream that is being decoded.
		double discSeconds = 0;
		double streamSeconds = 0;
		CdSegmentIndex::MeasureItem(image, lba, sectors, discSeconds, streamSeconds);
		_duration = discSeconds;
		_position = 0;
		_playing = true;

		return true;
	}

	void Stop()
	{
		_decoder.Close();
		_fifo.clear();
		_fifoPos = 0;
		_block.clear();
		_owed = 0;
		_shown.clear();
		_spare.clear();
		_playing = false;
		_paused = false;
		_skipRequested = false;
		_seekRequested = false;
		_position = 0;
		_duration = 0;
	}

	bool IsPlaying() { return _playing; }

	//The transport. A video is the machine's, not ours, so none of this tells the machine
	//anything: it waits either way, and hears about the end once, from ClockFrame.
	bool IsPaused() { return _playing && _paused; }
	void SetPaused(bool paused) { _paused = paused; }
	void RequestSkip() { _skipRequested = true; }
	double GetPosition() { return _playing ? _position.load() : 0; }
	double GetDuration() { return _playing ? _duration.load() : 0; }

	//Asks to move; the move itself happens on the machine's thread at the top of the next
	//frame. Only the request crosses threads.
	void RequestSeek(double seconds)
	{
		_seekTarget = seconds;
		_seekRequested = true;
	}

	uint32_t GetWidth() { return _decoder.GetWidth(); }
	uint32_t GetHeight() { return _decoder.GetHeight(); }

	//The picture the front end is drawing. Not the decoder's own buffer: the stream is moved
	//on at the end of a frame, after the frame built from it has been handed over and while
	//the thread that draws it may still be reading, and decoding into that same buffer frees
	//it underneath. Two of them, swapped as a new picture arrives, the way the PPU's own
	//output is handled.
	uint32_t* GetFrameBuffer()
	{
		return _shown.empty() ? nullptr : _shown.data();
	}

	//Whether that buffer is a picture this decoded. What draws the screen runs on its own
	//thread and is handed a frame as a bare address; this is how it tells a picture from the
	//machine's own output, which is a buffer of a different shape entirely.
	bool OwnsPicture(const uint32_t* buffer)
	{
		return _playing && buffer && (buffer == _shown.data() || buffer == _spare.data());
	}

	int GetSampleRate() { return _decoder.GetSampleRate(); }

	//The video's own sound for the stretch of time the machine has just run. It is stopped in
	//its wait loop while this plays, so this is its sound for now - it replaces what the APU
	//would have said rather than mixing with it, the same way the picture replaces the
	//machine's own.
	//
	//elapsedSamples, at elapsedRate, is how much sound the machine itself produced for that
	//stretch, which is what says how long it was. Exactly that much of the video is handed
	//over: the sound is asked for on a count of processor cycles rather than once a frame, so
	//handing over everything decoded made the size of each block depend on where the request
	//happened to fall between two decodes - long, short, long, empty - and a stream chopped up
	//that unevenly does not come out of the speakers as one.
	bool TakeAudio(int16_t*& samples, uint32_t& sampleCount, uint32_t& sampleRate, uint32_t elapsedSamples, uint32_t elapsedRate)
	{
		int rate = _decoder.GetSampleRate();
		if(!_playing || rate <= 0 || elapsedRate == 0 || elapsedSamples == 0) {
			return false;
		}

		//Held: the stream has not moved, so nothing is owed against it. Silence at its own
		//rate keeps the mixer on one rate across the pause.
		if(_paused) {
			_owed = 0;
			size_t want = (size_t)((double)elapsedSamples * rate / elapsedRate);
			_block.assign(want * 2, 0);
			samples = _block.data();
			sampleCount = (uint32_t)want;
			sampleRate = (uint32_t)rate;
			return sampleCount > 0;
		}

		_owed += (double)elapsedSamples * rate / elapsedRate;

		size_t want = (size_t)_owed;
		size_t have = (_fifo.size() - _fifoPos) / 2;
		size_t take = std::min(want, have);

		_block.assign(_fifo.begin() + _fifoPos, _fifo.begin() + _fifoPos + take * 2);
		_fifoPos += take * 2;

		//Whatever could not be covered stays owed and is handed over as soon as there is more
		//of it, so a moment of running dry costs the sound its smoothness and not its pitch
		_owed -= take;
		_owed = std::min(_owed, rate * MaxOwedSeconds);

		samples = _block.data();
		sampleCount = (uint32_t)take;
		sampleRate = (uint32_t)rate;
		return sampleCount > 0;
	}

	//One emulated frame's worth of the stream. False once it has run out, which is the
	//machine's cue to take its screen back.
	bool ClockFrame(double consoleFps)
	{
		if(!_playing) {
			return false;
		}

		if(_skipRequested.exchange(false)) {
			//Told to end early. The machine is let out the same way it would have been by the
			//stream running out, so what it does next is what it would have done anyway.
			Stop();
			return false;
		}

		if(_seekRequested.exchange(false)) {
			//Everything already decoded belongs to where the stream used to be: the queued
			//sound would play a quarter second of the old place over the new picture, and the
			//sound owed for time already run is owed against a stream that has moved.
			_decoder.Seek(_seekTarget.load());
			_fifo.clear();
			_fifoPos = 0;
			_block.clear();
			_owed = 0;
			_position = _decoder.GetTime();
		}

		if(_paused) {
			return true;
		}

		_decoder.Advance(consoleFps > 1 ? 1.0 / consoleFps : 1.0 / 50.0);
		_position = _decoder.GetTime();

		if(_decoder.HasPicture()) {
			//Into the buffer nothing is reading, then swapped in
			_spare = _decoder.GetPicture();
			_shown.swap(_spare);
		}

		//What has gone joins what is waiting only once it is worth the move
		if(_fifoPos > 0 && _fifoPos * 2 >= _fifo.size()) {
			_fifo.erase(_fifo.begin(), _fifo.begin() + _fifoPos);
			_fifoPos = 0;
		}
		vector<int16_t>& decoded = _decoder.GetSamples();
		if(!decoded.empty()) {
			_fifo.insert(_fifo.end(), decoded.begin(), decoded.end());
			decoded.clear();
		}

		if(_decoder.HasEnded() && _fifoPos >= _fifo.size()) {
			Stop();
			return false;
		}
		return true;
	}
};
