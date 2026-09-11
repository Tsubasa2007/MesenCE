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

	//Whether this stretch is meant to stay on screen after it has run, and whether it is
	//doing so now. A machine that names a still expects it to stand there until it names
	//the next one - it asks once and then goes back to reading its keys - so ending at the
	//foot of the stream would take the picture away a couple of seconds after asking for it.
	bool _hold = false;
	std::atomic<bool> _holding{ false };
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
	std::atomic<double> _seekElapsed{ 0 };
	//Where the stream has reached and how long it is, as last published by ClockFrame. The
	//length is measured once when the video is opened, because measuring it is a seek.
	std::atomic<double> _position{ 0 };
	std::atomic<double> _duration{ 0 };
	//Where the stretch sits inside the stream that was opened for it, in that stream's own
	//seconds. Only a seek needs these; they are not the machine's clock - see below.
	std::atomic<double> _streamStart{ 0 };
	std::atomic<double> _streamOpen{ 0 };
	//How long the stretch has been running, on the disc's clock rather than the stream's.
	//
	//These are not the same length. A stretch is a run of sectors and the drive hands them
	//over at a fixed rate whatever is in them, so an empty one still takes its turn: the
	//tracks here open with a run of empty sectors and some carry more part way in, up to
	//three seconds' worth. The machine counts those - a title asking for the whole of a
	//23.04s track asks for 22 seconds of it, not for the 20.72s of stream inside it - so a
	//stretch has to run for as long as the disc it was cut from, with the picture simply
	//standing still wherever there was nothing to decode.
	std::atomic<double> _elapsed{ 0 };


	//The pointer, and the picture with it drawn on. The decoded picture is left untouched:
	//a still is held for as long as the machine likes and the pointer moves over it, so
	//drawing into it would leave a trail of every place the pointer had been.
	//Which of the two channels to let through, bit 0 left and bit 1 right. A disc that puts
	//one thing in each channel - two words side by side on a page, spoken at the same time -
	//is played one word at a time by asking for one channel, and both at once is what it
	//sounds like otherwise. The chosen channel goes to BOTH outputs, which is what a player
	//does with this and what makes the word audible whichever speaker is listened to.
	uint8_t _audioChannels = 3;
	uint8_t _pointerShape = 0;
	bool _pointerShown = false;
	double _pointerX = 0;
	double _pointerY = 0;
	vector<uint32_t> _withPointer;
	vector<uint32_t> _shown;
	vector<uint32_t> _spare;

public:
	//The stretch of disc to play, counted the way the drive counts it
	//How much further than the stretch to open.
	//
	//The sound that belongs to the last pictures of a stretch is not underneath them: a
	//stream carries it a little ahead of or behind the picture it goes with, so cutting the
	//data off exactly at the last wanted picture loses the end of its sound. Measured on one
	//title's five short stretches, every one came up between a fifth and a third of a second
	//short of sound, which is what was heard. Reading a second further on recovers all of it,
	//and playing still stops where it was asked to.
	static constexpr uint32_t TailSectors = 75;

	//Play a stretch of an item: `fromSector` and `toSector` count from the item's own start,
	//which is how the machine names them.
	//
	//The stretch is not opened on its own. A stream carries the header that says what is in it
	//only at the very beginning, and the demuxer will not start without one, so opening part
	//way in fails outright - which is why a title that plays a video in pieces showed the
	//first piece and then nothing at all. What is opened instead runs from the start of the
	//item up to the end of the stretch, and the stream is then moved to where the stretch
	//begins. The front of it is read and skipped, which costs a few megabytes and is what a
	//real drive does anyway: it seeks past that part of the disc.
	bool Start(CdImageFile& image, uint32_t itemLba, uint32_t itemSectors, uint32_t fromSector, uint32_t toSector, bool hold = false)
	{
		Stop();
		_hold = hold;
		if(toSector <= fromSector) {
			return false;
		}

		//A title counts in the sectors that carry the stream, which is what the decoder is
		//given and what it says it took. The whole of the front is gathered rather than only
		//the wanted part: the header that says what is in the stream is at the beginning, and
		//the demuxer will not start without it.
		uint32_t gathered = _decoder.Open(image, itemLba, itemSectors, toSector + TailSectors);
		if(gathered <= fromSector) {
			return false;
		}

		_decoder.SetAudioLead(LeadSeconds);

		//How long the opened stretch runs, and how far into it the wanted part starts. Both
		//come from the one measurement, so what is offered, what is played and where a seek
		//lands are all on the same clock. The stream's own timestamps are no use here -
		//plm_get_duration hunts for the last one near the end of the data and subtracts the
		//first, which on these reels gave 4.4s for an 85s video and -74.2s for another.
		double streamOpen = gathered / 75.0;
		double streamStart = fromSector / 75.0;
		if(streamOpen <= streamStart) {
			return false;
		}

		if(streamStart > 0 && !_decoder.Seek(streamStart, streamOpen)) {
			return false;
		}

		_streamOpen = streamOpen;
		_streamStart = streamStart;
		//The length asked for, not what was left of it after the end of the item cut it
		//short - a stretch that runs off the end still had that long to run.
		_duration = (toSector - fromSector) / 75.0;
		_elapsed = 0;
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
		_hold = false;
		_holding = false;
		_withPointer.clear();
		_skipRequested = false;
		_seekRequested = false;
		_position = 0;
		_duration = 0;
		_streamOpen = 0;
		_streamStart = 0;
		_elapsed = 0;
	}

	bool IsPlaying() { return _playing; }

	//Standing on the last picture of a stretch that has run its length, waiting to be told
	//what to show next
	bool IsHolding() { return _holding.load(); }

	//Whether what is on screen is a picture being shown rather than a video being played
	//through. The machine is not waiting for one of these to finish - it put it up and went
	//back to its keys - so naming the next one is allowed to replace it at once.
	bool IsShowing() { return _playing && _hold; }


	//The machine sends the drive a position and expects to see a cursor there; on the
	//hardware the drive's own decoder draws it, and the machine itself draws nothing at
	//all while a disc program runs, so if this does not draw it nothing will.
	//Where to put the pointer, in the picture's own pixels - not as a fraction of it.
	//Which channels to play, bit 0 left and bit 1 right - see _audioChannels.
	void SetAudioChannels(uint8_t channels) { _audioChannels = channels & 3; }

	void SetPointer(bool shown, double x, double y, uint8_t shape)
	{
		_pointerShown = shown;
		_pointerShape = shape;
		_pointerX = x;
		_pointerY = y;
	}

	//The transport. A video is the machine's, not ours, so none of this tells the machine
	//anything: it waits either way, and hears about the end once, from ClockFrame.
	bool IsPaused() { return _playing && _paused; }
	void SetPaused(bool paused) { _paused = paused; }
	void RequestSkip() { _skipRequested = true; }
	//Counted from the beginning of the stretch that was asked for, which is what a viewer
	//sees, rather than from the start of the track the stretch was cut out of.
	double GetPosition() { return _playing ? _elapsed.load() : 0; }

	double GetDuration() { return _playing ? _duration.load() : 0; }

	//Asks to move; the move itself happens on the machine's thread at the top of the next
	//frame. Only the request crosses threads. Taken as a position in the stretch and put back
	//onto the track's clock, which is the one the stream is on.
	void RequestSeek(double seconds)
	{
		//Where the bar was let go is a position on the disc; the stream is what has to be
		//moved. Taken as the same fraction of the stream that it is of the stretch - the
		//empty sectors are spread through both, so the two run together closely enough for
		//a jump, and where it lands is measured rather than assumed.
		double length = _duration.load();
		double at = length > 0 ? seconds / length : 0;
		at = at < 0 ? 0 : (at > 1 ? 1 : at);
		_seekTarget = _streamStart.load() + at * (_streamOpen.load() - _streamStart.load());
		_seekElapsed = seconds;
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
		if(_shown.empty()) {
			return nullptr;
		}
		if(!_pointerShown || GetWidth() == 0 || GetHeight() == 0) {
			return _shown.data();
		}

		//Onto a copy. The decoded picture is left alone: a still stands for as long as the
		//machine likes while the pointer moves over it, and drawing into it would leave a
		//trail of everywhere the pointer had been.
		_withPointer = _shown;
		DrawPointer();
		return _withPointer.data();
	}

	//The cursor. Two of the machine's three are drawn here: the arrow it shows while idle and
	//the hourglass it shows after something is clicked. Which is which was measured from the
	//shape the machine sends with the pointer - parked anywhere at all, over a control or over
	//nothing, it asks for 0 for ninety frames out of ninety; in the second after a click it
	//asks for 1, 2 and 3, which cycle, so those are one animated busy cursor rather than three
	//shapes. The machine's third cursor is a pointing hand it shows over a clickable area, and
	//it is NOT drawn here on purpose: the program never asks for it, so whatever decides it is
	//in the drive rather than in anything this can see.
	//
	//These are a likeness, not a copy - the machine's own are in its decoder and only ever seen
	//as frames of video. The hotspot is the arrow's tip and it must stay exactly on the given
	//position, which is what makes a click land where it is aimed, so each shape carries the
	//offset of its own hotspot rather than being drawn from its corner.
	void DrawPointer()
	{
		//w is the sparkle the machine draws around the arrow's point
		static const char* arrow[] = {
			" w          ",
			"wXw         ",
			" XX         ",
			" XoX        ",
			" XooX       ",
			" XoooX      ",
			" XooooX     ",
			" XoooooX    ",
			" XooooooX   ",
			" XoooooooX  ",
			" XooooooooX ",
			" XoooooXXXXX",
			" XooXooX    ",
			" XoX XooX   ",
			" XX  XooX   ",
			" X    XooX  ",
			"       XooX ",
			"       XXXX ",
		};

		//Two frames of the hourglass: white with the grains falling from the top half to the
		//bottom, which is how the machine's own looks - a pale glass with a little dark in it
		//rather than a filled one.
		static const char* glassTop[] = {
			"XXXXXXX",
			"XoooooX",
			"XoXXXoX",
			"XXoXoXX",
			" XoXoX ",
			"  XoX  ",
			"  XoX  ",
			" XoooX ",
			"XXoooXX",
			"XoooooX",
			"XoooooX",
			"XXXXXXX",
		};
		static const char* glassLow[] = {
			"XXXXXXX",
			"XoooooX",
			"XoooooX",
			"XXoooXX",
			" XoooX ",
			"  XoX  ",
			"  XoX  ",
			" XoXoX ",
			"XXoXoXX",
			"XoXXXoX",
			"XoooooX",
			"XXXXXXX",
		};

		static constexpr uint32_t White = 0xFFFFFFFF;
		static constexpr uint32_t Edge = 0xFF000000;

		const char** bitmap = arrow;
		uint32_t rows = 18;
		int32_t hotX = 1, hotY = 1;
		if(_pointerShape != 0) {
			//The busy cursor, stepped by the shape the machine is asking for so that it turns
			//over the way the machine's own does
			bitmap = (_pointerShape & 1) ? glassTop : glassLow;
			rows = 12;
			hotX = 3;
			hotY = 0;
		}

		uint32_t width = GetWidth(), height = GetHeight();
		int32_t left = (int32_t)_pointerX - hotX;
		int32_t top = (int32_t)_pointerY - hotY;
		for(uint32_t row = 0; row < rows; row++) {
			int32_t y = top + (int32_t)row;
			if(y < 0 || y >= (int32_t)height) {
				continue;
			}
			const char* line = bitmap[row];
			for(uint32_t col = 0; line[col]; col++) {
				if(line[col] == ' ') {
					continue;
				}
				int32_t x = left + (int32_t)col;
				if(x < 0 || x >= (int32_t)width) {
					continue;
				}
				uint32_t colour = line[col] == 'X' ? Edge : White;
				_withPointer[(size_t)y * width + x] = colour;
			}
		}
	}

	//Whether that buffer is a picture this decoded. What draws the screen runs on its own
	//thread and is handed a frame as a bare address; this is how it tells a picture from the
	//machine's own output, which is a buffer of a different shape entirely.
	bool OwnsPicture(const uint32_t* buffer)
	{
		return _playing && buffer && (buffer == _shown.data() || buffer == _spare.data() || buffer == _withPointer.data());
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

		if(take < want) {
			//Nothing there for the rest of this call. Handing back nothing at all would let
			//the machine's own output stand in for the gap, at its own rate - which is what
			//an empty stretch of disc used to sound like. Silence at the video's rate keeps
			//the mixer on one rate and on one source for as long as the video is on screen.
			_block.resize(want * 2, 0);
			_owed -= (double)(want - take);
			take = want;
		}

		SelectChannels();

		samples = _block.data();
		sampleCount = (uint32_t)take;
		sampleRate = (uint32_t)rate;
		return sampleCount > 0;
	}

	//Let through only the channels asked for, copying whichever is kept to both outputs.
	void SelectChannels()
	{
		if(_audioChannels == 3) {
			return;
		}
		for(size_t i = 0; i + 1 < _block.size(); i += 2) {
			int16_t keep = 0;
			if(_audioChannels == 1) {
				keep = _block[i];
			} else if(_audioChannels == 2) {
				keep = _block[i + 1];
			}
			_block[i] = keep;
			_block[i + 1] = keep;
		}
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
			_decoder.Seek(_seekTarget.load(), _streamOpen.load());
			_fifo.clear();
			_fifoPos = 0;
			_block.clear();
			_owed = 0;
			_elapsed = _seekElapsed.load();
			_position = _elapsed.load();
		}

		if(_paused || _holding.load()) {
			return true;
		}

		//Both clocks are the same one here: a stretch is counted in sectors that carry
		//something, and those are exactly the sectors the stream is made of.
		double tick = consoleFps > 1 ? 1.0 / consoleFps : 1.0 / 50.0;
		_elapsed = _elapsed.load() + tick;
		_position = _elapsed.load();
		_decoder.Advance(tick);

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

		//Over when the stretch has had its turn on the disc. The stream inside it can run out
		//before that - the empty sectors at the end of a track are still part of it - and when
		//it does the picture simply stands until the time is up, which is what the machine
		//would see. Nothing is held back for the sound: what is still queued belongs past the
		//end, and the sound for the pictures shown has already gone out.
		if(_duration > 0 && _elapsed.load() >= _duration.load()) {
			if(_hold) {
				//Stand here showing the last picture. The machine is not told, because as far
				//as it is concerned nothing has happened: it asked for a still and the still
				//is up. What ends this is the next thing it asks for - see
				//NesConsole::ClockDiscVideo, which takes that request from a held picture as
				//readily as from a blank screen.
				_holding = true;
				return true;
			}
			Stop();
			return false;
		}
		return true;
	}
};
