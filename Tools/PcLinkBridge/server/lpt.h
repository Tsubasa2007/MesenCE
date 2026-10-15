#pragma once
//A parallel port as the wire: the handshakes done here, on the port's pins, as PCSVER.EXE did
//($02C2 sends a byte, $0377 takes one). The port is read and written through PortIo - on Windows
//the InpOut driver (main.cpp).
#include "serve.h"
#include <thread>

struct PortIo
{
	virtual ~PortIo() {}
	virtual uint8_t In(uint16_t port) = 0;
	virtual void Out(uint16_t port, uint8_t value) = 0;
};

class LptWire : public Wire
{
private:
	using Clock = std::chrono::steady_clock;

	PortIo& _io;
	uint16_t _base;
	std::atomic<bool>& _stop;

	//The BBK's lines, as the cable brings them to the status register: its nibble in bits 3-6, its
	//handshake inverted in bit 7 (Busy) - as bridge.h has them: the nibble in bits 0-3, the
	//handshake in bit 4
	uint8_t Lines()
	{
		uint8_t s = _io.In((uint16_t)(_base + 1));
		return (uint8_t)(((s >> 3) & 0x0F) | ((s & 0x80) ? 0x00 : 0x10));
	}

	//Ours: the nibble in data bits 0-3, the handshake in bit 4
	void Put(uint8_t v) { _io.Out(_base, (uint8_t)(v & 0x1F)); }

	//Waits for the BBK's handshake to be up (or down), the same in two reads, and returns its lines.
	//Polls flat out at first, then a millisecond at a time - the BBK waits for us, so a slow answer
	//only costs time. A stall throws after timeoutMs (< 0: never); stopping throws too.
	uint8_t WaitLine(bool up, int timeoutMs)
	{
		auto start = Clock::now();
		for(unsigned spins = 1;; spins++) {
			uint8_t a = Lines();
			if(((a & 0x10) != 0) == up && Lines() == a) {
				return a;
			}
			if(spins % 256 == 0) {
				if(_stop) {
					throw LinkLost("stopped");
				}
				auto waited = Clock::now() - start;
				if(timeoutMs >= 0 && waited > std::chrono::milliseconds(timeoutMs)) {
					throw LinkLost(up ? "the BBK stopped (its handshake stays down)" : "the BBK stopped (its handshake stays up)");
				}
				if(waited > std::chrono::milliseconds(20)) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			}
		}
	}

public:
	LptWire(PortIo& io, uint16_t base, std::atomic<bool>& stop) : _io(io), _base(base), _stop(stop) {}

	//Sets the data lines to output and checks they read back, as a standard (SPP) port's do: false
	//when there is no port here, or it is in ECP or EPP mode
	bool Check()
	{
		uint16_t control = (uint16_t)(_base + 2);
		_io.Out(control, (uint8_t)(_io.In(control) & ~0x20));
		static const uint8_t values[] = { 0x15, 0x0A, 0x00 };
		bool ok = true;
		for(uint8_t v : values) {
			_io.Out(_base, v);
			ok &= _io.In(_base) == v;
		}
		return ok;
	}

	//What the port's lines read now (for the start-up report)
	uint8_t BbkLines() { return Lines(); }

	bool Restart() override
	{
		Put(0);
		return true;
	}

	void Send(const vector<uint8_t>& data) override
	{
		for(uint8_t b : data) {
			WaitLine(true, Patience);
			Put(b & 0x0F);
			Put((b & 0x0F) | 0x10);
			WaitLine(false, Patience);
			Put((b >> 4) | 0x10);
			Put(b >> 4);
			Sent++;
		}
	}

	int Receive(int timeoutMs) override
	{
		uint8_t low;
		try {
			low = WaitLine(true, timeoutMs) & 0x0F;
		} catch(const LinkLost&) {
			if(_stop) {
				return -1;
			}
			throw;
		}
		Put(0x10);
		uint8_t high = WaitLine(false, Patience) & 0x0F;
		Put(0x00);
		Received++;
		return low | (high << 4);
	}
};
