#pragma once
#include "pch.h"
#include <atomic>
#include <mutex>

//Pages for the built-in YuXing host's browser (YuxingHost). Only the UI can reach the network: the
//host asks it with a YuxingWebRequest notification, and the UI answers through the
//YuxingWebResponse export, from a thread of its own, into here; the host takes the page on the
//emulation thread. One page is in flight at a time, matched by the request's number.
class YuxingWebBridge
{
private:
	inline static std::mutex _lock;
	inline static std::atomic<bool> _ready = false;
	inline static uint32_t _id = 0;
	inline static vector<uint8_t> _data;

public:
	static void Respond(uint32_t id, const uint8_t* data, uint32_t length)
	{
		std::lock_guard<std::mutex> lock(_lock);
		_id = id;
		_data.assign(data, data + length);
		_ready = true;
	}

	//Checked every cycle while a page is awaited, so the lock is only taken once one is there
	static bool Take(uint32_t id, vector<uint8_t>& page)
	{
		if(!_ready) {
			return false;
		}
		std::lock_guard<std::mutex> lock(_lock);
		if(!_ready || _id != id) {
			return false;
		}
		page = std::move(_data);
		_data.clear();
		_ready = false;
		return true;
	}
};
