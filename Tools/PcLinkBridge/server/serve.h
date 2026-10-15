#pragma once
//The PC side of the PC link: the loop that hands the BBK's bytes to the emulator's PC link server,
//BbkPcLink.h, and its answers back - the same code that serves PCLINK inside MesenCE - over a wire:
//PcLinkBridge on USB serial (here), or a parallel port (lpt.h).
#include "pch.h"
#include <chrono>
#include <ctime>
#include <stdexcept>
#include "../../../Core/NES/Mappers/Bbk/BbkPcLink.h"

//The conversation broke off: the bridge restarted, or a side stopped in the middle of a request
struct LinkLost : std::runtime_error
{
	using std::runtime_error::runtime_error;
};

//The bytes to and from the BBK. The server knows which way the next bytes go - the cable does not
//say - so it sends what it has, then says how many bytes it is to take.
struct Wire
{
	int Patience = 5000;   //ms a side may stall in the middle of a request
	uint64_t Sent = 0;
	uint64_t Received = 0;

	virtual ~Wire() {}
	virtual void Send(const vector<uint8_t>& data) = 0;
	virtual void Grant(size_t n) {}
	//A byte the BBK sent: waits as long as it takes when timeoutMs < 0, else a stall throws.
	//-1 when stopped.
	virtual int Receive(int timeoutMs) = 0;
	//Starts the wire over after a conversation broke off; false when it does not answer
	virtual bool Restart() = 0;
};

struct Port
{
	virtual ~Port() {}
	virtual void Write(const uint8_t* data, size_t len) = 0;
	virtual int Read(int timeoutMs) = 0;   //a byte, or -1 after the timeout
};

//PcLinkBridge, on a serial port (its protocol: ../firmware/PcLinkBridge/bridge.h)
class Bridge : public Wire
{
private:
	using Clock = std::chrono::steady_clock;

	Port& _port;
	std::atomic<bool>& _stop;
	size_t _window = 1024;
	size_t _queued = 0;   //bytes sent to the bridge that no 'K' has counted off yet
	std::deque<uint8_t> _rx;

	//One frame from the bridge; false after the timeout
	bool Frame(int timeoutMs)
	{
		int c = _port.Read(timeoutMs);
		if(c < 0) {
			return false;
		}
		if(c == 'K') {
			_queued = _queued >= 64 ? _queued - 64 : 0;
		} else if(c == 'D') {
			int b = _port.Read(Patience);
			if(b < 0) {
				throw LinkLost("the bridge stopped in the middle of a frame");
			}
			_rx.push_back((uint8_t)b);
			Received++;
		} else if(c == 'H') {
			throw LinkLost("the bridge restarted");
		} else {
			char msg[64];
			snprintf(msg, sizeof(msg), "the bridge sent $%02X", c);
			throw LinkLost(msg);
		}
		return true;
	}

public:
	int Version = 0;

	Bridge(Port& port, std::atomic<bool>& stop) : _port(port), _stop(stop) {}

	//Waits for the bridge's hello, dropping whatever comes before it
	bool AwaitHello(int timeoutMs)
	{
		static const char magic[] = "HBBK";
		int at = 0;
		auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
		while(Clock::now() < end && !_stop) {
			int c = _port.Read(100);
			if(c < 0) {
				continue;
			}
			at = c == magic[at] ? at + 1 : c == 'H' ? 1 : 0;
			if(at == 4) {
				int version = _port.Read(500);
				int ring = _port.Read(500);
				if(version < 0 || ring <= 0) {
					return false;
				}
				Version = version;
				_window = (size_t)ring * 64;
				_queued = 0;
				_rx.clear();
				return true;
			}
		}
		return false;
	}

	//Starts the bridge over: what it has queued is dropped, a byte in the middle is cut short
	bool Start(int timeoutMs)
	{
		uint8_t i = 'I';
		_port.Write(&i, 1);
		return AwaitHello(timeoutMs);
	}

	bool Restart() override { return Start(2000); }

	void Send(const vector<uint8_t>& data) override
	{
		//A 'K' counts off 64 bytes at a time, so up to 63 taken bytes may not be counted off yet: a
		//frame leaves that much of the window over, or it might wait for a 'K' that never comes
		size_t most = std::min<size_t>(255, _window - 64);
		size_t at = 0;
		while(at < data.size()) {
			size_t n = std::min<size_t>(most, data.size() - at);
			while(_queued + n > _window) {
				if(!Frame(Patience)) {
					throw LinkLost("the BBK stopped taking bytes");
				}
				if(!_rx.empty()) {
					throw LinkLost("the BBK sent while it should be receiving");
				}
			}
			vector<uint8_t> frame;
			frame.push_back('S');
			frame.push_back((uint8_t)n);
			frame.insert(frame.end(), data.begin() + at, data.begin() + at + n);
			_port.Write(frame.data(), frame.size());
			_queued += n;
			Sent += n;
			at += n;
		}
	}

	//Lets the bridge take this many more bytes from the BBK
	void Grant(size_t n) override
	{
		while(n) {
			size_t k = std::min<size_t>(n, 0xFFFF);
			uint8_t frame[3] = { 'R', (uint8_t)(k >> 8), (uint8_t)k };
			_port.Write(frame, 3);
			n -= k;
		}
	}

	int Receive(int timeoutMs) override
	{
		auto last = Clock::now();
		while(_rx.empty()) {
			if(_stop) {
				return -1;
			}
			if(Frame(200)) {
				last = Clock::now();
			} else if(timeoutMs >= 0 && Clock::now() - last > std::chrono::milliseconds(timeoutMs)) {
				throw LinkLost("the BBK stopped sending");
			}
		}
		uint8_t b = _rx.front();
		_rx.pop_front();
		return b;
	}
};

inline string LogLine(BbkPcLink& link, uint64_t traffic)
{
	static const char* names[25] = {
		nullptr, "open", nullptr, nullptr, nullptr, "close", "dir", nullptr, "create", nullptr, "delete",
		nullptr, nullptr, "rename", nullptr, nullptr, "cd", "md", "rd", nullptr, "open", "create", "dir",
		"delete", "attrib"
	};
	uint8_t cmd = link.Command();
	if(cmd >= 25 || !names[cmd]) {
		return string();
	}
	string status;
	switch(link.Result()) {
		case 0x00: status = "ok"; break;
		case 0x02: status = "failed"; break;
		case 0x03: status = "no such path"; break;
		case 0x05: status = "denied"; break;
		case 0x12: status = "nothing found"; break;
		case 0x1D: status = "write fault"; break;
		case 0x1E: status = "read fault"; break;
		default: {
			char s[16];
			snprintf(s, sizeof(s), "error $%02X", link.Result());
			status = s;
		}
	}
	if(cmd == 5) {
		char s[48];
		snprintf(s, sizeof(s), "%s (%llu bytes over the link)", status.c_str(), (unsigned long long)traffic);
		status = s;
	}
	time_t t = time(nullptr);
	tm now = {};
#ifdef _WIN32
	localtime_s(&now, &t);
#else
	localtime_r(&t, &now);
#endif
	char line[256];
	snprintf(line, sizeof(line), "%02d:%02d:%02d  %-7s %-24s %s", now.tm_hour, now.tm_min, now.tm_sec, names[cmd], link.Name().c_str(), status.c_str());
	return line;
}

//Serves the BBK until stopped. A conversation that breaks off is dropped and the wire started
//over; the open files and the current directory stay.
inline void Serve(Wire& wire, BbkPcLink& link, std::atomic<bool>& stop, const std::function<void(const string&)>& log)
{
	uint32_t logged = link.Requests();
	uint64_t mark = 0;
	while(!stop) {
		try {
			for(;;) {
				vector<uint8_t> out;
				uint8_t b;
				while(link.TakeByte(b)) {
					out.push_back(b);
				}
				wire.Send(out);

				size_t n = link.Expecting();
				bool idle = n == 17;
				if(idle && link.Requests() != logged) {
					logged = link.Requests();
					uint8_t cmd = link.Command();
					string line = LogLine(link, wire.Sent + wire.Received - mark);
					if(cmd == 1 || cmd == 5 || cmd == 8 || cmd == 20 || cmd == 21) {
						mark = wire.Sent + wire.Received;
					}
					if(!line.empty()) {
						log(line);
					}
				}

				wire.Grant(n);
				for(size_t i = 0; i < n; i++) {
					int c = wire.Receive(idle && i == 0 ? -1 : wire.Patience);
					if(c < 0) {
						return;
					}
					link.ReceiveByte((uint8_t)c);
				}
			}
		} catch(const LinkLost& e) {
			if(stop) {
				return;
			}
			log(string("! ") + e.what() + " - starting over");
			link.Resync();
			while(!stop && !wire.Restart()) {
				log("! no answer");
			}
		}
	}
}
