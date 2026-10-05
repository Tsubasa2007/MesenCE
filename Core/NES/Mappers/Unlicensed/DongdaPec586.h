#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/BaseNesPpu.h"
#include "NES/NesConsole.h"
#include "NES/NesControlManager.h"
#include "NES/Input/PecKeyMatrix.h"
#include "Utilities/Serializer.h"

//Dongda (东达) PEC-586 family - the "Pyramid" (金字塔) home computers: PEC-586, PEC-686F and
//PEC-9588, 512KB of ROM holding their programming languages and games. NES 2.0 mapper 257
//(UNIF PEC-586).
//The files in circulation call it mapper 179, which is a different board.
//
//Registers: $5000-$5FFF, decoded by A8-A10 only. $5000 is the one that matters:
// - bit 4: a 32KB bank, bits 2-0, of the lower 256KB.
// - PEC-586/686F, bit 4 clear: $8000-$FFFF shows the upper 256KB through a fixed address
//   swap - each 1KB of the window is the last 1KB of one 8KB block - which is where the
//   reset code lives. Bit 6 then puts an 8KB bank, (bits 3-0) + $20 (+ $10 for bit 5), at
//   $8000.
// - PEC-9588, bit 4 clear: a 16KB bank at $8000, bits 3-0 (+ $10 for bit 5), and the last
//   16KB at $C000 - or, with bit 6 set, the one before it, and $1C when bits 3-2 are set too.
//   The reference emulator tests the whole byte for that last case (>= $6C), but the BIOS
//   only ever writes $6C itself, while the built-in turtle graphics write $EB - $6B plus the
//   bitmap bit - and run on in $C000-$FFFF. It boots like an Apple II, from the top of the ROM.
// - bits 4-3 both set: horizontal mirroring, otherwise vertical.
// - bit 7: the bitmap mode below.
//$5300 (PEC-586/686F) and $5500 (PEC-9588) bit 2 is the cassette input, $5100 bit 0 its
//output and the printer strobe, $5200 the printer data; none of them are connected here.
//Reads of the block return a few bits picked by the last $54xx write, as FCEUX has them.
//The PEC-9588's floppy drive is an Apple II Disk II controller at $5600 + slot*16 (the BIOS uses
//slot 1, $5610-$561F). No drive is emulated, but the data latch ($56xC) reads like an empty
//drive's: while the motor runs ($56x9 on, $56x8 off) the read amplifier picks up noise, so bytes
//keep arriving and the disk code runs out its byte count looking for an address field and reports
//an error, instead of waiting forever. With the motor off the latch holds still, which is how the
//same code tells that a drive has stopped spinning.
//
//Bitmap mode: the background is 1 bit per pixel. A tile in an even nametable column takes its
//first plane, one in an odd column its second, for both colour bits; and the pattern table
//follows the nametable row instead of $2000 - the upper 16 rows use $0000, the rest $1000.
//So 8KB of CHR RAM covers the whole screen with 512 private tiles a half. Sprites are colored
//two entries further into their palette, as the reference emulator has it: the menus' blinking
//cursor is drawn in color 1 of a palette whose only visible entry is color 3.
//
//Keyboard: the PEC-586 key matrix (see PecKeyMatrix) on $4016/$4017.
class DongdaPec586 : public BaseMapper
{
private:
	static constexpr uint32_t Crc9588 = 0xFE31765B;

	//What reads of $5000-$5FFF carry, by the last $54xx write's high nibble
	static constexpr uint8_t ReadBits[16] = { 0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 };

	bool _is9588 = false;
	uint8_t _regs[8] = {};

	//Noise an empty floppy drive's data latch reads back (PEC-9588), from a 16-bit LFSR, and
	//whether the drive motor is on
	uint16_t _diskNoise = 0xACE1;
	bool _diskMotorOn = false;

	//Any access to a Disk II soft switch flips it, read or write
	void AccessDiskSwitch(uint16_t addr)
	{
		switch(addr & 0x0F) {
			case 0x08: _diskMotorOn = false; break;
			case 0x09: _diskMotorOn = true; break;
		}
	}

	//The last nametable byte the picture fetched, for the bitmap mode
	uint16_t _ntAddr = 0;

	uint8_t _kbdRow = 0;
	uint8_t _kbdColumn = 0;

	NesControlManager* NesControls() { return (NesControlManager*)_console->GetControlManager(); }

	void MapPrg16k(uint16_t start, uint32_t bank)
	{
		SetCpuMemoryMapping(start, start + 0x3FFF, PrgMemoryType::PrgRom, (bank * 0x4000) & (_prgSize - 1), MemoryAccessType::Read);
	}

	void UpdateState()
	{
		uint8_t reg = _regs[0];
		if(reg & 0x10) {
			SetCpuMemoryMapping(0x8000, 0xFFFF, PrgMemoryType::PrgRom, ((reg & 0x07) * 0x8000) & (_prgSize - 1), MemoryAccessType::Read);
		} else if(_is9588) {
			uint8_t bank = (reg & 0x0F) | ((reg & 0x20) >> 1);
			MapPrg16k(0x8000, bank);
			MapPrg16k(0xC000, !(reg & 0x40) ? 0x1F : ((reg & 0x0C) == 0x0C ? 0x1C : 0x1E));
		} else {
			for(uint32_t i = 0; i < 32; i++) {
				uint32_t offset = ((0x107 | (i << 3)) << 10) & (_prgSize - 1);
				SetCpuMemoryMapping(0x8000 + i * 0x400, 0x83FF + i * 0x400, PrgMemoryType::PrgRom, offset, MemoryAccessType::Read);
			}
			if(reg & 0x40) {
				uint32_t bank = (reg & 0x0F) | 0x20 | ((reg & 0x20) >> 1);
				SetCpuMemoryMapping(0x8000, 0x9FFF, PrgMemoryType::PrgRom, (bank * 0x2000) & (_prgSize - 1), MemoryAccessType::Read);
			}
		}

		SetMirroringType((reg & 0x18) == 0x18 ? MirroringType::Horizontal : MirroringType::Vertical);
		_console->GetPpu()->SetSpriteColorShift(reg & 0x80);
	}

	bool IsBackgroundFetch()
	{
		uint32_t cycle = _console->GetPpu()->GetCurrentCycle();
		return cycle <= 256 || cycle >= 321;
	}

protected:
	uint16_t GetPrgPageSize() override { return 0x2000; }
	uint16_t GetChrPageSize() override { return 0x2000; }
	uint32_t GetChrRamSize() override { return 0x2000; }
	uint32_t GetWorkRamSize() override { return 0x2000; }
	uint32_t GetSaveRamSize() override { return 0; }

	uint16_t RegisterStartAddress() override { return 0x5000; }
	uint16_t RegisterEndAddress() override { return 0x5FFF; }
	bool AllowRegisterRead() override { return true; }
	bool EnableCustomVramRead() override { return true; }

	//No 2C02 "read one dot before vblank" race, as with the other learning machines' clone PPUs.
	//One of the PEC-9588's programs polls $2002 for vblank before every character it prints,
	//with NMIs on, and keeps losing the flag to that race - its banner then takes seconds a letter.
	bool EnablePpuNmiSuppressRace() override { return false; }

	void GetMemoryRanges(MemoryRanges& ranges) override
	{
		BaseMapper::GetMemoryRanges(ranges);
		ranges.AddHandler(MemoryOperation::Write, 0x4016);
		ranges.AddHandler(MemoryOperation::Read, 0x4017);
		ranges.SetAllowOverride();
	}

	void InitMapper() override
	{
		_is9588 = _romInfo.Hash.PrgCrc32 == Crc9588;

		AddRegisterRange(0x4016, 0x4016, MemoryOperation::Write);
		AddRegisterRange(0x4017, 0x4017, MemoryOperation::Read);

		SetCpuMemoryMapping(0x6000, 0x7FFF, 0, PrgMemoryType::WorkRam);
		SelectChrPage(0, 0);
		UpdateState();
	}

	void Reset(bool softReset) override
	{
		BaseMapper::Reset(softReset);
		memset(_regs, 0, sizeof(_regs));
		_kbdRow = 0;
		_kbdColumn = 0;
		UpdateState();
	}

	uint8_t MapperReadVram(uint16_t addr, MemoryOperationType type) override
	{
		if(type == MemoryOperationType::PpuRenderingRead) {
			if(addr >= 0x2000) {
				if((addr & 0x3FF) < 0x3C0) {
					_ntAddr = addr;
				}
			} else if((_regs[0] & 0x80) && IsBackgroundFetch()) {
				addr = ((_ntAddr & 0x200) << 3) | ((_ntAddr & 0x01) << 3) | (addr & 0x0FF7);
			}
		}
		return InternalReadVram(addr);
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		if(_is9588 && (addr & 0xFF00) == 0x5600) {
			AccessDiskSwitch(addr);
			if((addr & 0x0F) == 0x0C) {
				//Disk II data latch with no disk: while the motor runs, a fresh noise byte every
				//read with bit 7 set like any byte the shift register has finished assembling
				if(_diskMotorOn) {
					for(int i = 0; i < 8; i++) {
						_diskNoise = (_diskNoise >> 1) ^ (-(int16_t)(_diskNoise & 1) & 0xB400);
					}
				}
				return (uint8_t)_diskNoise | 0x80;
			}
		}
		if(addr == 0x4017) {
			uint8_t key = PecKeyMatrix::Read(NesControls()->GetControlDevice<Sb2kKeyboard>().get(), _kbdRow, _kbdColumn);
			return (NesControls()->ReadRam(addr) & ~0x12) | key;
		}
		return (_console->GetMemoryManager()->GetOpenBus() & 0xD8) | ReadBits[_regs[4] >> 4];
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr == 0x4016) {
			NesControls()->WriteRam(addr, value);
			PecKeyMatrix::Write(value, _kbdRow, _kbdColumn);
			return;
		}

		if(_is9588 && (addr & 0xFF00) == 0x5600) {
			AccessDiskSwitch(addr);
		}

		_regs[(addr >> 8) & 0x07] = value;
		UpdateState();
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SVArray(_regs, 8);
		SV(_ntAddr);
		SV(_diskNoise);
		SV(_diskMotorOn);
		SV(_kbdRow);
		SV(_kbdColumn);
		if(!s.IsSaving()) {
			UpdateState();
		}
	}
};
