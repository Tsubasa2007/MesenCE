#pragma once
#include "pch.h"
#include <iomanip>
#include <sstream>

class StringUtilities
{
public:
	static vector<string> Split(string input, char delimiter)
	{
		vector<string> result;
		size_t index = 0;
		size_t lastIndex = 0;
		while((index = input.find(delimiter, index)) != string::npos) {
			result.push_back(input.substr(lastIndex, index - lastIndex));
			index++;
			lastIndex = index;
		}
		result.push_back(input.substr(lastIndex));
		return result;
	}

	static string TrimLeft(string str)
	{
		size_t startIndex = str.find_first_not_of("\t ");
		if(startIndex == string::npos) {
			return "";
		} else if(startIndex > 0) {
			return str.substr(startIndex);
		}
		return str;
	}

	static string TrimRight(string str)
	{
		size_t endIndex = str.find_last_not_of("\t\r\n ");
		if(endIndex == string::npos) {
			return "";
		}
		return str.substr(0, endIndex + 1);
	}

	static string Trim(string str)
	{
		return TrimLeft(TrimRight(str));
	}

	static string ToUpper(string str)
	{
		std::transform(str.begin(), str.end(), str.begin(), ::toupper);
		return str;
	}

	static string ToLower(string str)
	{
		std::transform(str.begin(), str.end(), str.begin(), ::tolower);
		return str;
	}

	//Whether a byte string is well-formed UTF-8. Paths are UTF-8 by contract here and
	//std::filesystem::u8path throws on anything else, so text that came from a file's
	//contents rather than from the filesystem has to be checked before it is used as one.
	static bool IsValidUtf8(const string& str)
	{
		for(size_t i = 0; i < str.size(); ) {
			uint8_t c = (uint8_t)str[i];
			int extra;
			uint32_t code;
			if(c < 0x80) { i++; continue; }
			else if((c & 0xE0) == 0xC0) { extra = 1; code = c & 0x1F; }
			else if((c & 0xF0) == 0xE0) { extra = 2; code = c & 0x0F; }
			else if((c & 0xF8) == 0xF0) { extra = 3; code = c & 0x07; }
			else { return false; }

			if(i + extra >= str.size()) { return false; }
			for(int j = 1; j <= extra; j++) {
				uint8_t cc = (uint8_t)str[i + j];
				if((cc & 0xC0) != 0x80) { return false; }
				code = (code << 6) | (cc & 0x3F);
			}

			//Overlong forms, surrogates and anything past the last code point are all
			//ill-formed, and all of them make u8path throw just as a stray byte does
			if(extra == 1 && code < 0x80) { return false; }
			if(extra == 2 && code < 0x800) { return false; }
			if(extra == 3 && code < 0x10000) { return false; }
			if(code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) { return false; }
			i += extra + 1;
		}
		return true;
	}

	static void CopyToBuffer(string str, char* outBuffer, uint32_t maxSize)
	{
		memcpy(outBuffer, str.c_str(), std::min<uint32_t>((uint32_t)str.size(), maxSize));
	}

	static bool StartsWith(const string& str, const char* content)
	{
		size_t length = strlen(content);
		if(str.size() < length) {
			return false;
		}

		for(size_t i = 0; i < length; i++) {
			if(str[i] != content[i]) {
				return false;
			}
		}
		return true;
	}

	static bool EndsWith(const string& str, const char* content)
	{
		size_t length = strlen(content);
		if(str.size() < length) {
			return false;
		}

		size_t startPos = str.size() - length;
		for(size_t i = startPos; i < str.size(); i++) {
			if(str[i] != content[i - startPos]) {
				return false;
			}
		}

		return true;
	}

	static bool Contains(const string& str, const char* content)
	{
		size_t length = strlen(content);
		return std::search(str.begin(), str.end(), content, content + length) != str.end();
	}

	static string GetString(char* src, uint32_t maxLen)
	{
		return GetString((uint8_t*)src, maxLen);
	}

	static string GetString(uint8_t* src, int maxLen)
	{
		for(int i = 0; i < maxLen; i++) {
			if(src[i] == 0) {
				return string(src, src + i);
			}
		}
		return string(src, src + maxLen);
	}

	static string SizeToString(int32_t size)
	{
		if((size & 0x3FF) == 0) {
			// Size is a multiple of 1 KiB, so print it in that form.
			return std::to_string(size / 1024) + " KB";
		} else {
			// Size is not a multiple of 1 KiB, so just print the bytes.
			return std::to_string(size) + " bytes";
		}
	}

	static string ToString(double value, uint32_t precision)
	{
		std::ostringstream stream;
		stream << std::fixed << std::setprecision(precision) << value;
		return stream.str();
	}
};
