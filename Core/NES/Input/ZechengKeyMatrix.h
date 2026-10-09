#pragma once
#include "pch.h"
#include "NES/Input/Sb2kKeyboard.h"

//The 11x8 key matrix of the ZeCheng-type keyboard used with some learning cartridges on the
//PEC-586 board (see DongdaPec586). Scanned over $4016/$4017: $4016 = 5 resets the row and leaves
//the arming bit clear, 0 resets the row and sets it; 3 steps to the next row while it is clear
//(or only rewinds the column while it is set), 6 steps while it is set; 2 and 7 rewind the column.
//Each $4017 read returns the next key of the row in bit 1, set while the key is down. The machine
//owns the scan position; Sb2kKeyboard only says which keys are down.
//
//The reference emulator has rows 0-9 and no use for $02. The cartridges' own key table (one byte
//per row*8 + 7-column) has row 10 as well - the keypad operators and F9-F12, which the menus ask
//for - and a scanner that writes 0, 2, 3, 2 and then 6, 2 per row, which reads only row 0 unless
//$02 rewinds the column. The table also puts the keypad + and * in row 10 rather than row 8, and
//the keypad 5 in row 7.
class ZechengKeyMatrix
{
public:
	//[row][column]
	static uint8_t GetKey(uint8_t row, uint8_t column)
	{
		using K = Sb2kKeyboard::Buttons;
		static constexpr uint8_t matrix[11][8] = {
			{ K::Z, K::A, K::Q, K::Num1, K::Shift, K::CapsLock, K::Tab, K::Grave },
			{ K::C, K::D, K::E, K::Num3, K::X, K::S, K::W, K::Num2 },
			{ K::B, K::G, K::T, K::Num5, K::V, K::F, K::R, K::Num4 },
			{ K::M, K::J, K::U, K::Num7, K::N, K::H, K::Y, K::Num6 },
			{ K::Dot, K::L, K::O, K::Num9, K::Comma, K::K, K::I, K::Num8 },
			{ K::Backslash, K::Apostrophe, K::LeftBracket, K::Minus, K::Slash, K::SemiColon, K::P, K::Num0 },
			{ K::End, K::Left, K::Home, K::None, K::Backspace, K::Enter, K::RightBracket, K::Equal },
			{ K::PageDown, K::Right, K::PageUp, K::Esc, K::Down, K::Numpad5, K::Up, K::None },
			{ K::Delete, K::None, K::None, K::NumpadEnter, K::Ins, K::Space, K::Alt, K::Ctrl },
			{ K::F8, K::F7, K::F6, K::F5, K::F4, K::F3, K::F2, K::F1 },
			{ K::NumpadPlus, K::NumpadMinus, K::NumpadMultiply, K::NumpadDivide, K::F12, K::F11, K::F10, K::F9 },
		};
		return matrix[row][column];
	}

	//The next key of the scan: $02 when it is down
	static uint8_t Read(Sb2kKeyboard* kbd, uint8_t row, uint8_t& column)
	{
		if(row > 10) {
			return 0;
		}
		uint8_t col = column++;
		if(col > 7 || !kbd) {
			return 0;
		}

		uint8_t key = GetKey(row, col);
		if(key == Sb2kKeyboard::None) {
			return 0;
		}

		bool pressed = kbd->IsPressed(key) ||
			(key == Sb2kKeyboard::Shift && kbd->IsPressed(Sb2kKeyboard::RightShift)) ||
			(key == Sb2kKeyboard::Ctrl && kbd->IsPressed(Sb2kKeyboard::RightCtrl)) ||
			(key == Sb2kKeyboard::Alt && kbd->IsPressed(Sb2kKeyboard::RightAlt));
		return pressed ? 0x02 : 0;
	}

	static void Write(uint8_t value, uint8_t& row, uint8_t& column, bool& armed)
	{
		switch(value) {
			case 0x00: armed = true; row = 0; break;
			case 0x02: column = 0; break;
			case 0x03:
				if(armed) {
					column = 0;
				} else if(++row > 12) {
					row = 0;
				}
				break;
			case 0x05: armed = false; row = 0; break;
			case 0x06:
				if(armed && ++row > 12) {
					row = 0;
				}
				break;
			case 0x07: column = 0; break;
		}
	}
};
