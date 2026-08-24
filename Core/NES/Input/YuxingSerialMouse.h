#pragma once
#include "pch.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/KeyManager.h"
#include "Shared/EmuSettings.h"
#include "Shared/Emulator.h"
#include "Utilities/Serializer.h"

//The mouse that came with the D-type machines' add-on cartridge, and the one every disk
//labelled for that model talks to. It is NOT the YuxingMouse: that one is the later
//machines' own device, which the BIOS clocks a bit at a time out of $4016, and the two are
//exact opposites at the wire level (see the table below). They cannot share a port line -
//their idle levels are inverted - so this is a separate device.
//
//This one is an ordinary asynchronous serial mouse with nothing driving it: its transmit
//line is simply wired to bit 0 of a controller port and it talks whenever it feels like it.
//The guest never writes the port at all, it just samples bit 0 in a software timing loop:
//
//                       this device            YuxingMouse
//    clocking           free-running           host-clocked ($4016 <- $00, $01)
//    bit order          LSB first              MSB first
//    polarity           straight               inverted
//    start of frame     line low               line high
//
//Framing is 1 start bit (low), 7 data bits LSB first, then stop bits, with the line idling
//high. The payload is the standard 3-byte mouse packet - the same one both mice use:
//
//    byte 0: 1 L R yy xx    sync bit, buttons, high 2 bits of each axis
//    byte 1: 0 xxxxxx       low 6 bits of X
//    byte 2: 0 yyyyyy       low 6 bits of Y
//
//Only bit 0 of the port is the mouse; the rest of the byte is fixed wiring, which is what
//makes the guest's "is a mouse plugged in" test a plain compare of the whole byte against
//$FD (port 1) or $E1 (port 2) with the line idle.
class YuxingSerialMouse : public BaseControlDevice
{
private:
	//Measured against the guest's own receive loop rather than guessed: it samples every 192
	//CPU cycles, taking byte 0's data bits at +285..+1437 from the falling edge it caught,
	//byte 1's start bit at +2027, byte 1's data at +2218..+3370, and then - immediately, with
	//no pause - demanding the line already be back at the stop level. The device therefore
	//has to run slightly ahead of that loop, and the byte pacing is what keeps the drift from
	//accumulating: the whole 3-byte packet has to stay in step to the end. These values leave
	//the smallest margin at 16 cycles, which is the most this loop allows, and put the line
	//rate at ~9600 baud.
	static constexpr uint32_t CyclesPerBit = 186;
	static constexpr uint32_t CyclesPerByte = 1912;
	static constexpr uint32_t PacketCycles = CyclesPerByte * 3;

	//Movement has to survive until the guest gets around to reading a packet, while the
	//state buffer is cleared on every poll - so each poll's delta is folded into these
	//accumulators in OnAfterSetState(), after movie playback had its say
	int32_t _accumX = 0;
	int32_t _accumY = 0;

	uint8_t _packet[3] = {};
	uint64_t _txStart = 0;
	bool _sending = false;
	uint8_t _lastButtons = 0;

	static int32_t Clamp(int32_t value)
	{
		return value < -128 ? -128 : (value > 127 ? 127 : value);
	}

	//True when the transmit line is at the high (idle / stop / '1') level
	bool GetLine()
	{
		if(!_sending) {
			return true;
		}

		uint64_t elapsed = _emu->GetMasterClock() - _txStart;
		if(elapsed >= PacketCycles) {
			return true;
		}

		uint32_t offset = (uint32_t)(elapsed % CyclesPerByte);
		if(offset < CyclesPerBit) {
			return false; //start bit
		} else if(offset < CyclesPerBit * 8) {
			uint8_t bit = (uint8_t)(offset / CyclesPerBit) - 1;
			return ((_packet[elapsed / CyclesPerByte] >> bit) & 0x01) != 0;
		}
		return true; //stop bits, then idle until the next byte's start bit
	}

	void StartPacket()
	{
		uint8_t buttons = 0;
		if(IsPressed(Buttons::Left)) { buttons |= 0x20; }
		if(IsPressed(Buttons::Right)) { buttons |= 0x10; }

		int32_t x = Clamp(_accumX);
		int32_t y = Clamp(_accumY);
		_accumX -= x;
		_accumY -= y;

		_packet[0] = (uint8_t)(0x40 | buttons | ((y >> 4) & 0x0C) | ((x >> 6) & 0x03));
		_packet[1] = (uint8_t)(x & 0x3F);
		_packet[2] = (uint8_t)(y & 0x3F);

		_lastButtons = buttons;
		_txStart = _emu->GetMasterClock();
		_sending = true;
	}

protected:
	bool HasCoordinates() override { return true; }
	enum Buttons { Left = 0, Right };

	//One character per button, which is what puts the buttons in the recorded input state
	string GetKeyNames() override { return "LR"; }

	void Serialize(Serializer& s) override
	{
		BaseControlDevice::Serialize(s);
		SV(_accumX); SV(_accumY); SV(_txStart); SV(_sending); SV(_lastButtons);
		SVArray(_packet, 3);
	}

	void InternalSetStateFromInput() override
	{
		SetMovement(KeyManager::GetMouseMovement(_emu, _emu->GetSettings()->GetInputConfig().MouseSensitivity));
		SetPressedState(Buttons::Left, KeyManager::IsKeyPressed(IKeyManager::BaseMouseButtonIndex + (int)MouseButton::LeftButton));
		SetPressedState(Buttons::Right, KeyManager::IsKeyPressed(IKeyManager::BaseMouseButtonIndex + (int)MouseButton::RightButton));
	}

	void OnAfterSetState() override
	{
		MousePosition pos = GetCoordinates();
		_accumX += pos.X;
		_accumY += pos.Y;

		if(_sending && _emu->GetMasterClock() - _txStart >= PacketCycles) {
			_sending = false;
		}

		//A real mouse of this kind reports when something changed and stays quiet otherwise.
		//A packet is far shorter than a frame, so at most one is ever in flight here.
		uint8_t buttons = (IsPressed(Buttons::Left) ? 0x20 : 0) | (IsPressed(Buttons::Right) ? 0x10 : 0);
		if(!_sending && (_accumX || _accumY || buttons != _lastButtons)) {
			StartPacket();
		}
	}

public:
	YuxingSerialMouse(Emulator* emu, uint8_t port, KeyMappingSet keyMappings) : BaseControlDevice(emu, ControllerType::YuxingSerialMouse, port, keyMappings)
	{
	}

	uint8_t ReadRam(uint16_t addr) override
	{
		if(!IsCurrentPort(addr)) {
			return 0;
		}

		//Everything except bit 0 is fixed wiring, and differs between the two ports - that
		//is what the guest's $FD / $E1 identification test is really looking at
		uint8_t value = (addr == 0x4016) ? 0xFC : 0xE0;
		return value | (GetLine() ? 0x01 : 0x00);
	}

	void WriteRam(uint16_t addr, uint8_t value) override
	{
		//Nothing drives this mouse - it talks on its own schedule
	}
};
