#pragma once
#include "pch.h"
#include "NES/Mappers/CdImageFile.h"
#include "NES/Mappers/CdVideoDecoder.h"

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

	bool _playing = false;
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
	}

	bool IsPlaying() { return _playing; }
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

		_decoder.Advance(consoleFps > 1 ? 1.0 / consoleFps : 1.0 / 50.0);

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
