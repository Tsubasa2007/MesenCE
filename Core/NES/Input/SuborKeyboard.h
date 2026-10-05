#pragma once
#include "pch.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/KeyManager.h"
#include "Utilities/Serializer.h"

class SuborKeyboard : public BaseControlDevice
{
private:
	uint8_t _row = 0;
	uint8_t _column = 0;
	bool _enabled = false;
	bool _reduced = false;
	bool _extended = false;
	bool _mouseOnBit0 = false;
	uint8_t _scan = 0;
	bool _out = false;
	uint16_t _rightShiftKey = 0;
	uint16_t _rightCtrlKey = 0;
	uint16_t _rightAltKey = 0;

protected:
	string GetKeyNames() override
	{
		return "ABCDEFGHIJKLMNOPQRSTUVWXYZ01234567891234567890120123456789edpmdmncdsasbemglrcpcsasbteeehidududlr123";
	}

	//Lets a script drive this keyboard through emu.setInput, as the learning machines'
	//keyboards can be. Without it nothing that needs a key press can be tested headlessly.
	vector<DeviceButtonName> GetKeyNameAssociations() override
	{
		vector<DeviceButtonName> names;
		for(int i = 0; i < 26; i++) {
			names.push_back({ string(1, (char)('a' + i)), Buttons::A + i });
		}
		for(int i = 0; i < 10; i++) {
			names.push_back({ "num" + std::to_string(i), Buttons::Num0 + i });
			names.push_back({ "numpad" + std::to_string(i), Buttons::Numpad0 + i });
		}
		for(int i = 0; i < 12; i++) {
			names.push_back({ "f" + std::to_string(i + 1), Buttons::F1 + i });
		}
		vector<DeviceButtonName> others = {
			{ "numpadenter", Buttons::NumpadEnter }, { "numpaddot", Buttons::NumpadDot }, { "numpadplus", Buttons::NumpadPlus },
			{ "numpadmultiply", Buttons::NumpadMultiply }, { "numpaddivide", Buttons::NumpadDivide }, { "numpadminus", Buttons::NumpadMinus },
			{ "numlock", Buttons::NumLock }, { "comma", Buttons::Comma }, { "dot", Buttons::Dot }, { "semicolon", Buttons::SemiColon },
			{ "apostrophe", Buttons::Apostrophe }, { "slash", Buttons::Slash }, { "backslash", Buttons::Backslash }, { "equal", Buttons::Equal },
			{ "minus", Buttons::Minus }, { "grave", Buttons::Grave }, { "leftbracket", Buttons::LeftBracket }, { "rightbracket", Buttons::RightBracket },
			{ "capslock", Buttons::CapsLock }, { "pause", Buttons::Pause }, { "ctrl", Buttons::Ctrl }, { "shift", Buttons::Shift },
			{ "alt", Buttons::Alt }, { "space", Buttons::Space }, { "backspace", Buttons::Backspace }, { "tab", Buttons::Tab },
			{ "esc", Buttons::Esc }, { "enter", Buttons::Enter }, { "end", Buttons::End }, { "home", Buttons::Home },
			{ "ins", Buttons::Ins }, { "delete", Buttons::Delete }, { "pageup", Buttons::PageUp }, { "pagedown", Buttons::PageDown },
			{ "up", Buttons::Up }, { "down", Buttons::Down }, { "left", Buttons::Left }, { "right", Buttons::Right }
		};
		names.insert(names.end(), others.begin(), others.end());
		return names;
	}

	// clang-format off
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
		Unknown1, Unknown2, Unknown3, None
	};

	Buttons _keyboardMatrix[104] = {
		Num4, G, F, C, F2, E, Num5, V,
		Num2, D, S, End, F1, W, Num3, X,
		Ins, Backspace, PageDown, Right, F8, PageUp, Delete, Home,
		Num9, I, L, Comma, F5, O, Num0, Dot,
		RightBracket, Enter, Up, Left, F7, LeftBracket, Backslash, Down,
		Q, CapsLock, Z, Tab, Esc, A, Num1, Ctrl,
		Num7, Y, K, M, F4, U, Num8, J,
		Minus, SemiColon, Apostrophe, Slash, F6, P, Equal, Shift,
		T, H, N, Space, F3, R, Num6, B,
		Numpad6, NumpadEnter, Numpad4, Numpad8, None, Unknown1, Unknown2, Unknown3,
		Alt, Numpad4, Numpad7, F11, F12, Numpad1, Numpad2, Numpad8,
		NumpadMinus, NumpadPlus, NumpadMultiply, Numpad9, F10, Numpad5, NumpadDivide, NumLock,
		Grave, Numpad6, Pause, Space, F9, Numpad3, NumpadDot, Numpad0
	};
	// clang-format on

	void InternalSetStateFromInput() override
	{
		for(KeyMapping& keyMapping : _keyMappings) {
			for(int i = 0; i < 99; i++) {
				SetPressedState(i, keyMapping.CustomKeys[i]);
			}
		}

		//The matrix has one Shift/Ctrl/Alt line each, so the right-hand keys are the same keys as
		//the left-hand ones the default mapping binds - as on the BBK keyboard. Only ever OR them on.
		if(_rightShiftKey && KeyManager::IsKeyPressed(_rightShiftKey)) { SetPressedState((uint8_t)Buttons::Shift, true); }
		if(_rightCtrlKey && KeyManager::IsKeyPressed(_rightCtrlKey)) { SetPressedState((uint8_t)Buttons::Ctrl, true); }
		if(_rightAltKey && KeyManager::IsKeyPressed(_rightAltKey)) { SetPressedState((uint8_t)Buttons::Alt, true); }
	}

	//The extended keyboard the Subor cartridges carry has Alt at row 12 bit 2, where the full
	//matrix above has Pause: their key decoder treats that bit as the Alt modifier (and pairs it
	//with Ctrl and Del for a reset), while row 10 bit 0 is an ordinary key to it. Every one of them
	//that has this decoder agrees, so the two swap places on that keyboard.
	//Space is only row 8 bit 3 there: the 13-row decoders (V6+, V14, V15, the SB WPS disk) give row
	//12 bit 3 a code of its own ($14), and the WPS disk drops any scan with more than one key down -
	//a Space held at both places never reached it.
	Buttons GetMatrixKey(uint32_t index)
	{
		if(_extended) {
			if(index == 98) {
				return Buttons::Alt;
			} else if(index == 80) {
				return Buttons::Pause;
			} else if(index == 99) {
				return Buttons::None;
			}
		}
		return _keyboardMatrix[index];
	}

	uint8_t GetActiveKeys(uint8_t row, uint8_t column)
	{
		uint8_t result = 0;
		uint32_t baseIndex = row * 8 + (column ? 4 : 0);
		for(int i = 0; i < 4; i++) {
			if(IsPressed(GetMatrixKey(baseIndex + i))) {
				result |= (1 << i);
			}
		}

		if(row == 9 && column) {
			//This bit is used to indicate that this is an "extended" version of
			//the keyboard which has four more rows to read from (numpad, etc.)
			//Corresponds to row 9, bit 4
			result |= 0x01;
		}

		return result;
	}

	void Serialize(Serializer& s) override
	{
		BaseControlDevice::Serialize(s);
		SV(_row);
		SV(_column);
		SV(_enabled);
		SV(_scan);
		SV(_out);
	}

	void RefreshStateBuffer() override
	{
		_row = 0;
		_column = 0;
	}

public:
	//A key by its Buttons index, for a board that reads this keyboard through a matrix of its
	//own. The order matches YuxingKeyboard's up to the arrow keys; the three unnamed keys
	//after them have no counterpart there.
	bool IsKeyPressed(uint8_t key)
	{
		return key <= (uint8_t)Buttons::Right && IsPressed(key);
	}

	//'extended' lets the reduced keyboard's counter run on through rows 9-12, the full keyboard's
	//numpad/Alt/F11/F12 rows, with row 9 carrying the bit that says they are there
	SuborKeyboard(Emulator* emu, KeyMappingSet keyMappings, bool reduced = false, bool extended = false, bool mouseOnBit0 = false) : BaseControlDevice(emu, ControllerType::SuborKeyboard, BaseControlDevice::ExpDevicePort, keyMappings)
	{
		_reduced = reduced;
		_extended = reduced && extended;
		_mouseOnBit0 = reduced && mouseOnBit0;
		_rightShiftKey = KeyManager::GetKeyCode("Right Shift");
		_rightCtrlKey = KeyManager::GetKeyCode("Right Ctrl");
		_rightAltKey = KeyManager::GetKeyCode("Right Alt");
	}

	uint8_t ReadRam(uint16_t addr) override
	{
		if(addr == 0x4017) {
			//The reduced keyboard leaves every line it does not drive high; the full one
			//reports only bits 1-4 and zeroes the rest.
			if(_reduced) {
				//Scan position 0 is the gap the counter passes through on its way round;
				//nothing answers there. Without 'extended' the counter wraps into it after row
				//8, which is what keeps a machine that probes row 9 from ever seeing the
				//extended-keyboard bit.
				//The extended keyboard leaves bit 0 alone: the Subor cartridges' scans discard it,
				//and a mouse can share the port on that line (see Ps2Mouse). So does the reduced one
				//when that mouse is there - holding the line high reads as a clock held low, and
				//the mouse can never answer.
				uint8_t idle = (_extended || _mouseOnBit0) ? 0xE0 : 0xE1;
				if(_scan == 0) {
					return idle | 0x1E;
				}
				return (uint8_t)((((~GetActiveKeys(_scan - 1, _out ? 0 : 1)) << 1) & 0x1E) | idle);
			}
			if(_enabled) {
				uint8_t value = ((~GetActiveKeys(_row, _column)) << 1) & 0x1E;
				return value;
			} else {
				return 0x1E;
			}
		}
		return 0;
	}

	void WriteRam(uint16_t addr, uint8_t value) override
	{
		StrobeProcessWrite(value);

		if(_reduced) {
			//The reduced keyboard's counter, ported from the VirtuaNES-BBK fork: $05 restarts it,
			//$04 steps it and flips halves, $06 flips halves only. It wraps at nine, one row
			//short of the matrix, so the extended-keyboard row is never reached - unless the
			//keyboard is the extended one, when it goes on through all 13 rows.
			if(value == 0x05) {
				_scan = 0;
				_out = false;
			} else if(value == 0x04) {
				if(++_scan > (_extended ? 13 : 9)) { _scan = 0; }
				_out = !_out;
			} else if(value == 0x06) {
				_out = !_out;
			}
			return;
		}

		uint8_t prevColumn = _column;
		_column = (value & 0x02) >> 1;
		_enabled = (value & 0x04) != 0;

		if(_enabled) {
			if(!_column && prevColumn) {
				_row = (_row + 1) % 13;
			}
		}
	}
};