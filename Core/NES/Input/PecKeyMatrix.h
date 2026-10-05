#pragma once
#include "pch.h"
#include "NES/Input/Sb2kKeyboard.h"

//The 13x8 key matrix of the Dongda PEC-586 family's keyboard, which the LingTong machines use
//too. Scanned over $4016/$4017: $4016 = 0/1/5 resets the row counter, 2/3 rewinds the column,
//6/7 steps to the next row, and each $4017 read returns the next key of the row in bits 1 and 4.
//The machine owns the scan position; Sb2kKeyboard only says which keys are down.
class PecKeyMatrix
{
public:
	//[row][column], as the reference emulator lays it out
	static uint8_t GetKey(uint8_t row, uint8_t column)
	{
		using K = Sb2kKeyboard::Buttons;
		static constexpr uint8_t matrix[13][8] = {
			{ K::Shift, K::Tab, K::Grave, K::Ctrl, K::CapsLock, K::Alt, K::Space, K::Esc },
			{ K::F3, K::F1, K::F2, K::F8, K::F4, K::F5, K::F7, K::F6 },
			{ K::Z, K::Q, K::Num1, K::Enter, K::A, K::NumpadDot, K::Numpad0, K::NumpadPlus },
			{ K::X, K::W, K::Num2, K::Numpad9, K::S, K::Numpad6, K::Numpad3, K::NumpadMultiply },
			{ K::C, K::E, K::Num3, K::Numpad8, K::D, K::Numpad5, K::Numpad2, K::NumpadDivide },
			{ K::V, K::R, K::Num4, K::Numpad7, K::F, K::Numpad4, K::Numpad1, K::NumLock },
			{ K::B, K::T, K::Num5, K::RightBracket, K::G, K::NumpadEnter, K::Backslash, K::Backspace },
			{ K::Comma, K::I, K::Num8, K::O, K::K, K::L, K::Dot, K::Num9 },
			{ K::M, K::U, K::Num7, K::P, K::J, K::SemiColon, K::Slash, K::Num0 },
			{ K::N, K::Y, K::Num6, K::LeftBracket, K::H, K::Apostrophe, K::Equal, K::Minus },
			{ K::None, K::None, K::F9, K::NumpadMinus, K::None, K::F10, K::F11, K::F12 },
			{ K::Delete, K::End, K::Ins, K::Left, K::PageDown, K::Down, K::Right, K::Up },
			{ K::None, K::None, K::None, K::None, K::None, K::PageUp, K::Home, K::Pause },
		};
		return matrix[row][column];
	}

	//The next key of the scan: $12 when it is down
	static uint8_t Read(Sb2kKeyboard* kbd, uint8_t row, uint8_t& column)
	{
		uint8_t col = column++;
		if(row > 12 || col > 7) {
			return 0;
		}

		uint8_t key = GetKey(row, col);
		if(key == Sb2kKeyboard::None || !kbd) {
			return 0;
		}

		bool pressed = kbd->IsPressed(key) ||
			(key == Sb2kKeyboard::Shift && kbd->IsPressed(Sb2kKeyboard::RightShift)) ||
			(key == Sb2kKeyboard::Ctrl && kbd->IsPressed(Sb2kKeyboard::RightCtrl)) ||
			(key == Sb2kKeyboard::Alt && kbd->IsPressed(Sb2kKeyboard::RightAlt));
		return pressed ? 0x12 : 0;
	}

	static void Write(uint8_t value, uint8_t& row, uint8_t& column)
	{
		switch(value & 0x07) {
			case 0x00: case 0x01: case 0x05: row = 0; break;
			case 0x02: case 0x03: column = 0; break;
			case 0x06: case 0x07:
				if(++row > 12) {
					row = 0;
				}
				break;
		}
	}
};
