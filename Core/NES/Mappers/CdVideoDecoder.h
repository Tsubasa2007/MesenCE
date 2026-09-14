#pragma once
#include "pch.h"
#include "NES/Mappers/CdImageFile.h"
#include "NES/Mappers/CdSegmentIndex.h"
#include "Utilities/pl_mpeg.h"

//Decoding the video these discs carry.
//
//What is on them is an MPEG program stream - MPEG-1 video at 352x288 with MP2 audio - laid
//out in Mode 2 Form 2 sectors, 2324 bytes of payload each. The machines themselves never
//decode it: the picture comes from a chip beside the CPU that owns the screen while it runs,
//which is why a program playing one draws nothing of its own and the emulated screen is
//blank. This is that chip's job.
//
//The stream is fed from the disc rather than from a file. A sector carries its payload after
//the sync, header and subheader, and only the sectors whose subheader claims video, audio or
//data are stream at all - the rest is the padding an item's fixed allocation is made up to,
//and passing it on would break the demuxer.
class CdVideoDecoder
{
private:
	static constexpr uint32_t RawSectorSize = 2352;
	static constexpr uint32_t Form2PayloadSize = 2324;
	static constexpr uint32_t Form1PayloadSize = 2048;

	//How far in to look for the streams the disc really has. A few hundred KB is the
	//library's own guidance; an item here is about a tenth of that.
	static constexpr size_t ProbeSize = 512 * 1024;

	//The layers a stream is built out of, as the four-byte codes that mark them
	static constexpr uint8_t PackStart = 0xBA;
	static constexpr uint8_t StreamEnd = 0xB9;
	static constexpr uint8_t FirstVideoStream = 0xE0;
	static constexpr uint8_t SecondVideoStream = 0xE1;
	static constexpr size_t PackHeaderSize = 12;

	//How far behind a wanted position to pick the stream up, and in what steps to run up to
	//it. These streams put a self-contained picture in about every half second, so this
	//clears one comfortably; the run costs a few milliseconds and only happens on a jump.
	static constexpr double RunUpSeconds = 1.5;
	static constexpr double StepSeconds = 1.0 / 50.0;

	//How far into a stream to jump when finding out what a jump costs it. Far enough in to be
	//an ordinary jump rather than a special case, near enough the front to be cheap.
	static constexpr double CalibrateMark = 3.0;

	//How far either side of the mark to look for the picture a jump actually brought up
	static constexpr double CalibrateWindow = 1.0;

	plm_t* _plm = nullptr;
	plm_buffer_t* _buffer = nullptr;

	//Filled by the library as it decodes: the newest picture, and the sound behind it
	vector<uint32_t> _picture;
	vector<int16_t> _samples;
	bool _hasPicture = false;

	//Somewhere for the reference reader to put its pictures, kept apart from this decoder's
	struct Watcher
	{
		vector<uint32_t>* Picture;
		bool* Has;
	};

	static void OnReferenceVideo(plm_t*, plm_frame_t* frame, void* user)
	{
		Watcher* watcher = (Watcher*)user;
		watcher->Picture->resize((size_t)frame->width * frame->height);
		plm_frame_to_bgra(frame, (uint8_t*)watcher->Picture->data(), (int)(frame->width * 4));
		*watcher->Has = true;
	}

	static void OnVideo(plm_t*, plm_frame_t* frame, void* user)
	{
		CdVideoDecoder* self = (CdVideoDecoder*)user;
		self->_picture.resize((size_t)frame->width * frame->height);
		plm_frame_to_bgra(frame, (uint8_t*)self->_picture.data(), (int)(frame->width * 4));
		self->_hasPicture = true;
	}

	static void OnAudio(plm_t*, plm_samples_t* samples, void* user)
	{
		CdVideoDecoder* self = (CdVideoDecoder*)user;
		size_t at = self->_samples.size();
		size_t count = (size_t)samples->count * 2;
		self->_samples.resize(at + count);
		for(size_t i = 0; i < count; i++) {
			//A sample that has been through a filter can sit outside the range
			float value = samples->interleaved[i] * 32767.0f;
			self->_samples[at + i] = (int16_t)(value > 32767.0f ? 32767.0f : (value < -32768.0f ? -32768.0f : value));
		}
	}

	//The stream as it was gathered off the disc, kept because the demuxer reads from it
	vector<uint8_t> _stream;

	uint32_t _width = 0;
	uint32_t _height = 0;

	//Where in the item the demuxer was last put. Its own clock starts again from zero there,
	//so this is what turns it back into a position on the disc.
	double _timeBase = 0;
	//How far ahead of the picture the sound is being decoded, kept so a jump can put it aside
	double _audioLead = 0;

	//How much later than asked a jump has to aim - see Calibrate
	double _seekLate = 0;

public:
	~CdVideoDecoder()
	{
		Close();
	}

	void Close()
	{
		if(_plm) {
			//Destroys the buffer it was created with as well
			plm_destroy(_plm);
			_plm = nullptr;
			_buffer = nullptr;
		}
		_stream.clear();
		_picture.clear();
		_samples.clear();
		_hasPicture = false;
		_width = 0;
		_height = 0;
		_timeBase = 0;
	}

	//Gather one stretch of the disc into a stream the demuxer can read, and say how many of
	//the item's sectors that came to.
	//
	//sectors is counted from lba the way the drive counts it - an item's whole allocation,
	//padding included. `take` is counted in the sectors that carry the stream, nought meaning
	//all of them. Which sectors those are is not decided here: see
	//CdSegmentIndex::ForEachStreamSector, which is also what writes one of these out for
	//another player to open.
	uint32_t Open(CdImageFile& image, uint32_t lba, uint32_t sectors, uint32_t take = 0)
	{
		Close();
		if(!image.IsOpen()) {
			return 0;
		}

		_stream.reserve((size_t)sectors * Form2PayloadSize);
		uint32_t gathered = CdSegmentIndex::ForEachStreamSector(image, lba, sectors, 0, take,
			[this](vector<uint8_t>& payload) {
				_stream.insert(_stream.end(), payload.begin(), payload.end());
			});

		if(_stream.empty()) {
			return 0;
		}

		MoveStillsToTheFirstVideoStream();

		if(!StartReading()) {
			Close();
			return 0;
		}

		_width = (uint32_t)plm_get_width(_plm);
		_height = (uint32_t)plm_get_height(_plm);
		if(_width == 0 || _height == 0) {
			return 0;
		}

		Calibrate();
		return gathered;
	}

private:
	//A disc keeps its still pictures on the second of its video streams, which is what the
	//standard sets that stream aside for, and its moving video on the first. The decoder only
	//ever looks at the first, so a menu made of stills came back as a stream with no video in
	//it at all - the pictures were there and were never found.
	//
	//Rather than teach the decoder a second stream it has no other use for, the stills are
	//moved into the first, which is empty on an item that carries them. An item with anything
	//already in the first stream is left exactly as it is.
	void MoveStillsToTheFirstVideoStream()
	{
		vector<uint32_t> stills;
		bool hasMovingVideo = false;

		//Walking the layers rather than searching the bytes: these four bytes mean a packet
		//only where a packet begins, and inside a picture they are as likely to be part of one.
		//
		//The chain does not run clean from one end of an item to the other. A sector's packets
		//do not always fill it - some end a few bytes short of the payload, and what is left is
		//not a packet - so where the chain breaks the walk picks the next sector up instead of
		//giving up. Giving up is what it did, five sectors into a menu page, and the pictures
		//further in were never seen.
		size_t at = 0;
		while(at + 4 <= _stream.size()) {
			if(_stream[at] != 0 || _stream[at + 1] != 0 || _stream[at + 2] != 1) {
				at = NextPack(at);
				continue;
			}

			uint8_t code = _stream[at + 3];
			if(code == PackStart) {
				//An MPEG-1 pack header is a fixed twelve bytes
				at += PackHeaderSize;
				continue;
			}
			if(code == StreamEnd) {
				at += 4;
				continue;
			}
			if(at + 6 > _stream.size()) {
				break;
			}

			size_t length = ((size_t)_stream[at + 4] << 8) | _stream[at + 5];
			if(length == 0) {
				at = NextPack(at);
				continue;
			}
			if(code == FirstVideoStream) {
				hasMovingVideo = true;
			} else if(code == SecondVideoStream) {
				stills.push_back((uint32_t)(at + 3));
			}
			at += 6 + length;
		}

		if(hasMovingVideo) {
			return;
		}
		for(uint32_t position : stills) {
			_stream[position] = FirstVideoStream;
		}
	}

	//Where the next sector's pack begins, past a point the chain could not be followed from
	size_t NextPack(size_t from)
	{
		for(size_t at = from + 1; at + 4 <= _stream.size(); at++) {
			if(_stream[at] == 0 && _stream[at + 1] == 0 && _stream[at + 2] == 1 &&
				_stream[at + 3] == PackStart) {
				return at;
			}
		}
		return _stream.size();
	}

	//A reader over the gathered stream, at its beginning. Cheap - the bytes are already here
	//and are not copied - so this is also how the stream is put back to the start: winding a
	//reader back with a seek is not the same thing, and left the stream running a quarter of
	//a second behind where a fresh one does.
	bool StartReading()
	{
		if(_plm) {
			//Destroys the buffer it was made with as well
			plm_destroy(_plm);
			_plm = nullptr;
			_buffer = nullptr;
		}

		_buffer = plm_buffer_create_with_memory(_stream.data(), (size_t)_stream.size(), 0);
		if(!_buffer) {
			return false;
		}

		_plm = plm_create_with_buffer(_buffer, 1);
		if(!_plm) {
			plm_buffer_destroy(_buffer);
			_buffer = nullptr;
			return false;
		}

		//What the system header claims is not what these discs carry - the library says as
		//much, and names Video CD as the case where the header cannot be believed. Probing
		//reads far enough in to find the streams that are really there.
		if(!plm_probe(_plm, ProbeSize) || plm_get_num_video_streams(_plm) == 0) {
			return false;
		}

		plm_set_video_decode_callback(_plm, OnVideo, this);
		plm_set_audio_decode_callback(_plm, OnAudio, this);
		if(_audioLead > 0) {
			plm_set_audio_lead_time(_plm, _audioLead);
		}
		return true;
	}

public:

	//What a jump actually costs this stream, measured rather than reasoned about.
	//
	//A jump lands short: the picture that comes up belongs a little earlier than the moment
	//asked for. Four different accounts of why were tried and measured, and the two that
	//claimed to explain it overshot by twice the error - so what the lag is made of is not
	//settled. What it comes to is, and it is the same on every jump into a stream, so it can
	//simply be measured once here and added on afterwards.
	//
	//Played to a mark, the picture there is kept. The same mark is then jumped to, and the
	//stream carried forward until that same picture comes round again. How far it had to go
	//is how short the jump fell, and every later jump aims that much further on.
	//
	//A still stretch matches at once and measures nothing, which is harmless: where no two
	//pictures differ, landing on either is the same picture.
	void Calibrate()
	{
		_seekLate = 0;
		if(!_plm || _stream.empty()) {
			return;
		}

		double duration = (double)_stream.size() / (2324.0 * 75.0);
		double mark = duration * 0.5 < CalibrateMark ? duration * 0.5 : CalibrateMark;
		if(duration < CalibrateMark || mark <= CalibrateWindow) {
			//Too short to have anywhere to jump into
			Rewind();
			return;
		}

		//The pictures either side of the mark, as a stream that has only ever been played
		//gives them. A second reader of the same bytes rather than this one wound back: what
		//is being measured is what a jump costs, so the thing it is measured against must
		//never have jumped. Measuring against this decoder rewound gave twice the answer,
		//because the reference had been displaced by the very lag being looked for.
		vector<uint64_t> shown;
		vector<double> when;
		Reference(mark, shown, when);
		if(shown.empty()) {
			Rewind();
			return;
		}

		//Jump to the mark and see which of those pictures actually came up
		if(!Seek(mark, duration) || !_hasPicture) {
			Rewind();
			return;
		}

		uint64_t landed = Signature(_picture);
		for(size_t i = 0; i < shown.size(); i++) {
			if(shown[i] != landed) {
				continue;
			}
			double late = mark - when[i];
			//Only a plausible amount. A still stretch matches everywhere and measures
			//nothing, which is harmless: where no two pictures differ, either is the same
			//picture.
			if(late > 0 && late < CalibrateWindow) {
				_seekLate = late;
			}
			break;
		}

		Rewind();
	}

	//Play the same bytes through a reader of their own, and note the pictures around the
	//mark. Nothing here touches the decoder being calibrated.
	void Reference(double mark, vector<uint64_t>& shown, vector<double>& when)
	{
		plm_buffer_t* buffer = plm_buffer_create_with_memory(_stream.data(), _stream.size(), 0);
		if(!buffer) {
			return;
		}

		plm_t* plm = plm_create_with_buffer(buffer, 1);
		if(!plm) {
			plm_buffer_destroy(buffer);
			return;
		}

		if(plm_probe(plm, ProbeSize) && plm_get_num_video_streams(plm) > 0) {
			//Only the picture matters here
			plm_set_audio_enabled(plm, 0);
			vector<uint32_t> picture;
			bool has = false;
			Watcher watcher = { &picture, &has };
			plm_set_video_decode_callback(plm, OnReferenceVideo, &watcher);

			for(uint32_t step = 0; step < 4000; step++) {
				double at = plm_get_time(plm);
				if(at > mark + CalibrateWindow || plm_has_ended(plm)) {
					break;
				}
				if(has && at > mark - CalibrateWindow) {
					shown.push_back(Signature(picture));
					when.push_back(at);
				}
				plm_decode(plm, StepSeconds);
			}
		}

		plm_destroy(plm);
	}
	//A number standing for a picture, so a run of them can be kept without keeping the
	//pictures themselves
	static uint64_t Signature(const vector<uint32_t>& picture)
	{
		uint64_t value = 1469598103934665603ull;
		for(uint32_t pixel : picture) {
			value = (value ^ pixel) * 1099511628211ull;
		}
		return value;
	}
	//Back to the beginning, as if nothing had been read yet
	void Rewind()
	{
		StartReading();
		_samples.clear();
		_hasPicture = false;
		_timeBase = 0;
	}

	bool IsOpen() { return _plm != nullptr; }
	uint32_t GetWidth() { return _width; }
	uint32_t GetHeight() { return _height; }
	double GetDuration() { return _plm ? plm_get_duration(_plm) : 0; }
	double GetFrameRate() { return _plm ? plm_get_framerate(_plm) : 0; }
	uint32_t GetStreamSize() { return (uint32_t)_stream.size(); }

	int GetSampleRate() { return _plm ? plm_get_samplerate(_plm) : 0; }

	//How far into the item the picture has got. The demuxer counts from wherever it was
	//last put, so a seek's landing point is added back on.
	double GetTime() { return _plm ? _timeBase + plm_get_time(_plm) : 0; }

	//Move to a position and pick up from there.
	//
	//Deliberately not plm_seek - see plm_seek_bytes. That one searches by the stream's own
	//timestamps, and these streams do not keep honest ones, so it landed nowhere and the bar
	//sprang straight back to where it had been.
	//
	//The disc is the clock instead, the same one the length comes from: an item is a run of
	//sectors handed over at a fixed rate, so a fraction of the way through it is that same
	//fraction of the way through the bytes gathered off it. `duration` is the length the
	//caller already measured, not a second opinion worked out here, so the position, the
	//length and the seek cannot disagree.
	//
	//Everything already decoded belongs to where the stream used to be, so it goes.
	bool Seek(double seconds, double duration)
	{
		if(!_plm || duration <= 0 || _stream.empty()) {
			return false;
		}

		//Land ahead of the wanted position and decode up to it without showing anything.
		//
		//A jump cannot land on a picture that stands on its own. The library picks up at the
		//next picture of any kind, throws away only the first, and trusts what follows - so
		//if the next picture is one that is meant to be drawn on top of another, it is drawn
		//on top of whatever was left in the buffer, and a frame or two of the previous
		//position shows through. That was the flicker after a drag.
		//
		//It is also why a stretch started part way into a track began late: decoding could
		//only really begin at the next self-contained picture, which on these streams is up
		//to half a second further on. Running up from behind the position fixes both - by
		//the time the wanted point is reached a self-contained picture has gone by, the
		//pictures after it stand on something real, and the one on screen is the one that
		//belongs at the position asked for.
		//Aimed past the mark by what a jump into this stream was measured to cost - see
		//Calibrate. Zero until that has been worked out, and while working it out.
		seconds += _seekLate;
		if(seconds > duration) {
			seconds = duration;
		}

		double landing = seconds - RunUpSeconds;
		if(landing < 0) {
			landing = 0;
		}

		double fraction = landing / duration;
		fraction = fraction < 0 ? 0 : (fraction > 1 ? 1 : fraction);

		//Never onto the last few bytes, where there is no whole packet left to find
		size_t offset = (size_t)(fraction * (double)_stream.size());
		if(offset + 256 >= _stream.size()) {
			offset = _stream.size() > 256 ? _stream.size() - 256 : 0;
		}

		plm_seek_bytes(_plm, offset);
		_timeBase = fraction * duration;
		_samples.clear();
		_hasPicture = false;

		//The sound is normally decoded ahead of the picture, and must not be while running up:
		//everything gathered on the way is thrown away at the end, and anything decoded past
		//the point being sought to would go with it - the library will not produce it a second
		//time. That left the start of every jumped-to stretch with a quarter second of nothing
		//behind it, and a quarter of its frames with no sound to hand over at all.
		plm_set_audio_lead_time(_plm, 0);

		//Carried forward in the same steps a frame of play uses, so nothing here behaves
		//differently from ordinary decoding. Bounded: a stream that will not advance - the
		//end, or damage - stops this rather than spinning on it.
		uint32_t steps = (uint32_t)(RunUpSeconds / StepSeconds) + 2;
		while(GetTime() < seconds && steps-- > 0 && !HasEnded()) {
			plm_decode(_plm, StepSeconds);
		}

		//The picture now standing is the one wanted, so it stays. The sound gathered getting
		//here belongs before the position and does not - and with no lead it stops where the
		//picture does, so none of what comes next is lost with it.
		_samples.clear();
		plm_set_audio_lead_time(_plm, _audioLead);
		return true;
	}
	bool HasEnded() { return !_plm || plm_has_ended(_plm); }
	bool HasPicture() { return _hasPicture; }
	const vector<uint32_t>& GetPicture() { return _picture; }
	vector<int16_t>& GetSamples() { return _samples; }

	//How far ahead of the picture to decode the sound. The library keeps a running store for
	//whoever is playing it, which is what the caller here needs: the sound leaves in pieces
	//that do not line up with the pieces it arrives in.
	void SetAudioLead(double seconds)
	{
		_audioLead = seconds;
		if(_plm) {
			plm_set_audio_lead_time(_plm, seconds);
		}
	}

	//Let the library carry the stream forward by that much of its own time. It owns the
	//pacing between the picture and the sound: they come out of one stream and asking for
	//each of them separately, as this used to, leaves them to drift apart - which is heard
	//long before it is seen.
	void Advance(double seconds)
	{
		if(_plm) {
			plm_decode(_plm, seconds);
		}
	}


};
