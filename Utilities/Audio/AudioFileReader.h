#pragma once
#include "pch.h"
#include <functional>
#include <unordered_set>

//Decodes a whole compressed audio file (an MP3, an AAC/M4A, ...) front to back, handing out
//16-bit stereo frames as it goes. Windows decodes through Media Foundation, which covers the
//common formats; OGG goes through stb_vorbis everywhere, and elsewhere it and plain PCM WAV
//are all there is.
class AudioFileReader
{
public:
	//Called once, with the rate the frames come at, before any data. Returning false stops.
	using FormatCallback = std::function<bool(uint32_t sampleRate)>;
	//Called with each block of interleaved stereo frames. Returning false stops.
	using DataCallback = std::function<bool(const int16_t* frames, uint32_t frameCount)>;

	static const std::unordered_set<string>& GetExtensions();
	static bool IsAudioFile(const string& path);

	//False with a message in error when the file could not be opened or decoded. Stopping
	//from a callback is not a failure.
	static bool Decode(const string& path, const FormatCallback& onFormat, const DataCallback& onData, string& error);

private:
	static bool DecodeVorbis(const string& path, const FormatCallback& onFormat, const DataCallback& onData, string& error);
	static bool DecodeWav(const string& path, const FormatCallback& onFormat, const DataCallback& onData, string& error);
#ifdef _WIN32
	static bool DecodeMediaFoundation(const string& path, const FormatCallback& onFormat, const DataCallback& onData, string& error);
#endif
};
