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

	//How much of this poll's movement has already been folded in. The machine clocks a
	//report out mid-frame, well after the poll that set the movement, and a script sets
	//movement later still - the input-polled event runs after OnAfterSetState, so a poll
	//that folded only there would drop everything a script had to say. Folding again when
	//a report is about to go out picks up whatever arrived late, and remembering how much
	//was already taken keeps it from being counted twice. The coordinates themselves are
	//left as they are - they are what a movie records.
	int16_t _foldedX = 0;
	int16_t _foldedY = 0;

	bool _queue[QueueSize] = {};
	uint8_t _queueHead = 0;
	uint8_t _queueCount = 0;
	bool _clock = false;
	bool _vcdMode = false;

	//What the last report said the buttons were, so a change in them is worth reporting even
	//when the mouse has not moved
	uint8_t _sentButtons = 0;

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

	//Whether anything has happened worth a report. A real mouse sends one only when it has
	//moved or a button has changed, and stays quiet otherwise.
	void Fold()
	{
		MousePosition pos = GetCoordinates();
		_accumX += pos.X - _foldedX;
		_accumY += pos.Y - _foldedY;
		_foldedX = pos.X;
		_foldedY = pos.Y;
	}

	bool HasSomethingToReport()
	{
		Fold();
		uint8_t buttons = (uint8_t)((IsPressed(Buttons::Left) ? 1 : 0) | (IsPressed(Buttons::Right) ? 2 : 0));
		return _accumX != 0 || _accumY != 0 || buttons != _sentButtons;
	}

	void LoadReport()
	{
		uint8_t buttons = 0;
		if(IsPressed(Buttons::Left)) { buttons |= 0x20; }
		if(IsPressed(Buttons::Right)) { buttons |= 0x10; }
		_sentButtons = (uint8_t)((IsPressed(Buttons::Left) ? 1 : 0) | (IsPressed(Buttons::Right) ? 2 : 0));

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
		SV(_accumX); SV(_accumY); SV(_queueHead); SV(_queueCount); SV(_clock); SV(_vcdMode); SV(_sentButtons); SV(_foldedX); SV(_foldedY);
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
		_foldedX = 0;
		_foldedY = 0;
		Fold();
	}

public:
	YuxingMouse(Emulator* emu, uint8_t port, KeyMappingSet keyMappings) : BaseControlDevice(emu, ControllerType::YuxingMouse, port, keyMappings)
	{
	}

	//Set by YuxingMapper - see the class comment
	void SetVcdMode(bool enabled) { _vcdMode = enabled; }

	//Names for the movement and the buttons, so that a script can drive this mouse the
	//way it can drive every other one in the fork - each of the others has this and only
	//this one did not, which left it the single pointing device no test could move.
	vector<DeviceButtonName> GetKeyNameAssociations() override
	{
		return {
			{ "xOffset", BaseControlDevice::DeviceXCoordButtonId, true },
			{ "yOffset", BaseControlDevice::DeviceYCoordButtonId, true },
			{ "left", Buttons::Left },
			{ "right", Buttons::Right },
		};
	}

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
				//A report every time one is asked for. The machine asks sixteen times a frame,
				//without pause, and a real mouse on the other end of that wire is always
				//sending - so what it hears when the mouse is still is not silence but the
				//same report over again, saying nothing has moved and no button is down.
				//
				//Answering only when something had changed left it hearing nothing at all, and
				//the last thing it had heard stood. After a click that was a button going
				//down, so it went on acting on that click: once on the page it was clicked on,
				//and again on the page that click had turned to.
				//
				//The idle first, and it is load-bearing. A report is framed - a low start bit,
				//seven of data, a high stop bit - and between reports the line idles high,
				//which is how the reader knows where one begins. Sent back to back there is no
				//idle at all, and the reader hunts for a start bit up to three times: landing
				//on a zero inside a byte it takes that for the start, reads seven bits out of
				//step, finds a data bit where the stop bit should be and throws the report
				//away. That is what it did to all but one of 1230 of them, which is why the
				//pointer once did not move at all.
				Fold();
				for(int i = 0; i < 4; i++) {
					Push(true);
				}
				LoadReport();
			} else {
				_queueHead = (_queueHead + 1) % QueueSize;
				_queueCount--;
			}
		}
		_clock = (value & 0x01) != 0;
	}
};
