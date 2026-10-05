#pragma once
#include "pch.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/KeyManager.h"
#include "Shared/EmuSettings.h"
#include "Shared/Emulator.h"
#include "Utilities/Serializer.h"

//A PS/2 mouse wired straight to the controller port lines, with the guest bit-banging the PS/2
//protocol itself and timing it against the clock the mouse generates. Carried by a Subor-type
//expansion card whose driver talks to it the way a PC talks to a PS/2 mouse: it sends READ_DATA
//($EB) whenever it wants a position, checks for the $FA acknowledge, and takes a 3-byte packet:
//the PS/2 status byte (bit 3 set, buttons in bits 0-2, signs in bits 4-5), then Y counting
//upwards, then X. Y comes first and the left and right buttons trade bits compared with a PC's
//PS/2 mouse - which is how its driver reads it, and how BbkMouse's packet is laid out too.
//
//This is the same family of device as BbkMouse (an EM84502-style mouse on $4016/$4017), but
//BbkMouse reproduces the BBK BIOS's own read pattern by counting reads, which only works for a
//driver that reads in exactly that pattern. This driver waits on clock edges in timing loops
//instead, so this device models the two lines in CPU time.
//
//The wiring, as the guest's driver uses it:
//  $4016 bit 2 = 0   the host drives the data line with bit 0 (0 pulls it low), and $4017 bit 0
//                    reads the clock line
//  $4016 bit 2 = 1   the host lets go of the data line, and $4017 bit 0 reads the data line
//  $4017 bit 0       reads the selected line inverted (1 = low), as controller inputs are
//The host never drives the clock. The cut-down Subor keyboard shares the port and only ever
//writes values with bit 2 set ($04/$05/$06), so its scan leaves the mouse alone.
//
//Host to mouse: the host pulls data low (the start bit); the mouse clocks 12 pulses, sampling
//on each rising edge the start bit, 8 data bits LSB first, odd parity and the stop bit, then
//holds data low through the 12th pulse as the line-level acknowledge.
//Mouse to host: 11-bit frames - start, 8 data bits LSB first, odd parity, stop - with data set
//while the clock is high and the host reading on the falling edge.
//
//The guest's driver sends every command twice in a row, the second as soon as the first has been
//acknowledged, and only listens after the second. A real mouse would have begun answering the
//first and been interrupted by the host taking the data line; here a request from the host
//simply cancels any reply still queued.
class Ps2Mouse : public BaseControlDevice
{
private:
	//CPU cycles. The clock runs at ~11 kHz, inside the PS/2 range; the guest's edge-polling loops
	//take ~14 cycles a pass and act on an edge within ~45, so each half of the clock has room.
	static constexpr uint32_t HalfPeriod = 80;
	static constexpr uint32_t Period = HalfPeriod * 2;
	static constexpr uint32_t RequestDelay = 40;    //from the host pulling data low to the first clock
	static constexpr uint32_t ReplyDelay = 400;     //from the acknowledge to the first reply byte
	static constexpr uint32_t ByteGap = 200;        //between reply bytes
	static constexpr uint32_t FrameCycles = Period * 11 + ByteGap;

	static constexpr uint8_t CmdReset = 0xFF;
	static constexpr uint8_t CmdReadData = 0xEB;
	static constexpr uint8_t CmdGetDeviceId = 0xF2;
	static constexpr uint8_t Ack = 0xFA;

	enum class State : uint8_t { Idle, Receive, Transmit };

	State _state = State::Idle;
	uint8_t _out = 0x05;           //the last $4016 write
	uint64_t _requestAt = 0;       //Idle with the host holding data low: when the mouse starts clocking
	bool _requestPending = false;

	uint64_t _rxStart = 0;         //time of the first falling edge
	uint8_t _rxNext = 1;           //the next rising edge to sample, 1-11
	uint16_t _rxBits = 0;

	uint64_t _txStart = 0;
	uint8_t _txLen = 0;
	uint8_t _txData[4] = {};

	//Movement has to survive until the guest asks for it, while the state buffer is cleared on
	//every poll - so each poll's delta is folded in here in OnAfterSetState(), after movie
	//playback has had its say. _seenX/_seenY are the coordinates already folded in, so that a
	//later change to the state within the same frame (a script's emu.setInput) is picked up
	//once and only once.
	int32_t _accumX = 0;
	int32_t _accumY = 0;
	int32_t _seenX = 0;
	int32_t _seenY = 0;

	//The movement a queued READ_DATA reply carries, handed back if the reply is cancelled
	int32_t _replyX = 0;
	int32_t _replyY = 0;

	bool HostHoldsDataLow() { return (_out & 0x05) == 0; }

	uint64_t RxFall(uint8_t k) { return _rxStart + (uint64_t)(k - 1) * Period; }
	uint64_t RxRise(uint8_t k) { return RxFall(k) + HalfPeriod; }

	static uint8_t OddParity(uint8_t value)
	{
		uint8_t ones = 0;
		for(int i = 0; i < 8; i++) {
			ones += (value >> i) & 1;
		}
		return (ones & 1) ? 0 : 1;
	}

	void Reply(uint64_t at, std::initializer_list<uint8_t> bytes)
	{
		_txLen = 0;
		for(uint8_t b : bytes) {
			_txData[_txLen++] = b;
		}
		_txStart = at;
		_state = State::Transmit;
	}

	void CatchUpMovement()
	{
		MousePosition pos = GetCoordinates();
		_accumX += pos.X - _seenX;
		_accumY += pos.Y - _seenY;
		_seenX = pos.X;
		_seenY = pos.Y;
	}

	//A reply that never reached the host gives its movement back
	void CancelReply()
	{
		if(_state == State::Transmit) {
			_accumX += _replyX;
			_accumY += _replyY;
		}
		_replyX = 0;
		_replyY = 0;
	}

	void PackReadData(uint8_t packet[3])
	{
		CatchUpMovement();

		//PS/2 counts Y upwards
		int32_t dx = std::clamp(_accumX, -127, 127);
		int32_t dy = std::clamp(-_accumY, -127, 127);
		_accumX -= dx;
		_accumY += dy;
		_replyX = dx;
		_replyY = -dy;

		//Right in bit 0 and left in bit 1, the reverse of a PC's PS/2 mouse: right is bit 0 on
		//BbkMouse as well, and the card's driver opens what it points at on bit 1
		packet[0] = 0x08 |
			(IsPressed(Buttons::Right) ? 0x01 : 0) |
			(IsPressed(Buttons::Left) ? 0x02 : 0) |
			(IsPressed(Buttons::Middle) ? 0x04 : 0) |
			(dx < 0 ? 0x10 : 0) |
			(dy < 0 ? 0x20 : 0);
		//Y before X - the order BbkMouse's packet has too; a PC's PS/2 mouse sends X first
		packet[1] = (uint8_t)dy;
		packet[2] = (uint8_t)dx;
	}

	void CommandReceived(uint8_t cmd, uint64_t at)
	{
		at += ReplyDelay;
		_replyX = 0;
		_replyY = 0;
		switch(cmd) {
			case CmdReadData: {
				uint8_t p[3];
				PackReadData(p);
				Reply(at, { Ack, p[0], p[1], p[2] });
				break;
			}
			case CmdReset: Reply(at, { Ack, 0xAA, 0x00 }); break;
			case CmdGetDeviceId: Reply(at, { Ack, 0x00 }); break;
			default: Reply(at, { Ack }); break;
		}
	}

	//Brings the mouse up to 'now', using the host's data line as it has been since the last
	//write - the host only changes it by writing, and every write catches up first
	void Advance(uint64_t now)
	{
		while(true) {
			switch(_state) {
				case State::Idle:
					if(_requestPending && now >= _requestAt) {
						_requestPending = false;
						_state = State::Receive;
						_rxStart = _requestAt;
						_rxNext = 1;
						_rxBits = 0;
						continue;
					}
					return;

				case State::Receive:
					while(_rxNext <= 11 && RxRise(_rxNext) <= now) {
						uint16_t bit = HostHoldsDataLow() ? 0 : 1;
						if(_rxNext == 1 && bit) {
							//The host let go before the first clock: nothing to receive
							_state = State::Idle;
							return;
						}
						_rxBits |= bit << (_rxNext - 1);
						_rxNext++;
					}
					if(_rxNext > 11 && RxRise(12) <= now) {
						uint8_t cmd = (uint8_t)(_rxBits >> 1);
						uint8_t parity = (_rxBits >> 9) & 1;
						_state = State::Idle;
						if(parity == OddParity(cmd)) {
							CommandReceived(cmd, RxRise(12));
						}
						continue;
					}
					return;

				case State::Transmit:
					if(now >= _txStart + (uint64_t)FrameCycles * _txLen) {
						_replyX = 0;
						_replyY = 0;
						_state = State::Idle;
						continue;
					}
					return;
			}
		}
	}

	bool IsClockLow(uint64_t now)
	{
		if(_state == State::Receive) {
			for(uint8_t k = 1; k <= 12; k++) {
				if(now >= RxFall(k) && now < RxRise(k)) {
					return true;
				}
			}
		} else if(_state == State::Transmit && now >= _txStart) {
			uint32_t offset = (uint32_t)((now - _txStart) % FrameCycles);
			return offset < Period * 11 && (offset % Period) >= HalfPeriod;
		}
		return false;
	}

	bool IsMouseHoldingDataLow(uint64_t now)
	{
		if(_state == State::Receive) {
			return now >= RxFall(12) && now < RxRise(12);
		} else if(_state == State::Transmit && now >= _txStart) {
			uint64_t elapsed = now - _txStart;
			uint8_t byte = _txData[elapsed / FrameCycles];
			uint32_t bit = (uint32_t)(elapsed % FrameCycles) / Period;
			if(bit == 0) {
				return true; //start bit
			} else if(bit <= 8) {
				return ((byte >> (bit - 1)) & 1) == 0;
			} else if(bit == 9) {
				return OddParity(byte) == 0;
			}
		}
		return false;
	}

protected:
	bool HasCoordinates() override { return true; }
	enum Buttons { Left = 0, Right, Middle };

	//One character per button, which is what puts the buttons in the recorded input state
	string GetKeyNames() override { return "LRM"; }

	void Serialize(Serializer& s) override
	{
		BaseControlDevice::Serialize(s);
		SV(_state); SV(_out); SV(_requestAt); SV(_requestPending);
		SV(_rxStart); SV(_rxNext); SV(_rxBits);
		SV(_txStart); SV(_txLen);
		SVArray(_txData, 4);
		SV(_accumX); SV(_accumY); SV(_seenX); SV(_seenY);
		SV(_replyX); SV(_replyY);
	}

	void InternalSetStateFromInput() override
	{
		//The buttons come from the physical mouse rather than a key mapping, so the device the
		//machine connects by itself needs no setup
		SetMovement(KeyManager::GetMouseMovement(_emu, _emu->GetSettings()->GetInputConfig().MouseSensitivity));
		SetPressedState(Buttons::Left, KeyManager::IsKeyPressed(IKeyManager::BaseMouseButtonIndex + (int)MouseButton::LeftButton));
		SetPressedState(Buttons::Right, KeyManager::IsKeyPressed(IKeyManager::BaseMouseButtonIndex + (int)MouseButton::RightButton));
		SetPressedState(Buttons::Middle, KeyManager::IsKeyPressed(IKeyManager::BaseMouseButtonIndex + (int)MouseButton::MiddleButton));
	}

	void OnAfterSetState() override
	{
		//Reads the coordinates without clearing them - the input recorder runs after this
		MousePosition pos = GetCoordinates();
		_accumX += pos.X;
		_accumY += pos.Y;
		_seenX = pos.X;
		_seenY = pos.Y;
	}

public:
	Ps2Mouse(Emulator* emu, uint8_t port, KeyMappingSet keyMappings) : BaseControlDevice(emu, ControllerType::Ps2Mouse, port, keyMappings)
	{
	}

	uint8_t ReadRam(uint16_t addr) override
	{
		if(addr != 0x4017) {
			return 0;
		}

		uint64_t now = _emu->GetMasterClock();
		Advance(now);
		bool low = (_out & 0x04) ? (IsMouseHoldingDataLow(now) || HostHoldsDataLow()) : IsClockLow(now);
		return low ? 0x01 : 0x00;
	}

	void WriteRam(uint16_t addr, uint8_t value) override
	{
		if(addr != 0x4016) {
			return;
		}

		uint64_t now = _emu->GetMasterClock();
		Advance(now);

		bool wasLow = HostHoldsDataLow();
		_out = value;
		if(HostHoldsDataLow() && !wasLow && _state != State::Receive) {
			//A request to send: it takes precedence over anything the mouse was about to say
			CancelReply();
			_state = State::Idle;
			_requestPending = true;
			_requestAt = now + RequestDelay;
		} else if(!HostHoldsDataLow()) {
			_requestPending = false;
		}
	}

	vector<DeviceButtonName> GetKeyNameAssociations() override
	{
		return {
			{ "xOffset", BaseControlDevice::DeviceXCoordButtonId, true },
			{ "yOffset", BaseControlDevice::DeviceYCoordButtonId, true },
			{ "left", Buttons::Left },
			{ "right", Buttons::Right },
			{ "middle", Buttons::Middle },
		};
	}
};
