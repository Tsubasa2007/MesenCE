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

	//Lets a script drive this keyboard through emu.setInput, the way the SB-2000's does.
	//Without it the matrix can only be reached from the host keyboard, so nothing that
	//needs a key press - which on these machines includes leaving the opening screen -
	//can be tested headlessly.
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
			{ "leftwin", Buttons::LeftWin },
			{ "rightwin", Buttons::RightWin },
			{ "menu", Buttons::Menu },
			{ "printscreen", Buttons::PrintScreen },
		};
	}

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

	//The VCD models scan the same matrix over a serial link instead of through $4207, and
	//report a single key as its matrix position. The reference emulator sweeps the whole
	//matrix without stopping, so the last pressed cell in row-major order is the one sent.
	//Returns -1 when nothing is held.
	int32_t GetLastPressedCell()
	{
		int32_t result = -1;
		for(uint8_t row = 0; row < RowCount; row++) {
			for(uint8_t col = 0; col < 8; col++) {
				Buttons key = _keyboardMatrix[row][col];
				if(key != Buttons::None && IsPressed((uint8_t)key)) {
					result = row * 8 + col;
				}
			}
		}
		return result;
	}

	//Ctrl/Shift/Alt as the serial protocol's modifier bits (6/5/7)
	uint8_t GetModifiers()
	{
		uint8_t result = 0;
		if(IsPressed((uint8_t)Buttons::Ctrl)) { result |= 1 << 6; }
		if(IsPressed((uint8_t)Buttons::Shift)) { result |= 1 << 5; }
		if(IsPressed((uint8_t)Buttons::Alt)) { result |= 1 << 7; }
		return result;
	}

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
