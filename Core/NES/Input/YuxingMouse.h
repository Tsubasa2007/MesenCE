#pragma once
#include "pch.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/KeyManager.h"
#include "Shared/EmuSettings.h"
#include "Shared/Emulator.h"
#include "Utilities/Serializer.h"

//YuXing serial mouse. Ported from the VirtuaNES-BBK fork
//(NES/PadEX/EXPAD_YuXing_Mouse.cpp, itself from NintendulatorNRS by NewRisingSun).
//
//Not the EM84502 the BBK drive unit uses: this one has no command set at all. A rising
//edge on $4016 bit 0 (with bit 2 low) clocks out the next bit of a three-byte report,
//and when the queue runs dry the next edge loads a fresh one. Each byte goes out as a
//start bit, seven data bits from bit 6 down, then a stop bit, and the line is inverted.
//
//The report is: buttons + the high 2 bits of each axis, then Y, then X (6 bits each).
//
//On the VCD models the whole port is shifted up three bits - the data bit appears at
//bit 3 instead of bit 0, and the constant bit 6 falls off the top of the byte entirely.
class YuxingMouse : public BaseControlDevice
{
private:
	static constexpr uint32_t QueueSize = 32;

	//Movement has to survive until the machine clocks a report out, while the state buffer
	//is cleared on every poll - so each poll's delta is folded into these accumulators in
	//OnAfterSetState(), after movie playback had its say
	int32_t _accumX = 0;
	int32_t _accumY = 0;

	bool _queue[QueueSize] = {};
	uint8_t _queueHead = 0;
	uint8_t _queueCount = 0;
	bool _clock = false;
	bool _vcdMode = false;

	void Push(bool bit)
	{
		if(_queueCount < QueueSize) {
			_queue[(_queueHead + _queueCount) % QueueSize] = bit;
			_queueCount++;
		}
	}

	void PushByte(uint8_t value)
	{
		Push(false); //start bit
		for(int i = 0; i < 7; i++) {
			Push((value & 0x40) != 0);
			value <<= 1;
		}
		Push(true); //stop bit
	}

	void LoadReport()
	{
		uint8_t buttons = 0;
		if(IsPressed(Buttons::Left)) { buttons |= 0x20; }
		if(IsPressed(Buttons::Right)) { buttons |= 0x10; }

		uint8_t x = (uint8_t)_accumX;
		uint8_t y = (uint8_t)_accumY;
		_accumX = 0;
		_accumY = 0;

		PushByte((uint8_t)(0x40 | buttons | ((x >> 4) & 0x0C) | ((y >> 6) & 0x03)));
		PushByte((uint8_t)(y & 0x3F));
		PushByte((uint8_t)(x & 0x3F));
	}

protected:
	bool HasCoordinates() override { return true; }
	enum Buttons { Left = 0, Right };

	//One character per button, which is what puts the buttons in the recorded input state
	string GetKeyNames() override { return "LR"; }

	void Serialize(Serializer& s) override
	{
		BaseControlDevice::Serialize(s);
		SV(_accumX); SV(_accumY); SV(_queueHead); SV(_queueCount); SV(_clock); SV(_vcdMode);
		for(uint32_t i = 0; i < QueueSize; i++) {
			SVI(_queue[i]);
		}
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
	}

public:
	YuxingMouse(Emulator* emu, uint8_t port, KeyMappingSet keyMappings) : BaseControlDevice(emu, ControllerType::YuxingMouse, port, keyMappings)
	{
	}

	//Set by YuxingMapper - see the class comment
	void SetVcdMode(bool enabled) { _vcdMode = enabled; }

	uint8_t ReadRam(uint16_t addr) override
	{
		if(addr != 0x4016 && addr != 0x4017) {
			return 0;
		}

		uint16_t value = 0x40;
		if(_clock && _queueCount > 0) {
			value |= _queue[_queueHead] ? 0 : 1;
		}
		return (uint8_t)(value << (_vcdMode ? 3 : 0));
	}

	void WriteRam(uint16_t addr, uint8_t value) override
	{
		if(addr != 0x4016) {
			return;
		}

		//The VCD models' keyboard-select writes double as a clock edge
		if(_vcdMode && (value == 0xFE || value == 0xFF)) {
			value &= 0x01;
		}

		if(!(value & 0x04) && !_clock && (value & 0x01)) {
			if(_queueCount == 0) {
				LoadReport();
			} else {
				_queueHead = (_queueHead + 1) % QueueSize;
				_queueCount--;
			}
		}
		_clock = (value & 0x01) != 0;
	}
};
