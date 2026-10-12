#pragma once
#include "pch.h"
#include "NES/Input/Sb2kKeyboard.h"
#include "Utilities/ISerializable.h"
#include "Utilities/Serializer.h"

//The Subor keyboard's scan, for a machine that answers it from its own keyboard rather than
//through a SuborKeyboard device: 13 rows of 8 keys, read four at a time. $4016 bit 2 enables
//the keyboard, bit 1 picks the half of the row, a fall of bit 0 goes back to row 0, and each
//return from the second half to the first steps to the next row. A read returns the four keys
//in bits 1-4, low when down. The layout is SuborKeyboard's; the machine owns the scan position
//and Sb2kKeyboard only says which keys are down.
class SuborKeyMatrix final : public ISerializable
{
private:
	uint8_t _row = 0;
	uint8_t _column = 0;
	bool _strobe = false;

	static uint8_t GetKey(uint8_t index)
	{
		using K = Sb2kKeyboard::Buttons;
		static constexpr uint8_t matrix[104] = {
			K::Num4, K::G, K::F, K::C, K::F2, K::E, K::Num5, K::V,
			K::Num2, K::D, K::S, K::End, K::F1, K::W, K::Num3, K::X,
			K::Ins, K::Backspace, K::PageDown, K::Right, K::F8, K::PageUp, K::Delete, K::Home,
			K::Num9, K::I, K::L, K::Comma, K::F5, K::O, K::Num0, K::Dot,
			K::RightBracket, K::Enter, K::Up, K::Left, K::F7, K::LeftBracket, K::Backslash, K::Down,
			K::Q, K::CapsLock, K::Z, K::Tab, K::Esc, K::A, K::Num1, K::Ctrl,
			K::Num7, K::Y, K::K, K::M, K::F4, K::U, K::Num8, K::J,
			K::Minus, K::SemiColon, K::Apostrophe, K::Slash, K::F6, K::P, K::Equal, K::Shift,
			K::T, K::H, K::N, K::Space, K::F3, K::R, K::Num6, K::B,
			K::Numpad6, K::NumpadEnter, K::Numpad4, K::Numpad8, K::None, K::None, K::None, K::None,
			K::Alt, K::Numpad4, K::Numpad7, K::F11, K::F12, K::Numpad1, K::Numpad2, K::Numpad8,
			K::NumpadMinus, K::NumpadPlus, K::NumpadMultiply, K::Numpad9, K::F10, K::Numpad5, K::NumpadDivide, K::NumLock,
			K::Grave, K::Numpad6, K::Pause, K::Space, K::F9, K::Numpad3, K::NumpadDot, K::Numpad0
		};
		return matrix[index];
	}

	static bool IsDown(Sb2kKeyboard* kbd, uint8_t key)
	{
		return key != Sb2kKeyboard::None && (kbd->IsPressed(key) ||
			(key == Sb2kKeyboard::Shift && kbd->IsPressed(Sb2kKeyboard::RightShift)) ||
			(key == Sb2kKeyboard::Ctrl && kbd->IsPressed(Sb2kKeyboard::RightCtrl)) ||
			(key == Sb2kKeyboard::Alt && kbd->IsPressed(Sb2kKeyboard::RightAlt)));
	}

public:
	void Reset()
	{
		_row = 0;
		_column = 0;
		_strobe = false;
	}

	void Write(uint8_t value)
	{
		bool strobe = (value & 0x01) != 0;
		if(_strobe && !strobe) {
			_row = 0;
			_column = 0;
		}
		_strobe = strobe;

		uint8_t prevColumn = _column;
		_column = (value >> 1) & 0x01;
		if((value & 0x04) && !_column && prevColumn) {
			_row = (_row + 1) % 13;
		}
	}

	uint8_t Read(Sb2kKeyboard* kbd)
	{
		uint8_t keys = 0;
		if(kbd) {
			for(int i = 0; i < 4; i++) {
				if(IsDown(kbd, GetKey(_row * 8 + _column * 4 + i))) {
					keys |= 1 << i;
				}
			}
		}
		if(_row == 9 && _column) {
			//Row 9's last bit says the keyboard is the extended one, with rows 10-12
			keys |= 0x01;
		}
		return (uint8_t)((~keys << 1) & 0x1E);
	}

	void Serialize(Serializer& s) override
	{
		SV(_row);
		SV(_column);
		SV(_strobe);
	}
};
