#pragma once
#include "pch.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/KeyManager.h"
#include "Utilities/Serializer.h"

//Subor SB-2000 PS/2-style keyboard. The serial protocol (commands, ACKs, byte pacing,
//IRQs) lives in Sb2kMapper; this device only tracks which keys are down and hands the
//mapper one key transition at a time as scan-code set 1 make/break codes (the BIOS
//selects set 1 during its init handshake). Codes are stored in the reference emulator's
//convention: values >= $80 are extended keys, transmitted as an $E0 prefix + code.
class Sb2kKeyboard : public BaseControlDevice
{
private:
	uint8_t _prevKeyState[99] = {};

	//Right-hand modifiers have their own scan codes ($36/$9D/$B8) but Mesen's mapping UI
	//exposes a single Shift/Ctrl/Alt button, so the physical right-hand keys are read
	//directly and reported through the three spare matrix slots. Resolved at construction.
	uint16_t _rightShiftKey = 0;
	uint16_t _rightCtrlKey = 0;
	uint16_t _rightAltKey = 0;

protected:
	string GetKeyNames() override
	{
		return "ABCDEFGHIJKLMNOPQRSTUVWXYZ01234567891234567890120123456789edpmdmncdsasbemglrcpcsasbteeehidududlr123";
	}

	enum Buttons
	{
		A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
		Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
		F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
		Numpad0, Numpad1, Numpad2, Numpad3, Numpad4, Numpad5, Numpad6, Numpad7, Numpad8, Numpad9,
		NumpadEnter, NumpadDot, NumpadPlus, NumpadMultiply, NumpadDivide, NumpadMinus, NumLock,
		Comma, Dot, SemiColon, Apostrophe,
		Slash, Backslash,
		Equal, Minus, Grave,
		LeftBracket, RightBracket,
		CapsLock, Pause,
		Ctrl, Shift, Alt,
		Space, Backspace, Tab, Esc, Enter,
		End, Home,
		Ins, Delete,
		PageUp, PageDown,
		Up, Down, Left, Right,
		RightShift, RightCtrl, RightAlt, None
	};

	//Scan-code set 1 make codes per button, indexed by the Buttons enum. Extended keys
	//(navigation cluster, numpad enter/divide, right Ctrl/Alt) carry bit 7 set.
	static constexpr uint8_t ScanCodes[99] = {
		//A-Z
		0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
		0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
		//0-9 (top row)
		0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
		//F1-F12
		0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
		//Numpad 0-9
		0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49,
		//Numpad enter, dot, plus, multiply, divide, minus, num lock
		0x9C, 0x53, 0x4E, 0x37, 0xB5, 0x4A, 0x45,
		//Comma, dot, semicolon, apostrophe
		0x33, 0x34, 0x27, 0x28,
		//Slash, backslash
		0x35, 0x2B,
		//Equal, minus, grave
		0x0D, 0x0C, 0x29,
		//Brackets
		0x1A, 0x1B,
		//Caps lock, pause
		0x3A, 0xC5,
		//Left Ctrl, Shift, Alt
		0x1D, 0x2A, 0x38,
		//Space, backspace, tab, esc, enter
		0x39, 0x0E, 0x0F, 0x01, 0x1C,
		//End, home
		0xCF, 0xC7,
		//Insert, delete
		0xD2, 0xD3,
		//Page up, page down
		0xC9, 0xD1,
		//Up, down, left, right
		0xC8, 0xD0, 0xCB, 0xCD,
		//Right Shift, Ctrl, Alt
		0x36, 0x9D, 0xB8
	};

	void InternalSetStateFromInput() override
	{
		for(KeyMapping& keyMapping : _keyMappings) {
			for(int i = 0; i < 99; i++) {
				SetPressedState(i, keyMapping.CustomKeys[i]);
			}
		}

		//Physical right-hand modifiers (only ever OR'd on, so mapped keys are never cleared)
		if(_rightShiftKey && KeyManager::IsKeyPressed(_rightShiftKey)) { SetPressedState((uint8_t)Buttons::RightShift, true); }
		if(_rightCtrlKey && KeyManager::IsKeyPressed(_rightCtrlKey)) { SetPressedState((uint8_t)Buttons::RightCtrl, true); }
		if(_rightAltKey && KeyManager::IsKeyPressed(_rightAltKey)) { SetPressedState((uint8_t)Buttons::RightAlt, true); }
	}

	//Lets a script press keys by name (emu.setInput), which is how the headless test
	//runner drives the machines that have no joypad.
	vector<DeviceButtonName> GetKeyNameAssociations() override
	{
		return {
			{ "a", Buttons::A },
			{ "b", Buttons::B },
			{ "c", Buttons::C },
			{ "d", Buttons::D },
			{ "e", Buttons::E },
			{ "f", Buttons::F },
			{ "g", Buttons::G },
			{ "h", Buttons::H },
			{ "i", Buttons::I },
			{ "j", Buttons::J },
			{ "k", Buttons::K },
			{ "l", Buttons::L },
			{ "m", Buttons::M },
			{ "n", Buttons::N },
			{ "o", Buttons::O },
			{ "p", Buttons::P },
			{ "q", Buttons::Q },
			{ "r", Buttons::R },
			{ "s", Buttons::S },
			{ "t", Buttons::T },
			{ "u", Buttons::U },
			{ "v", Buttons::V },
			{ "w", Buttons::W },
			{ "x", Buttons::X },
			{ "y", Buttons::Y },
			{ "z", Buttons::Z },
			{ "num0", Buttons::Num0 },
			{ "num1", Buttons::Num1 },
			{ "num2", Buttons::Num2 },
			{ "num3", Buttons::Num3 },
			{ "num4", Buttons::Num4 },
			{ "num5", Buttons::Num5 },
			{ "num6", Buttons::Num6 },
			{ "num7", Buttons::Num7 },
			{ "num8", Buttons::Num8 },
			{ "num9", Buttons::Num9 },
			{ "f1", Buttons::F1 },
			{ "f2", Buttons::F2 },
			{ "f3", Buttons::F3 },
			{ "f4", Buttons::F4 },
			{ "f5", Buttons::F5 },
			{ "f6", Buttons::F6 },
			{ "f7", Buttons::F7 },
			{ "f8", Buttons::F8 },
			{ "f9", Buttons::F9 },
			{ "f10", Buttons::F10 },
			{ "f11", Buttons::F11 },
			{ "f12", Buttons::F12 },
			{ "numpad0", Buttons::Numpad0 },
			{ "numpad1", Buttons::Numpad1 },
			{ "numpad2", Buttons::Numpad2 },
			{ "numpad3", Buttons::Numpad3 },
			{ "numpad4", Buttons::Numpad4 },
			{ "numpad5", Buttons::Numpad5 },
			{ "numpad6", Buttons::Numpad6 },
			{ "numpad7", Buttons::Numpad7 },
			{ "numpad8", Buttons::Numpad8 },
			{ "numpad9", Buttons::Numpad9 },
			{ "numpadenter", Buttons::NumpadEnter },
			{ "numpaddot", Buttons::NumpadDot },
			{ "numpadplus", Buttons::NumpadPlus },
			{ "numpadmultiply", Buttons::NumpadMultiply },
			{ "numpaddivide", Buttons::NumpadDivide },
			{ "numpadminus", Buttons::NumpadMinus },
			{ "numlock", Buttons::NumLock },
			{ "comma", Buttons::Comma },
			{ "dot", Buttons::Dot },
			{ "semicolon", Buttons::SemiColon },
			{ "apostrophe", Buttons::Apostrophe },
			{ "slash", Buttons::Slash },
			{ "backslash", Buttons::Backslash },
			{ "equal", Buttons::Equal },
			{ "minus", Buttons::Minus },
			{ "grave", Buttons::Grave },
			{ "leftbracket", Buttons::LeftBracket },
			{ "rightbracket", Buttons::RightBracket },
			{ "capslock", Buttons::CapsLock },
			{ "pause", Buttons::Pause },
			{ "ctrl", Buttons::Ctrl },
			{ "shift", Buttons::Shift },
			{ "alt", Buttons::Alt },
			{ "space", Buttons::Space },
			{ "backspace", Buttons::Backspace },
			{ "tab", Buttons::Tab },
			{ "esc", Buttons::Esc },
			{ "enter", Buttons::Enter },
			{ "end", Buttons::End },
			{ "home", Buttons::Home },
			{ "ins", Buttons::Ins },
			{ "delete", Buttons::Delete },
			{ "pageup", Buttons::PageUp },
			{ "pagedown", Buttons::PageDown },
			{ "up", Buttons::Up },
			{ "down", Buttons::Down },
			{ "left", Buttons::Left },
			{ "right", Buttons::Right },
			{ "rightshift", Buttons::RightShift },
			{ "rightctrl", Buttons::RightCtrl },
			{ "rightalt", Buttons::RightAlt },
		};
	}

	void Serialize(Serializer& s) override
	{
		BaseControlDevice::Serialize(s);
		SVArray(_prevKeyState, sizeof(_prevKeyState));
	}

public:
	Sb2kKeyboard(Emulator* emu, KeyMappingSet keyMappings) : BaseControlDevice(emu, ControllerType::Sb2kKeyboard, BaseControlDevice::ExpDevicePort, keyMappings)
	{
		_rightShiftKey = KeyManager::GetKeyCode("Right Shift");
		_rightCtrlKey = KeyManager::GetKeyCode("Right Ctrl");
		_rightAltKey = KeyManager::GetKeyCode("Right Alt");
	}

	//Not on the controller bus - Sb2kMapper polls the device through its own registers
	uint8_t ReadRam(uint16_t addr) override { return 0; }
	void WriteRam(uint16_t addr, uint8_t value) override {}

	//Returns one pending key transition: the scan code in bits 0-7 (bit 7 set = extended,
	//to be sent as $E0 + code) and bit 8 set for a release; -1 when nothing changed.
	//One event per call - the keyboard sends one make/break sequence per report interval,
	//and unreported transitions stay pending for the next call.
	int32_t GetNextKeyEvent()
	{
		for(int32_t i = 0; i < 99; i++) {
			uint8_t pressed = IsPressed((uint8_t)i) ? 1 : 0;
			if(pressed != _prevKeyState[i]) {
				_prevKeyState[i] = pressed;
				return ScanCodes[i] | (pressed ? 0 : 0x100);
			}
		}
		return -1;
	}
};
