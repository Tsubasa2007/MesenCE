#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/NesConsole.h"
#include "NES/NesMemoryManager.h"
#include "NES/NesControlManager.h"
#include "NES/Input/Sb2kKeyboard.h"
#include "NES/Input/ZechengKeyMatrix.h"

//光达 (GuangDa) learning cartridge "GD-24in1" (Bit Corp code, 1989): a 1MB ROM with a BASIC, a
//pinyin input method and its hanzi font, 8KB of RAM at $6000 and 8KB of CHR RAM. The dump has no
//header of its own, so it is recognised by its PRG CRC32.
//
// - PRG: writes to $5000-$5FFF pick the 32KB page at $8000 (bits 0-4). Every page that the
//   program switches from keeps the same switching code at the same address, and the first
//   32KB's reset code goes to page 8.
// - Bit 7: a bitmap of one bit per pixel, for the text screens (the BASICs, the editors and the
//   hanzi programs). Each tile then holds two character cells - its first 8 bytes the left one,
//   the last 8 the right one - and the nametable names every tile twice in a row: on an even
//   column both of the PPU's planes read the first 8 bytes, on an odd column the last 8. The top
//   half of the screen ($2000-$21FF) takes its 256 tiles from the first 4KB and the bottom half
//   from the second. Both come from the address of the last nametable fetch (bits 0 and 9), so a
//   sprite - fetched after the PPU's dummy nametable reads at the row's start - takes the first 8
//   bytes from the half it is in; the BASIC draws its cursor into both halves.
// - Bit 6 is set with bit 7 by one program (a hanzi input lesson) and changes nothing visible.
// - $5500 read: the cartridge's own input port. Bit 7 is its cassette input - one program's lessons
//   come on tape and are read from it by timing the signal's edges - and bit 6 says which machine
//   the cartridge sits in: clear, the keyboard answers on $4017 bit 1 and the tape comes in here;
//   set, the programs look for the keys on $4017 bit 6 and the tape on $4016 bit 1 (the Family
//   BASIC data recorder's input). Only the first agrees with the menus, which always read bit 1,
//   so bit 6 reads clear. Bits 4-5 are read as a setting by one program; they read clear too, and
//   with no tape bit 7 stays low.
// - Keyboard: the ZeCheng-type 11x8 matrix (see ZechengKeyMatrix), its key table laid out the same
//   way. A $4016 write with bit 1 clear goes back to row 0, bit 2 going high steps to the next row,
//   and every write rewinds the column; each $4017 read returns the next key of the row in bit 1,
//   set while the key is down. The cartridge has three scanners and this is the rule that serves
//   them all: the menus write 0, then 3, 2, eight reads and 7 per row; the BASIC 0, 2, then 3, 2,
//   eight reads, 6 and 2; the typing lessons (page 14) 5, 7, then 6, eight reads, 3 and 7.
class BitCorpGd24 : public BaseMapper
{
private:
	static constexpr uint32_t Crc = 0xF5C1F4A1;
	uint8_t _reg = 0;
	uint8_t _kbdRow = 0;
	uint8_t _kbdColumn = 0;
	uint8_t _kbdStrobe = 0;
	uint8_t _column = 0;
	uint8_t _half = 0;

	NesControlManager* NesControls() { return (NesControlManager*)_console->GetControlManager(); }

protected:
	uint16_t GetPrgPageSize() override { return 0x8000; }
	uint16_t GetChrPageSize() override { return 0x2000; }
	uint32_t GetChrRamSize() override { return 0x2000; }
	uint32_t GetWorkRamSize() override { return 0x2000; }
	uint16_t RegisterStartAddress() override { return 0x5000; }
	uint16_t RegisterEndAddress() override { return 0x5FFF; }
	bool AllowRegisterRead() override { return true; }
	bool EnableCustomVramRead() override { return true; }

	void GetMemoryRanges(MemoryRanges& ranges) override
	{
		BaseMapper::GetMemoryRanges(ranges);
		ranges.AddHandler(MemoryOperation::Write, 0x4016);
		ranges.AddHandler(MemoryOperation::Read, 0x4017);
		ranges.SetAllowOverride();
	}

	void InitMapper() override
	{
		_reg = 0;
		_kbdRow = 0;
		_kbdColumn = 0;
		_kbdStrobe = 0;
		_column = 0;
		_half = 0;
		RemoveRegisterRange(0x5000, 0x5FFF, MemoryOperation::Read);
		AddRegisterRange(0x5500, 0x5500, MemoryOperation::Read);
		AddRegisterRange(0x4016, 0x4016, MemoryOperation::Write);
		AddRegisterRange(0x4017, 0x4017, MemoryOperation::Read);
		SelectPrgPage(0, 0);
		SelectChrPage(0, 0);
	}

	void Reset(bool softReset) override
	{
		BaseMapper::Reset(softReset);
		_reg = 0;
		_kbdRow = 0;
		_kbdColumn = 0;
		_kbdStrobe = 0;
		SelectPrgPage(0, 0);
	}

	uint8_t MapperReadVram(uint16_t addr, MemoryOperationType type) override
	{
		if(type == MemoryOperationType::PpuRenderingRead) {
			if((addr & 0x3000) == 0x2000 && (addr & 0x3FF) < 0x3C0) {
				_column = addr & 0x01;
				_half = (addr >> 9) & 0x01;
			} else if(addr < 0x2000 && (_reg & 0x80)) {
				addr = (addr & ~0x1008) | (_half << 12) | (_column << 3);
			}
		}
		return InternalReadVram(addr);
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		if(addr == 0x5500) {
			return _console->GetMemoryManager()->GetOpenBus(0x0F);
		}

		//$4017: the joypad's bit 0 and the key under the scan in bit 1. The light gun on port 2 is plugged in only for
		//the games that use it (the two light gun games, page $1F): its light bit (3) reads 1 while it sees nothing,
		//and one editor's start-up waits for $4017 & $1A to read 0 - with the gun in, it never starts. Elsewhere its two
		//bits read 0, as with the gun out.
		uint8_t value = NesControls()->ReadRam(addr) & ~0x02;
		if((_reg & 0x1F) != 0x1F) {
			value &= ~0x18;
		}
		Sb2kKeyboard* kbd = NesControls()->GetControlDevice<Sb2kKeyboard>().get();
		return value | ZechengKeyMatrix::Read(kbd, _kbdRow, _kbdColumn);
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr == 0x4016) {
			NesControls()->WriteRam(addr, value);
			if(!(value & 0x02)) {
				_kbdRow = 0;
			} else if((value & 0x04) && !(_kbdStrobe & 0x04) && _kbdRow < 11) {
				_kbdRow++;
			}
			_kbdStrobe = value;
			_kbdColumn = 0;
			return;
		}
		_reg = value;
		SelectPrgPage(0, value & 0x1F);
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SV(_reg);
		SV(_kbdRow);
		SV(_kbdColumn);
		SV(_kbdStrobe);
		SV(_column);
		SV(_half);
	}

public:
	static bool IsBitCorpGd24(uint32_t prgCrc) { return prgCrc == Crc; }
};
