#include "pch.h"
#include "Utilities/Audio/AudioFileReader.h"
#include "Utilities/Audio/stb_vorbis.h"
#include "Utilities/UTF8Util.h"
#include "Utilities/StringUtilities.h"

#ifdef _WIN32
	#include <windows.h>
	#include <mfapi.h>
	#include <mfidl.h>
	#include <mfreadwrite.h>
	#pragma comment(lib, "mfplat.lib")
	#pragma comment(lib, "mfreadwrite.lib")
	#pragma comment(lib, "mfuuid.lib")
#endif

const std::unordered_set<string>& AudioFileReader::GetExtensions()
{
#ifdef _WIN32
	static const std::unordered_set<string> extensions = { ".mp3", ".m4a", ".aac", ".wav", ".wma", ".flac", ".ogg" };
#else
	static const std::unordered_set<string> extensions = { ".wav", ".ogg" };
#endif
	return extensions;
}

static string GetLowerExtension(const string& path)
{
	size_t dot = path.find_last_of('.');
	size_t sep = path.find_last_of("/\\");
	if(dot == string::npos || (sep != string::npos && dot < sep)) {
		return "";
	}
	string ext = path.substr(dot);
	std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return (char)::tolower((uint8_t)c); });
	return ext;
}

bool AudioFileReader::IsAudioFile(const string& path)
{
	return GetExtensions().count(GetLowerExtension(path)) > 0;
}

bool AudioFileReader::Decode(const string& path, const FormatCallback& onFormat, const DataCallback& onData, string& error)
{
	string ext = GetLowerExtension(path);
	if(ext == ".ogg") {
		return DecodeVorbis(path, onFormat, onData, error);
	}
#ifdef _WIN32
	return DecodeMediaFoundation(path, onFormat, onData, error);
#else
	if(ext == ".wav") {
		return DecodeWav(path, onFormat, onData, error);
	}
	error = "this kind of audio file can only be read on Windows";
	return false;
#endif
}

static bool ReadWholeFile(const string& path, vector<uint8_t>& data)
{
	utf8::ifstream file(path, std::ios::in | std::ios::binary);
	if(!file) {
		return false;
	}
	file.seekg(0, std::ios::end);
	data.resize((size_t)file.tellg());
	file.seekg(0, std::ios::beg);
	file.read((char*)data.data(), data.size());
	return (bool)file;
}

bool AudioFileReader::DecodeVorbis(const string& path, const FormatCallback& onFormat, const DataCallback& onData, string& error)
{
	//From memory rather than by name: stb_vorbis opens a name with fopen, which cannot reach a
	//path outside the system code page
	vector<uint8_t> data;
	if(!ReadWholeFile(path, data)) {
		error = "could not read the file";
		return false;
	}
	int vorbisError = 0;
	stb_vorbis* vorbis = stb_vorbis_open_memory(data.data(), (int)data.size(), &vorbisError, nullptr);
	if(!vorbis) {
		error = "not an Ogg Vorbis file (error " + std::to_string(vorbisError) + ")";
		return false;
	}
	if(onFormat(stb_vorbis_get_info(vorbis).sample_rate)) {
		vector<int16_t> buffer(4096 * 2);
		int frames;
		while((frames = stb_vorbis_get_frame_short_interleaved(vorbis, 2, buffer.data(), (int)buffer.size())) > 0) {
			if(!onData(buffer.data(), (uint32_t)frames)) {
				break;
			}
		}
	}
	stb_vorbis_close(vorbis);
	return true;
}

bool AudioFileReader::DecodeWav(const string& path, const FormatCallback& onFormat, const DataCallback& onData, string& error)
{
	vector<uint8_t> data;
	if(!ReadWholeFile(path, data) || data.size() < 12 || memcmp(data.data(), "RIFF", 4) != 0 || memcmp(data.data() + 8, "WAVE", 4) != 0) {
		error = "not a WAV file";
		return false;
	}
	auto read16 = [&data](size_t pos) { return (uint16_t)(data[pos] | (data[pos + 1] << 8)); };
	auto read32 = [&data](size_t pos) { return (uint32_t)(data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | ((uint32_t)data[pos + 3] << 24)); };

	uint16_t channels = 0;
	uint16_t bits = 0;
	uint32_t rate = 0;
	size_t pos = 12;
	while(pos + 8 <= data.size()) {
		uint32_t size = read32(pos + 4);
		size_t body = pos + 8;
		if(memcmp(data.data() + pos, "fmt ", 4) == 0 && size >= 16 && body + 16 <= data.size()) {
			if(read16(body) != 1) {
				error = "only uncompressed WAV files can be read here";
				return false;
			}
			channels = read16(body + 2);
			rate = read32(body + 4);
			bits = read16(body + 14);
		} else if(memcmp(data.data() + pos, "data", 4) == 0) {
			if(bits != 16 || channels < 1 || channels > 2 || rate == 0) {
				error = "only 16-bit mono or stereo WAV files can be read here";
				return false;
			}
			size_t end = (std::min)(data.size(), body + (size_t)size);
			if(!onFormat(rate)) {
				return true;
			}
			vector<int16_t> buffer;
			const size_t blockFrames = 4096;
			for(size_t p = body; p + channels * 2 <= end;) {
				buffer.clear();
				while(buffer.size() < blockFrames * 2 && p + channels * 2 <= end) {
					int16_t left = (int16_t)read16(p);
					int16_t right = channels == 2 ? (int16_t)read16(p + 2) : left;
					buffer.push_back(left);
					buffer.push_back(right);
					p += channels * 2;
				}
				if(!onData(buffer.data(), (uint32_t)(buffer.size() / 2))) {
					break;
				}
			}
			return true;
		}
		pos = body + size + (size & 1);
	}
	error = "the WAV file has no audio data";
	return false;
}

#ifdef _WIN32
bool AudioFileReader::DecodeMediaFoundation(const string& path, const FormatCallback& onFormat, const DataCallback& onData, string& error)
{
	HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if(FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
		error = "Media Foundation is not available";
		if(SUCCEEDED(comResult)) {
			CoUninitialize();
		}
		return false;
	}

	bool result = false;
	IMFSourceReader* reader = nullptr;
	IMFMediaType* requested = nullptr;
	IMFMediaType* actual = nullptr;
	do {
		if(FAILED(MFCreateSourceReaderFromURL(utf8::utf8::decode(path).c_str(), nullptr, &reader))) {
			error = "the file could not be opened as audio";
			break;
		}
		reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
		reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

		//16-bit at the file's own rate and channel count: the decoder converts, nothing is resampled
		if(FAILED(MFCreateMediaType(&requested))) {
			break;
		}
		requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		requested->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
		requested->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
		if(FAILED(reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, requested))) {
			error = "the file's audio cannot be decoded";
			break;
		}
		if(FAILED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual))) {
			break;
		}
		UINT32 rate = 0, channels = 0, bits = 0;
		actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
		actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
		actual->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
		if(rate == 0 || bits != 16 || (channels != 1 && channels != 2)) {
			error = "the decoder did not give 16-bit audio";
			break;
		}

		result = true;
		if(!onFormat(rate)) {
			break;
		}

		vector<int16_t> stereo;
		while(true) {
			DWORD flags = 0;
			IMFSample* sample = nullptr;
			if(FAILED(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample))) {
				error = "decoding stopped partway through the file";
				result = false;
				break;
			}
			if(flags & MF_SOURCE_READERF_ENDOFSTREAM) {
				if(sample) {
					sample->Release();
				}
				break;
			}
			if(!sample) {
				continue;
			}

			bool keepGoing = true;
			IMFMediaBuffer* buffer = nullptr;
			if(SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
				BYTE* bytes = nullptr;
				DWORD length = 0;
				if(SUCCEEDED(buffer->Lock(&bytes, nullptr, &length))) {
					const int16_t* pcm = (const int16_t*)bytes;
					uint32_t frames = length / (2 * channels);
					if(channels == 2) {
						keepGoing = onData(pcm, frames);
					} else {
						stereo.resize(frames * 2);
						for(uint32_t i = 0; i < frames; i++) {
							stereo[i * 2] = stereo[i * 2 + 1] = pcm[i];
						}
						keepGoing = onData(stereo.data(), frames);
					}
					buffer->Unlock();
				}
				buffer->Release();
			}
			sample->Release();
			if(!keepGoing) {
				break;
			}
		}
	} while(false);

	if(actual) {
		actual->Release();
	}
	if(requested) {
		requested->Release();
	}
	if(reader) {
		reader->Release();
	}
	MFShutdown();
	if(SUCCEEDED(comResult)) {
		CoUninitialize();
	}
	return result;
}
#endif
