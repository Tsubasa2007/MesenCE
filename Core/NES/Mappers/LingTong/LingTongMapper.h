#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/NesConsole.h"
#include "NES/NesCpu.h"
#include "NES/NesControlManager.h"
#include "NES/NesMemoryManager.h"
#include "NES/Input/Sb2kKeyboard.h"
#include "NES/Input/Sb2kMouse.h"
#include "NES/Mappers/Bung/DrPcJrGameChips.h"
#include "NES/Mappers/Bbk/BbkLpcAudio.h"
#include "Utilities/Serializer.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/VirtualFile.h"
#include "Shared/MessageManager.h"

//灵童 (LingTong) SMART-128B / SMART-128C learning machines - Power Software's "POWER-BIOS
//v3.0". The iNES files in circulation carry mapper 174, which is an unrelated NTDEC
//multicart, so the two BIOSes are recognised by their PRG CRC32 instead.
//
// - SMART-128B: a 32KB boot ROM. Its menu boots DOS from a floppy, or hands the machine to
//   a plugged-in expansion or game card.
// - SMART-128C: a 2MB flash set. The same BIOS (a later revision) sits in its top 32KB, and
//   the rest holds a read-only ROM disk with SY-DOS 6.22, WPS, FoxBase, the sound and game
//   programs, the GB2312 font, and a second BIOS bank that DOS calls into. With no card
//   plugged in it boots that DOS straight from ROM.
//
//Recovered by reading the BIOS rather than ported - the reference emulator's mapper 174 only
//knows the 32KB BIOS, never forwards a write to its disk controller, and banks CHR on
//every nametable fetch.
//
//PRG, everything decoded at $5000-$57FF:
// - $5000 bit 6 set: DRAM, laid out by the low nibble.
//     0 ($50): $5400 names a 32KB page, as four 8KB banks - see the MMC3 note below.
//     8: a 32KB game, banked by writes to its own window.
//     A/B: UNROM, 128KB/256KB - writes to the window pick the 16KB at $8000, the last
//       16KB of the game is fixed at $C000.
//     otherwise a memory size ($52/$53/$54/$56/$51 = 128KB/256KB/512KB/1MB/2MB): $5400
//       names the 16KB at $8000 and $C000-$FFFF is the last 16KB of that size. The BIOS
//       settles on one after its memory test and copies its own $F800-$FFFF there, which is
//       where DOS's upper half and the vectors then live; every loader steps $5400 16KB at
//       a time. The memory test itself runs at $50 and counts 32KB pages until one already
//       holds what it wrote into an earlier one, which is where the page number wrapped.
//   The 128C's game menu loads a game into DRAM from $5400 = 0 and then starts it from a
//   stub that picks the layout: $50 for NROM and MMC3, $5A for UNROM.
// - Otherwise ROM, in 16KB pages of the 2MB space. $5100 bit 4 and bit 2 are page bits 6
//   and 5; $5000 bit 5 and bits 3-0 are page bits 4 and 3-0 for $8000-$BFFF, while
//   $C000-$FFFF always takes the top page (bits 4-0 set) of the same 512KB quarter. So the
//   BIOS's own $1F gives the font pages $60-$6F at $8000 and its code (page $7F) at $C000;
//   $1B puts DOS's BIOS bank (page $5F) at $C000; the ROM-disk loaders write $0B/$0F/$1B/$1F
//   to reach the four quarters. Every program that switches $C000-$FFFF away first copies
//   itself to RAM, which is what pinned this down. The ROM disk's own code computes $5100 as
//   ((page + $40) >> 3) | $0B and $5000 as ((page & $1F) + $10) & $AF.
// - $5080 bits 0-1: the game card slot, mapped over $8000-$FFFF. Empty, it reads as open
//   bus, which is how the BIOS decides there is no card.
// - $5100 bit 0 is the printer strobe (pulsed after a byte goes to $5200).
//
//CHR is 32KB of RAM in two 16KB halves, one per pattern table (PPU A12 picks the half).
//With $5180 bit 0 set, each half is four 4KB units and which one is seen depends on who is
//looking: the CPU gets unit ($5388 & 3), while the picture takes the unit named by the
//quarter of the nametable it last fetched from - so the 30 tile rows become four bands of
//256 private tiles and the screen is a bitmap. With bit 0 clear, $5388 is a 4KB unit number
//and $0000-$1FFF sees eight contiguous KB from there; the BIOS uses that only with the
//picture off, to copy text rows around.
//
//Keyboard: a PEC-586 style 13x8 matrix on $4016/$4017 - $4016 = 5 resets the row counter,
//2 rewinds the column, 7 steps to the next row, and each $4017 read returns the next key in
//bit 1 (and 4). The BIOS can also take an IRQ-driven serial keyboard at $50C0, but falls back
//to scanning the matrix whenever that one has sent nothing.
//
//MMC3 games (the 128C's game menu): the four 8KB banks at $50 are ($55C0-$55C3 & $5580) |
//(($5400 x 4 + bank position) & ~$5580) - so with $5580 = 0, as the BIOS leaves it, the page
//is straight. The games are patched: PRG R6/R7 go to $55C0/$55C1, the counter to
//$5280-$5283 (an MMC3's $C000/$C001/$E000/$E001), mirroring to $5300, and the CHR registers
//to a helper the launcher leaves at $7800 - see GameChrMode.
//
//Speech: a TMS5220-type LPC synthesizer (see BbkLpcAudio's LingTong variant). WPS's speech
//build (WPS /S) writes the stream to $5700 a whole byte at a time, waiting for bit 7 of a read
//to say the chip can take more. The stream frames itself - each record is a header ended by $FC
//and frames up to a stop code - and the 24 bytes of $FF after a phrase are stop codes too, so
//nothing here has to find phrase boundaries; a lone $FF inside a record is just data. (The
//reference emulator instead feeds this port four bits per $5x write, which suits its 中索
//disc games but turns WPS's bytes into resets.)
//
//Mouse: a serial device on $4017 bit 2, beside the key matrix's bit 1, clocked by the same
//strobes - each fall of $4016 bit 0 loads its next byte and the following reads shift it out
//from the top. The format is read from the only software that drives it, the 中索 discs' menu,
//which takes one byte per matrix row (bits 7-6 are the buttons in every byte, $40 left and
//$80 right; bits 5-4 say what the byte is):
//  00   a single step: bits 3-2 vertical (01 up, 11 down), bits 1-0 horizontal (01 left,
//       11 right); a byte of 0 means nothing happened
//  01   first of three: bit 3 down, bit 2 vertical bit 4, bit 1 right, bit 0 horizontal bit 4
//  10   vertical distance, bits 3-0
//  11   horizontal distance, bits 3-0 - the packet is complete
//The 128C's own software never reads it.
//
//Floppy: an Apple II style Disk II controller - $5600-$560F are its sixteen soft switches
//(stepper phases, motor, drive select, Q6/Q7), read-only, and the BIOS decodes 6-and-2 GCR
//itself. $5300 bit 3 reads high when no disk is in the drive, which the BIOS checks before
//every boot attempt. No disk images exist for this machine, so the drive is always empty.
//
//中索 (Zoson) disc games: the VCD machine these discs came with is this hardware, and each game
//.CDV is 2KB of code followed by the game (see ZosonCdvLoader). Opened on its own, there is no
//BIOS: at every power-on and reset the game goes into DRAM from its start, the code into the
//RAM at $6200, and the CPU starts at $6200 - which is all the machine's own disc loader does
//before jumping there. The code sets everything else up from the real registers.
//
//STAND-IN, not the machine's code: the disc menu starts a program by putting its number in
//$5FFF and jumping to $4800, into the machine's resident loader, which no dump has. In its
//place the mapper leaves a few bytes at $4800 that hand $5FFF to it (a write to $57FF), and
//then loads that program the way it loaded the first one and jumps to $6200. The number is
//the program's place in the disc's sorted file list - the menu itself is 0 - which is how
//every icon of the menus lines up with a file, gaps in the ST10xx names included; the files
//are read from the folder the opened file came from. Anything the real loader did besides
//(its own messages, a CD drive's timing, the BIOS entry points the menu also jumps to at
//$F600/$F780) is not there. ($6200, not
//$6000: the codes that leave a helper at $7800 copy it from $6250 on, and what sits at offset
//$50 of the file is the helper's jump table - the games call $7803 and $7806.)
class LingTongMapper : public BaseMapper
{
private:
	static constexpr uint32_t Crc128B = 0x1054DF22;
	static constexpr uint32_t Crc128C = 0xC95226A1;

	//The BIOS measures the DRAM rather than assuming it: it counts pages until one already
	//holds the pattern it wrote into an earlier one, which is where the address wrapped, shows
	//the total and picks the layout for it (256KB/512KB/1MB/2MB+ = $53/$54/$56/$51). A real
	//SMART-128C reports 2048KB at power-on. Nothing says what a 128B carries, so it keeps the
	//1MB the reference emulator gives both; the 中索 disc games fit in either.
	static constexpr uint32_t MaxDramSize = 0x200000;
	static constexpr uint32_t DramOffset = 0x2000;
	uint32_t _dramSize = 0x100000;

	uint8_t _reg5000 = 0;
	uint8_t _reg5080 = 0;
	uint8_t _reg5100 = 0x1F;
	uint8_t _reg5180 = 0;
	uint8_t _reg5388 = 0;
	uint8_t _reg5400 = 0;

	uint8_t _reg5580 = 0;
	uint8_t _prgBank[4] = {};
	uint8_t _chrBank[8] = {};

	//The scanline counter at $5280-$5283 is an MMC3's, so the register logic is borrowed
	DrPcJrMmc3 _mmc3;

	unique_ptr<BbkLpcAudio> _speech;
	void WriteSpeech(uint8_t value)
	{
		_speech->WriteData(value);
	}

	//A game's own bank register, written through its ROM window
	uint8_t _cartBank = 0;
	bool _cartRegs = false;

	//The quarter of the nametable the picture last fetched a tile from
	uint8_t _ntQuarter = 0;

	//Key matrix scan position
	uint8_t _kbdRow = 0;
	uint8_t _kbdColumn = 0;

	uint8_t _status5300 = 0;

	//The mouse, and the bytes it has queued: a single step, or a three-byte packet
	shared_ptr<Sb2kMouse> _mouse;
	uint8_t _mousePacket[3] = {};
	uint8_t _mousePacketSize = 0;
	uint8_t _mousePacketPos = 0;
	uint8_t _mouseShift = 0;
	bool _mouseStrobe = false;
	int32_t _mouseX = 0;
	int32_t _mouseY = 0;

	//The next byte for the shift register, starting a new report when the last is used up
	uint8_t NextMouseByte()
	{
		if(_mousePacketPos < _mousePacketSize) {
			return _mousePacket[_mousePacketPos++];
		}

		int8_t dx = 0, dy = 0;
		uint8_t buttons = 0;
		if(_mouse) {
			_mouse->TakeDelta(dx, dy, buttons);
		}
		_mouseX += dx;
		_mouseY += dy;
		uint8_t buttonBits = ((buttons & 0x01) ? 0x40 : 0) | ((buttons & 0x02) ? 0x80 : 0);

		_mousePacketPos = 0;
		if(std::abs(_mouseX) <= 1 && std::abs(_mouseY) <= 1) {
			_mousePacket[0] = buttonBits |
				(_mouseY < 0 ? 0x04 : (_mouseY > 0 ? 0x0C : 0)) |
				(_mouseX < 0 ? 0x01 : (_mouseX > 0 ? 0x03 : 0));
			_mousePacketSize = 1;
			_mouseX = _mouseY = 0;
		} else {
			//Up to 31 a packet; the rest waits for the next one
			int32_t x = std::clamp(_mouseX, -31, 31);
			int32_t y = std::clamp(_mouseY, -31, 31);
			_mouseX -= x;
			_mouseY -= y;
			uint8_t ax = (uint8_t)std::abs(x);
			uint8_t ay = (uint8_t)std::abs(y);
			_mousePacket[0] = buttonBits | 0x10 | (y > 0 ? 0x08 : 0) | ((ay & 0x10) ? 0x04 : 0) | (x > 0 ? 0x02 : 0) | ((ax & 0x10) ? 0x01 : 0);
			_mousePacket[1] = buttonBits | 0x20 | (ay & 0x0F);
			_mousePacket[2] = buttonBits | 0x30 | (ax & 0x0F);
			_mousePacketSize = 3;
		}
		return _mousePacket[_mousePacketPos++];
	}

	uint8_t ReadMouseBit()
	{
		uint8_t bit = (_mouseShift & 0x80) ? 0x04 : 0;
		_mouseShift <<= 1;
		return bit;
	}

	void WriteMouseStrobe(uint8_t value)
	{
		bool strobe = (value & 0x01) != 0;
		if(_mouseStrobe && !strobe) {
			_mouseShift = NextMouseByte();
		}
		_mouseStrobe = strobe;
	}

	//A 中索 disc game opened on its own: its start code and the game
	static constexpr uint16_t CdvStubAddress = 0x6200;
	static constexpr uint16_t LoaderAddress = 0x4800;
	static constexpr uint16_t LoaderRequestRegister = 0x57FF;
	vector<uint8_t> _cdvStub;
	vector<uint8_t> _cdvImage;
	bool IsCdv() { return !_cdvStub.empty(); }

	NesControlManager* NesControls() { return (NesControlManager*)_console->GetControlManager(); }

	void UpdatePrgMapping()
	{
		bool cartRegs = false;
		if(_reg5000 & 0x40) {
			uint8_t mode = _reg5000 & 0x0F;
			if(mode == 0x00) {
				//$50, what the memory test and the game launcher use: $5400 names a 32KB page,
				//cut into four 8KB banks. The bits of $5580 pick which bits of each bank number
				//come from $55C0-$55C3 rather than from the page and the bank's own position - all
				//of them for an MMC3 game (whose patched code writes those registers), none for the
				//BIOS. They replace the page's bits too: a 中索 disc game's start code leaves $5400
				//on the page it copied its graphics from, sets $5580 = $7F and starts the game from
				//$55C0-$55C3 alone.
				for(int i = 0; i < 4; i++) {
					uint32_t bank = (((uint32_t)_reg5400 * 4 | i) & ~(uint32_t)_reg5580) | (_prgBank[i] & _reg5580);
					MapDram((uint16_t)(0x8000 + i * 0x2000), (uint16_t)(0x9FFF + i * 0x2000), bank * 0x2000, true);
				}
			} else if(mode == 0x08) {
				//A 32KB game, banked by writes to the window itself
				MapDram(0x8000, 0xFFFF, (uint32_t)_cartBank * 0x8000, false);
				cartRegs = true;
			} else if(mode == 0x0A || mode == 0x0B) {
				//UNROM, 128KB or 256KB: writes to the window pick the 16KB at $8000, and the
				//last 16KB of the game stays at $C000. The bank number wraps at the game's size -
				//the 中索 discs' conversions write their bank with bit 4 set ($10-$17).
				uint32_t size = mode == 0x0A ? 0x20000 : 0x40000;
				MapDram(0x8000, 0xBFFF, ((uint32_t)_cartBank * 0x4000) & (size - 1), false);
				MapDram(0xC000, 0xFFFF, size - 0x4000, false);
				cartRegs = true;
			} else {
				//Configured for a memory size - the BIOS's own setting after the memory test
				//($53/$54/$56/$51 = 256KB/512KB/1MB/2MB, $52 = 128KB as the game launcher uses
				//it). $5400 names the 16KB page at $8000, and $C000-$FFFF is the last 16KB of
				//that size: DOS's upper half and the BIOS routines it calls are copied there once,
				//after the memory test, and every loader steps $5400 16KB at a time.
				//$5580 overrides bits of each 8KB bank from $55C0-$55C3 here too, as at $50:
				//the ROM disk's database program runs at $56 with $5580 = $FF and swaps its 16KB
				//overlays in with $55C0/$55C1 (leaving $55C3 = $7F, the last 8KB anyway), and the 中索 discs'
				//games page CHR data through $C000 with $5580 = $1F, then $0F and $55C2 = $0E.
				static constexpr uint32_t sizes[16] = {
					0, 0x200000, 0x20000, 0x40000, 0x80000, 0x80000, 0x100000, 0x100000,
					0, 0, 0, 0, 0x80000, 0x80000, 0x80000, 0x80000
				};
				uint32_t lastBank = sizes[mode] / 0x2000 - 1;
				uint32_t defaults[4] = { (uint32_t)_reg5400 * 2, (uint32_t)_reg5400 * 2 + 1, lastBank - 1, lastBank };
				for(int i = 0; i < 4; i++) {
					//Addresses wrap at the size: a disc game set to 128KB pages with $5400 = $2A
					uint32_t bank = ((defaults[i] & ~(uint32_t)_reg5580) | (_prgBank[i] & _reg5580)) & lastBank;
					MapDram((uint16_t)(0x8000 + i * 0x2000), (uint16_t)(0x9FFF + i * 0x2000), bank * 0x2000, true);
				}
			}
		}
		if(cartRegs != _cartRegs) {
			_cartRegs = cartRegs;
			if(cartRegs) {
				AddRegisterRange(0x8000, 0xFFFF, MemoryOperation::Write);
			} else {
				RemoveRegisterRange(0x8000, 0xFFFF, MemoryOperation::Write);
			}
		}
		if(_reg5000 & 0x40) {
			return;
		}

		if(_reg5080 & 0x03) {
			//Game card slot, empty
			RemoveCpuMemoryMapping(0x8000, 0xFFFF);
			return;
		}

		uint32_t quarter = ((_reg5100 & 0x10) ? 0x40 : 0) | ((_reg5100 & 0x04) ? 0x20 : 0);
		uint32_t lowPage = quarter | ((_reg5000 & 0x20) ? 0x10 : 0) | (_reg5000 & 0x0F);
		uint32_t highPage = quarter | 0x1F;
		SetCpuMemoryMapping(0x8000, 0xBFFF, PrgMemoryType::PrgRom, (lowPage * 0x4000) & (_prgSize - 1), MemoryAccessType::Read);
		SetCpuMemoryMapping(0xC000, 0xFFFF, PrgMemoryType::PrgRom, (highPage * 0x4000) & (_prgSize - 1), MemoryAccessType::Read);
	}

	//DRAM addresses wrap at the size fitted
	void MapDram(uint16_t start, uint16_t end, uint32_t offset, bool writable)
	{
		SetCpuMemoryMapping(start, end, PrgMemoryType::WorkRam, DramOffset + (offset & (_dramSize - 1)),
			writable ? MemoryAccessType::ReadWrite : MemoryAccessType::Read);
	}

	bool BitmapMode() { return (_reg5180 & 0x01) != 0; }

	//$5180 bit 1: an MMC3 game's eight 1KB CHR banks, $5380-$5387, each naming one of the 32
	//1KB slots of CHR RAM - for the picture and the CPU alike. The game's CHR stays in DRAM
	//after its PRG, and a helper the launcher leaves at $7800 keeps the slots as a cache: the
	//patched game writes its MMC3 CHR numbers to $7FF0-$7FF5 and calls it, and it copies any
	//bank that is not resident in through $2007 before pointing a register at it.
	bool GameChrMode() { return (_reg5180 & 0x02) != 0; }

	uint32_t GameChrAddress(uint16_t addr)
	{
		return ((uint32_t)_chrBank[addr >> 10] * 0x400 + (addr & 0x3FF)) & (_chrRamSize - 1);
	}

	void UpdateIrq()
	{
		if(_mmc3.IrqPending()) {
			_console->GetCpu()->SetIrqSource(IRQSource::External);
		} else {
			_console->GetCpu()->ClearIrqSource(IRQSource::External);
		}
	}

	uint32_t CpuChrAddress(uint16_t addr)
	{
		if(BitmapMode()) {
			return ((addr & 0x1000) ? 0x4000 : 0) | ((_reg5388 & 0x03) << 12) | (addr & 0x0FFF);
		}
		return ((uint32_t)_reg5388 * 0x1000 + addr) & 0x7FFF;
	}

	uint32_t PictureChrAddress(uint16_t addr)
	{
		if(BitmapMode()) {
			return ((addr & 0x1000) ? 0x4000 : 0) | (_ntQuarter << 12) | (addr & 0x0FFF);
		}
		return CpuChrAddress(addr);
	}

	//PEC-586 matrix, as the reference emulator lays it out: [row][column]
	uint8_t MatrixKey(uint8_t row, uint8_t column)
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

	uint8_t ReadKeyMatrix()
	{
		uint8_t column = _kbdColumn++;
		if(_kbdRow > 12 || column > 7) {
			return 0;
		}

		uint8_t key = MatrixKey(_kbdRow, column);
		if(key == Sb2kKeyboard::None) {
			return 0;
		}

		shared_ptr<Sb2kKeyboard> kbd = NesControls()->GetControlDevice<Sb2kKeyboard>();
		bool pressed = kbd && (kbd->IsPressed(key) ||
			(key == Sb2kKeyboard::Shift && kbd->IsPressed(Sb2kKeyboard::RightShift)) ||
			(key == Sb2kKeyboard::Ctrl && kbd->IsPressed(Sb2kKeyboard::RightCtrl)) ||
			(key == Sb2kKeyboard::Alt && kbd->IsPressed(Sb2kKeyboard::RightAlt)));
		return pressed ? 0x12 : 0;
	}

	void WriteKeyMatrix(uint8_t value)
	{
		switch(value & 0x07) {
			case 0x00: case 0x01: case 0x05: _kbdRow = 0; break;
			case 0x02: case 0x03: _kbdColumn = 0; break;
			case 0x06: case 0x07:
				if(++_kbdRow > 12) {
					_kbdRow = 0;
				}
				break;
		}
	}

protected:
	uint16_t GetPrgPageSize() override { return 0x4000; }
	uint16_t GetChrPageSize() override { return 0x1000; }
	uint32_t GetChrRamSize() override { return 0x8000; }
	uint16_t GetChrRamPageSize() override { return 0x1000; }
	uint32_t GetWorkRamSize() override { return DramOffset + MaxDramSize; }
	uint32_t GetWorkRamPageSize() override { return 0x2000; }
	uint32_t GetSaveRamSize() override { return 0; }

	//$4100-$4FFF and $5800-$5FFF are plain RAM; the register block keeps a copy of what was
	//written, which is what reads of an address with no input wired to it return
	uint32_t GetMapperRamSize() override { return 0x2000; }

	uint16_t RegisterStartAddress() override { return 0x5000; }
	uint16_t RegisterEndAddress() override { return 0x57FF; }
	bool AllowRegisterRead() override { return true; }
	bool EnableCustomVramRead() override { return true; }
	bool EnableVramAddressHook() override { return true; }
	bool EnableCpuClockHook() override { return true; }

	//No 2C02 "read one dot before vblank" race, as with the other learning machines' clone PPUs.
	//The 中索 discs' CHR helper waits on $2002 in an 8-cycle loop (its branch crosses a page),
	//which divides Dendy's 106392-dot frame exactly: the read lands on that one dot every frame
	//and the flag never comes.
	bool EnablePpuNmiSuppressRace() override { return false; }

	void ProcessCpuClock() override
	{
		BaseProcessCpuClock();
		_speech->Clock();
	}

	void NotifyVramAddressChange(uint16_t addr) override
	{
		_mmc3.ClockA12(addr, _console->GetMasterClock());
		UpdateIrq();
	}

	void GetMemoryRanges(MemoryRanges& ranges) override
	{
		BaseMapper::GetMemoryRanges(ranges);
		ranges.AddHandler(MemoryOperation::Write, 0x4016);
		ranges.AddHandler(MemoryOperation::Read, 0x4017);
		ranges.SetAllowOverride();
	}

	void InitMapper(RomData& romData) override
	{
		romData.Info.System = GameSystem::Dendy;
		_dramSize = romData.Info.Hash.PrgCrc32 == Crc128C ? 0x200000 : 0x100000;
		UpdatePrgMapping();
		if(!romData.CdvHeader.empty()) {
			_cdvStub = romData.CdvHeader;
			_cdvImage = romData.PrgRom;
		}
	}

	void OnAfterResetPowerOn() override
	{
		if(!IsCdv()) {
			return;
		}

		//What the disc loader leaves behind: the machine as it powers up, the game in DRAM from
		//its start, the start code at $6200, and a jump to it. A reset does the same again, so
		//a game that wrote over its own DRAM starts clean.
		ResetRegisters();
		LayDownCdv(_cdvStub, _cdvImage);
		_console->GetCpu()->GetState().PC = CdvStubAddress;

		//The stand-in loader (see the class comment): SEI / LDA $5FFF / STA $57FF / JMP $6200
		static constexpr uint8_t loader[] = {
			0x78,
			0xAD, 0xFF, 0x5F,
			0x8D, LoaderRequestRegister & 0xFF, LoaderRequestRegister >> 8,
			0x4C, CdvStubAddress & 0xFF, CdvStubAddress >> 8
		};
		memcpy(_mapperRam + (LoaderAddress - 0x4000), loader, sizeof(loader));
	}

	void LayDownCdv(const vector<uint8_t>& stub, const vector<uint8_t>& image)
	{
		memcpy(_workRam + DramOffset, image.data(), std::min((size_t)_dramSize, image.size()));
		memcpy(_workRam + CdvStubAddress - 0x6000, stub.data(), std::min((size_t)(0x8000 - CdvStubAddress), stub.size()));
	}

	//Program n of the disc - the folder's .CDV files in name order, the menu being 0. Falls
	//back to starting the opened file again when there is no such program.
	void LoadDiscProgram(uint8_t n)
	{
		vector<uint8_t> file;
		string path;
		string folder = FolderUtilities::GetFolderName(_emu->GetRomInfo().RomFile.GetFilePath());
		vector<string> files = FolderUtilities::GetFilesInFolder(folder, { ".cdv" }, false);
		std::sort(files.begin(), files.end(), [](const string& a, const string& b) {
			string ua = FolderUtilities::GetFilename(a, true);
			string ub = FolderUtilities::GetFilename(b, true);
			std::transform(ua.begin(), ua.end(), ua.begin(), ::toupper);
			std::transform(ub.begin(), ub.end(), ub.begin(), ::toupper);
			return ua < ub;
		});
		if(n < files.size()) {
			path = files[n];
			VirtualFile(path).ReadFile(file);
		}

		ResetRegisters();
		if(file.size() > 0x800 && ((file.size() - 0x800) & 0x3FFF) == 0 && file.size() - 0x800 <= _dramSize) {
			MessageManager::Log("[CDV] Stand-in loader: program " + std::to_string(n) + " = " + FolderUtilities::GetFilename(path, true));
			vector<uint8_t> stub(file.begin(), file.begin() + 0x800);
			vector<uint8_t> image(file.begin() + 0x800, file.end());
			LayDownCdv(stub, image);
		} else {
			MessageManager::Log("[CDV] Stand-in loader: no program " + std::to_string(n) + " beside the opened file - starting it again");
			LayDownCdv(_cdvStub, _cdvImage);
		}
	}

	void InitMapper() override
	{
		_romInfo.System = GameSystem::Dendy;

		AddRegisterRange(0x4016, 0x4016, MemoryOperation::Write);
		AddRegisterRange(0x4017, 0x4017, MemoryOperation::Read);

		SetCpuMemoryMapping(0x4100, 0x4FFF, PrgMemoryType::MapperRam, 0x0100, MemoryAccessType::ReadWrite);
		SetCpuMemoryMapping(0x5800, 0x5FFF, PrgMemoryType::MapperRam, 0x1800, MemoryAccessType::ReadWrite);
		SetCpuMemoryMapping(0x6000, 0x7FFF, PrgMemoryType::WorkRam, 0, MemoryAccessType::ReadWrite);

		//Only for the debugger's views - the picture and the CPU are routed separately below
		SetPpuMemoryMapping(0x0000, 0x1FFF, ChrMemoryType::ChrRam, 0, MemoryAccessType::ReadWrite);
		SetMirroringType(MirroringType::Vertical);

		_speech.reset(new BbkLpcAudio(_console, BbkLpcAudio::LpcVariant::LingTong));
		ResetRegisters();

		//Its own port, so both joypads stay plugged in; the buttons come from the physical
		//mouse, so it needs no key setup
		_mouse.reset(new Sb2kMouse(_emu, BaseControlDevice::MapperInputPort, KeyMappingSet()));
		_console->GetControlManager()->AddSystemControlDevice(_mouse);
	}

	void ResetRegisters()
	{
		_reg5000 = 0;
		_reg5080 = 0;
		_reg5100 = 0x1F;
		_reg5180 = 0;
		_reg5388 = 0;
		_reg5400 = 0;
		_cartBank = 0;
		_cartRegs = false;
		_reg5580 = 0;
		memset(_prgBank, 0, sizeof(_prgBank));
		memset(_chrBank, 0, sizeof(_chrBank));
		_mmc3.Reset();
		_speech->Reset();
		_ntQuarter = 0;
		_kbdRow = 0;
		_kbdColumn = 0;
		_status5300 = 0;
		UpdatePrgMapping();
	}

	uint8_t MapperReadVram(uint16_t addr, MemoryOperationType type) override
	{
		if(addr < 0x2000) {
			if(GameChrMode()) {
				return _chrRam[GameChrAddress(addr)];
			}
			uint32_t offset = type == MemoryOperationType::PpuRenderingRead ? PictureChrAddress(addr) : CpuChrAddress(addr);
			return _chrRam[offset & (_chrRamSize - 1)];
		}

		if(type == MemoryOperationType::PpuRenderingRead && addr < 0x3000 && (addr & 0x3FF) < 0x3C0) {
			_ntQuarter = (addr >> 8) & 0x03;
		}
		return InternalReadVram(addr);
	}

	void MapperWriteVram(uint16_t addr, uint8_t value) override
	{
		if(addr < 0x2000) {
			_chrRam[GameChrMode() ? GameChrAddress(addr) : (CpuChrAddress(addr) & (_chrRamSize - 1))] = value;
			return;
		}
		InternalWriteVram(addr, value);
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		if(addr == 0x4017) {
			return (NesControls()->ReadRam(addr) & ~0x16) | ReadKeyMatrix() | ReadMouseBit();
		}

		switch(addr) {
			case 0x5300:
				//Bit 3: no disk in the drive. Bit 7: printer ready. Bit 4 is the serial
				//keyboard's clock line - toggled so the LED handshake never stalls.
				_status5300 ^= 0x10;
				return 0x88 | _status5300;

			case 0x50C0:
			case 0x50C1:
				//Serial keyboard data - nothing ever arrives
				return 0;

			case 0x5700:
				return _speech->IsReady() ? 0x80 : 0x00;
		}

		if(addr >= 0x5600 && addr <= 0x56FF) {
			//Disk II soft switches. With no disk the read latch only ever holds noise that
			//never forms an address mark, so the BIOS's searches run out and report an error.
			return (addr & 0x0F) == 0x0C ? 0xFF : 0;
		}

		return _mapperRam[addr - 0x4000];
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr == 0x4016) {
			NesControls()->WriteRam(addr, value);
			WriteKeyMatrix(value);
			WriteMouseStrobe(value);
			return;
		}

		if(addr >= 0x8000) {
			_cartBank = value;
			UpdatePrgMapping();
			return;
		}

		_mapperRam[addr - 0x4000] = value;

		switch(addr) {
			case 0x5000: _reg5000 = value; UpdatePrgMapping(); break;
			case 0x5080: _reg5080 = value; UpdatePrgMapping(); break;
			case 0x5100: _reg5100 = value; UpdatePrgMapping(); break;
			case 0x5180: _reg5180 = value; break;
			case 0x5400: _reg5400 = value; UpdatePrgMapping(); break;

			case 0x5300:
				switch(value) {
					case 0x02: SetMirroringType(MirroringType::ScreenAOnly); break;
					case 0x03: SetMirroringType(MirroringType::ScreenBOnly); break;
					default: SetMirroringType((value & 0x80) ? MirroringType::Vertical : MirroringType::Horizontal); break;
				}
				break;

			case 0x5388:
			case 0x538C:
				_reg5388 = value;
				if(GameChrMode()) {
					//A 4KB unit for one pattern table - its four slots at once. The 中索 discs'
					//MMC1 conversions bank CHR this way ($5388 for $0000, $538C for $1000);
					//the 128C itself only ever writes $5388 with the slots off.
					uint8_t first = (addr & 0x04) ? 4 : 0;
					for(int i = 0; i < 4; i++) {
						_chrBank[first + i] = (uint8_t)(value * 4 + i);
					}
				}
				break;

			case 0x5380: case 0x5381: case 0x5382: case 0x5383:
			case 0x5384: case 0x5385: case 0x5386: case 0x5387:
				_chrBank[addr & 0x07] = value;
				break;

			case 0x5580: _reg5580 = value; UpdatePrgMapping(); break;

			case 0x5700: WriteSpeech(value); break;

			case LoaderRequestRegister:
				if(IsCdv()) {
					LoadDiscProgram(value);
				}
				break;

			case 0x55C0: case 0x55C1: case 0x55C2: case 0x55C3:
				_prgBank[addr & 0x03] = value;
				UpdatePrgMapping();
				break;

			case 0x5280: case 0x5281: case 0x5282: case 0x5283:
				//Latch, reload, disable, enable - an MMC3's $C000/$C001/$E000/$E001
				_mmc3.Write((uint16_t)(0xC000 | ((addr & 0x02) << 12) | (addr & 0x01)), value);
				UpdateIrq();
				break;
		}
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SV(_reg5000);
		SV(_reg5080);
		SV(_reg5100);
		SV(_reg5180);
		SV(_reg5388);
		SV(_reg5400);
		SV(_cartBank);
		SV(_reg5580);
		SVArray(_prgBank, 4);
		SVArray(_chrBank, 8);
		SV(_mmc3);
		SV(_speech);
		SV(_ntQuarter);
		SV(_kbdRow);
		SV(_kbdColumn);
		SV(_status5300);
		SVArray(_mousePacket, 3);
		SV(_mousePacketSize);
		SV(_mousePacketPos);
		SV(_mouseShift);
		SV(_mouseStrobe);
		SV(_mouseX);
		SV(_mouseY);

		if(!s.IsSaving()) {
			_cartRegs = false;
			RemoveRegisterRange(0x8000, 0xFFFF, MemoryOperation::Write);
			UpdatePrgMapping();
		}
	}

public:
	static bool IsLingTong(uint32_t prgCrc) { return prgCrc == Crc128B || prgCrc == Crc128C; }
};
