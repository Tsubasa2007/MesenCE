#pragma once
#include "pch.h"
#include "NES/Mappers/CdImageFile.h"
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

	plm_t* _plm = nullptr;
	plm_buffer_t* _buffer = nullptr;

	//Filled by the library as it decodes: the newest picture, and the sound behind it
	vector<uint32_t> _picture;
	vector<int16_t> _samples;
	bool _hasPicture = false;

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
	}

	//Gather one stretch of the disc into a stream the demuxer can read.
	//
	//sectors is counted from lba the way the drive counts it - an item's whole allocation,
	//padding included - and the padding is dropped here.
	bool Open(CdImageFile& image, uint32_t lba, uint32_t sectors)
	{
		Close();
		if(!image.IsOpen()) {
			return false;
		}

		_stream.reserve((size_t)sectors * Form2PayloadSize);
		for(uint32_t i = 0; i < sectors; i++) {
			uint8_t submode = image.SubmodeAt(lba + i);
			//Bits 1 to 3 are video, audio and data; a sector claiming none of them is padding
			if((submode & 0x0E) == 0) {
				continue;
			}

			uint32_t size = (submode & 0x20) ? Form2PayloadSize : Form1PayloadSize;
			vector<uint8_t> payload = image.ReadRawPayload(lba + i, size);
			if(payload.empty()) {
				break;
			}
			_stream.insert(_stream.end(), payload.begin(), payload.end());
		}

		if(_stream.empty()) {
			return false;
		}

		_buffer = plm_buffer_create_with_memory(_stream.data(), (size_t)_stream.size(), 0);
		if(!_buffer) {
			Close();
			return false;
		}

		_plm = plm_create_with_buffer(_buffer, 1);
		if(!_plm) {
			Close();
			return false;
		}

		//What the system header claims is not what these discs carry - the library says as
		//much, and names Video CD as the case where the header cannot be believed. Probing
		//reads far enough in to find the streams that are really there.
		if(!plm_probe(_plm, ProbeSize) || plm_get_num_video_streams(_plm) == 0) {
			Close();
			return false;
		}

		plm_set_video_decode_callback(_plm, OnVideo, this);
		plm_set_audio_decode_callback(_plm, OnAudio, this);

		_width = (uint32_t)plm_get_width(_plm);
		_height = (uint32_t)plm_get_height(_plm);
		return _width > 0 && _height > 0;
	}

	bool IsOpen() { return _plm != nullptr; }
	uint32_t GetWidth() { return _width; }
	uint32_t GetHeight() { return _height; }
	double GetDuration() { return _plm ? plm_get_duration(_plm) : 0; }
	double GetFrameRate() { return _plm ? plm_get_framerate(_plm) : 0; }
	uint32_t GetStreamSize() { return (uint32_t)_stream.size(); }


	int GetSampleRate() { return _plm ? plm_get_samplerate(_plm) : 0; }
	bool HasEnded() { return !_plm || plm_has_ended(_plm); }
	bool HasPicture() { return _hasPicture; }
	const vector<uint32_t>& GetPicture() { return _picture; }
	vector<int16_t>& GetSamples() { return _samples; }

	//How far ahead of the picture to decode the sound. The library keeps a running store for
	//whoever is playing it, which is what the caller here needs: the sound leaves in pieces
	//that do not line up with the pieces it arrives in.
	void SetAudioLead(double seconds)
	{
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
