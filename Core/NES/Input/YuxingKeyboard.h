#pragma once
#include "pch.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/KeyManager.h"
#include "Utilities/Serializer.h"

//YuXing learning machine keyboard - a 14x8 key matrix wired straight to the mapper, not
//to $4016/$4017: software writes a 14-bit row mask to $4202/$4203 and reads the OR of
//every selected row's eight columns back from $4207. Ported from the VirtuaNES-BBK fork
//(NES/VCD.hpp, YX_KeyMap by tpu).
//
//The mapper owns the row mask and calls GetColumns() - this device only reports which
//matrix cells are currently held.
class YuxingKeyboard : public BaseControlDevice
{
public:
	static constexpr uint8_t RowCount = 14;

private:
	//Scroll Lock is the 101st key on a matrix that Mesen can only give 100 custom-key
	//slots to, so it is read straight from the host instead of being remappable. It sits
	//in an isolated matrix cell (row 10, column 7) and no software is known to use it.
	uint16_t _scrollLockKey = 0;

protected:
	string GetKeyNames() override
	{
		return "ABCDEFGHIJKLMNOPQRSTUVWXYZ01234567891234567890120123456789edpmdmncdsasbemglrcpcsasbteeehidududlrwwmps";
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
		LeftWin, RightWin, Menu, PrintScreen,
		//Not remappable - see _scrollLockKey
		ScrollLock,
		None
	};

	//Row-major key matrix: [row][column]. None marks a matrix position with no key on it.
	Buttons _keyboardMatrix[RowCount][8] = {
		{ Esc,        F9,           Num7,    R,        A,        None,        None,      Shift       },
		{ None,       NumpadEnter,  None,    NumpadMultiply, NumpadDivide, Up,    Backspace, F12       },
		{ None,       None,         None,    NumpadPlus, NumLock, Left,       Down,      Right       },
		{ None,       Numpad7,      Numpad8, NumpadMinus, Numpad9, F11,       End,       PageDown    },
		{ F8,         Num6,         E,       RightBracket, L,     Z,          N,         Space       },
		{ F7,         Num5,         W,       LeftBracket, K,      Delete,     B,         Slash       },
		{ F6,         Num4,         Q,       P,        J,        Backslash,   V,         Dot         },
		{ F5,         Num3,         Equal,   O,        H,        Apostrophe,  C,         Comma       },
		{ F4,         Num2,         Minus,   I,        G,        SemiColon,   X,         M           },
		{ F3,         Num1,         Num0,    U,        F,        CapsLock,    LeftWin,   None        },
		{ F2,         Grave,        Num9,    Y,        D,        Ctrl,        Alt,       ScrollLock  },
		{ F1,         Enter,        Num8,    T,        S,        None,        RightWin,  Menu        },
		{ NumpadDot,  F10,          Numpad4, Numpad6,  Numpad5,  Ins,         Home,      PageUp      },
		{ None,       Numpad0,      Numpad1, Numpad3,  Numpad2,  PrintScreen, Tab,       Pause       }
	};

	void InternalSetStateFromInput() override
	{
		for(KeyMapping& keyMapping : _keyMappings) {
			for(int i = 0; i < (int)Buttons::ScrollLock; i++) {
				SetPressedState(i, keyMapping.CustomKeys[i]);
			}
		}

		if(_scrollLockKey && KeyManager::IsKeyPressed(_scrollLockKey)) {
			SetPressedState((uint8_t)Buttons::ScrollLock, true);
		}
	}

	void Serialize(Serializer& s) override
	{
		BaseControlDevice::Serialize(s);
	}

public:
	YuxingKeyboard(Emulator* emu, KeyMappingSet keyMappings) : BaseControlDevice(emu, ControllerType::YuxingKeyboard, BaseControlDevice::ExpDevicePort, keyMappings)
	{
		_scrollLockKey = KeyManager::GetKeyCode("Scroll Lock");
	}

	//The matrix is wired to the mapper, not to the controller port - see GetColumns()
	uint8_t ReadRam(uint16_t addr) override { return 0; }
	void WriteRam(uint16_t addr, uint8_t value) override {}

	//Returns the eight column lines for every row the mask selects, ORed together - the
	//matrix has no diodes, so selecting several rows at once merges their key states.
	//The V8.2-D / V8.3-D machines predate three of the keys and never scan those cells.
	uint8_t GetColumns(uint16_t rowMask, bool skipNewKeys)
	{
		uint8_t result = 0;
		for(uint8_t row = 0; row < RowCount; row++) {
			if(!(rowMask & (1 << row))) {
				continue;
			}

			for(uint8_t col = 0; col < 8; col++) {
				if(skipNewKeys && ((row == 9 && col == 6) || (row == 11 && col == 6) || (row == 11 && col == 7))) {
					continue;
				}

				Buttons key = _keyboardMatrix[row][col];
				if(key != Buttons::None && IsPressed((uint8_t)key)) {
					result |= (1 << col);
				}
			}
		}
		return result;
	}
};
