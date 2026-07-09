#pragma once
#include "pch.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/KeyManager.h"
#include "Utilities/Serializer.h"

//BBK/YuXing FD-1 keyboard - shares the Subor keyboard's scan protocol on $4016/$4017,
//but the physical key matrix differs from the Subor keyboard in a few positions. Ported
//from the VirtuaNES-BBK fork (NES/PadEX/EXPAD_BBK_FD1.cpp). Only the divergent cells and
//the shared-modifier wiring are BBK-specific; everything else matches SuborKeyboard.
class BbkKeyboard : public BaseControlDevice
{
private:
	uint8_t _row = 0;
	uint8_t _column = 0;
	bool _enabled = false;

	//The BBK keyboard wires both left/right Shift, Ctrl and Alt keys to a single matrix
	//line each (the fork checks DIK_LSHIFT || DIK_RSHIFT, etc.). Mesen maps one host key
	//per emulated button, so read the physical right-hand modifiers directly and OR them
	//onto the single Shift/Ctrl/Alt matrix bit. Resolved once at construction.
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
		Unknown1, Unknown2, Unknown3, None
	};

	//Identical to the Subor keyboard matrix except for the three BBK divergences noted
	//inline (verified against EXPAD_BBK_FD1.cpp): the Subor "Alt" line (ScanNo 11) is not
	//wired on the BBK, and the BBK reads Alt and Tab from ScanNo 13 where the Subor has
	//Pause and (nothing) respectively.
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
		None, Numpad4, Numpad7, F11, F12, Numpad1, Numpad2, Numpad8, //BBK: ScanNo 11 Alt line unwired (Subor had Alt here)
		NumpadMinus, NumpadPlus, NumpadMultiply, Numpad9, F10, Numpad5, NumpadDivide, NumLock,
		Grave, Numpad6, Alt, Tab, F9, Numpad3, NumpadDot, Numpad0 //BBK: ScanNo 13 reads Alt/Tab (Subor had Pause/Space)
	};

	void InternalSetStateFromInput() override
	{
		for(KeyMapping& keyMapping : _keyMappings) {
			for(int i = 0; i < 99; i++) {
				SetPressedState(i, keyMapping.CustomKeys[i]);
			}
		}

		//Honor the physical right-hand modifiers as aliases of the single Shift/Ctrl/Alt
		//matrix line (see class comment). Only ever OR them on, so a mapped left modifier
		//is never cleared.
		if(_rightShiftKey && KeyManager::IsKeyPressed(_rightShiftKey)) { SetPressedState((uint8_t)Buttons::Shift, true); }
		if(_rightCtrlKey && KeyManager::IsKeyPressed(_rightCtrlKey)) { SetPressedState((uint8_t)Buttons::Ctrl, true); }
		if(_rightAltKey && KeyManager::IsKeyPressed(_rightAltKey)) { SetPressedState((uint8_t)Buttons::Alt, true); }
	}

	uint8_t GetActiveKeys(uint8_t row, uint8_t column)
	{
		uint8_t result = 0;
		uint32_t baseIndex = row * 8 + (column ? 4 : 0);
		for(int i = 0; i < 4; i++) {
			if(IsPressed(_keyboardMatrix[baseIndex + i])) {
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
		SV(_row); SV(_column); SV(_enabled);
	}

	void RefreshStateBuffer() override
	{
		_row = 0;
		_column = 0;
	}

public:
	BbkKeyboard(Emulator* emu, KeyMappingSet keyMappings) : BaseControlDevice(emu, ControllerType::BbkKeyboard, BaseControlDevice::ExpDevicePort, keyMappings)
	{
		_rightShiftKey = KeyManager::GetKeyCode("Right Shift");
		_rightCtrlKey = KeyManager::GetKeyCode("Right Ctrl");
		_rightAltKey = KeyManager::GetKeyCode("Right Alt");
	}

	uint8_t ReadRam(uint16_t addr) override
	{
		if(addr == 0x4017) {
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
