#pragma once
#include "pch.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/KeyManager.h"
#include "Shared/EmuSettings.h"
#include "Shared/Emulator.h"
#include "Utilities/Serializer.h"

//Subor SB-2000 HT6513B serial mouse (Microsoft-compatible "M3" mode at 1200bps).
//The serial line, plug-and-play announcement and byte pacing live in Sb2kMapper; this
//device only accumulates host mouse input and packs it into the 3-byte report the
//mapper transmits. Like the real mouse, nothing is sent while idle - a report goes out
//only when the mouse moved or a button changed.
class Sb2kMouse : public BaseControlDevice
{
private:
	//Movement has to survive until the mapper drains it into a packet, while the state buffer
	//is cleared on every poll - so each poll's delta is folded into these accumulators in
	//OnAfterSetState(), after movie playback had its say
	int32_t _accumX = 0;
	int32_t _accumY = 0;
	uint8_t _lastButtons = 0;

protected:
	bool HasCoordinates() override { return true; }
	enum Buttons { Left = 0, Right, Middle };

	//One character per button, which is what puts the buttons in the recorded input state
	//(movies store GetTextState(): the coordinates, then a column per named button)
	string GetKeyNames() override { return "LRM"; }

	void Serialize(Serializer& s) override
	{
		BaseControlDevice::Serialize(s);
		SV(_accumX); SV(_accumY); SV(_lastButtons);
	}

	void InternalSetStateFromInput() override
	{
		//Movement and buttons both go through the state buffer, so they end up in movies and
		//are replaced by the recorded input during playback. The buttons are read from the
		//physical mouse rather than a key mapping, so the auto-connected device needs no setup.
		SetMovement(KeyManager::GetMouseMovement(_emu, _emu->GetSettings()->GetInputConfig().MouseSensitivity));
		SetPressedState(Buttons::Left, KeyManager::IsKeyPressed(IKeyManager::BaseMouseButtonIndex + (int)MouseButton::LeftButton));
		SetPressedState(Buttons::Right, KeyManager::IsKeyPressed(IKeyManager::BaseMouseButtonIndex + (int)MouseButton::RightButton));
		SetPressedState(Buttons::Middle, KeyManager::IsKeyPressed(IKeyManager::BaseMouseButtonIndex + (int)MouseButton::MiddleButton));
	}

	void OnAfterSetState() override
	{
		//Runs after any input provider replaced the state, so a replayed movie accumulates the
		//same movement the recording captured. Reads the coordinates without clearing them -
		//the input recorder runs after this and needs to see them.
		MousePosition pos = GetCoordinates();
		_accumX += pos.X;
		_accumY += pos.Y;
	}

public:
	Sb2kMouse(Emulator* emu, uint8_t port, KeyMappingSet keyMappings) : BaseControlDevice(emu, ControllerType::Sb2kMouse, port, keyMappings)
	{
	}

	//Not on the controller bus - Sb2kMapper polls the device through its own registers
	uint8_t ReadRam(uint16_t addr) override { return 0; }
	void WriteRam(uint16_t addr, uint8_t value) override {}

	//Builds the 3-byte M3 report: byte 0 = 01LR yyxx (sync bit, left/right buttons, top
	//two bits of each 8-bit signed delta), bytes 1/2 = low 6 bits of dx/dy. The middle
	//button is not reported (the reference emulator leaves its 4th byte zero).
	//Returns false when there is nothing new to send.
	bool GetPacket(uint8_t packet[3])
	{
		//Bit layout matches what the reference emulator feeds its packet builder:
		//right=$01, middle=$02, left=$04.
		uint8_t buttons =
			(IsPressed(Buttons::Right) ? 0x01 : 0) |
			(IsPressed(Buttons::Middle) ? 0x02 : 0) |
			(IsPressed(Buttons::Left) ? 0x04 : 0);

		//One packet carries an 8-bit signed delta; leave any excess accumulated for the
		//next report instead of dropping it (fast motion would get lost otherwise)
		int32_t dx = std::clamp(_accumX, -128, 127);
		int32_t dy = std::clamp(_accumY, -128, 127);

		if(dx == 0 && dy == 0 && buttons == _lastButtons) {
			return false;
		}

		_accumX -= dx;
		_accumY -= dy;
		_lastButtons = buttons;

		uint8_t bx = (uint8_t)dx;
		uint8_t by = (uint8_t)dy;
		packet[0] = 0x40 | ((buttons & 0x01) << 4) | ((buttons & 0x04) << 3) | ((by & 0xC0) >> 4) | ((bx & 0xC0) >> 6);
		packet[1] = bx & 0x3F;
		packet[2] = by & 0x3F;
		return true;
	}

	//The Dr. PC Jr. BIOS takes the same movement but packs it differently (its own button
	//bit positions, and a report every frame rather than only when something changed), so
	//it drains the raw delta and builds its own packet. Buttons come back in the order the
	//reference emulator's shared mouse state uses: bit 0 left, bit 1 right, bit 2 middle.
	void TakeDelta(int8_t& dx, int8_t& dy, uint8_t& buttons)
	{
		buttons =
			(IsPressed(Buttons::Left) ? 0x01 : 0) |
			(IsPressed(Buttons::Right) ? 0x02 : 0) |
			(IsPressed(Buttons::Middle) ? 0x04 : 0);

		//Anything past one report's range stays accumulated for the next one
		int32_t mx = std::clamp(_accumX, -128, 127);
		int32_t my = std::clamp(_accumY, -128, 127);
		_accumX -= mx;
		_accumY -= my;
		dx = (int8_t)mx;
		dy = (int8_t)my;
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
