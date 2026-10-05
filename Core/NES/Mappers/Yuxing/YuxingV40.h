#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/BaseNesPpu.h"
#include "NES/NesConsole.h"
#include "NES/NesMemoryManager.h"
#include "Utilities/Serializer.h"

//The YuXing V4.0 learning machine. It predates the mapper 169 board the later YuXing machines
//share, and the reference emulator's handling of it was never finished. Read off its BIOS:
//
//- PRG: a 64KB BIOS (16KB pages A-D) and a 64KB LOGO extension, as one 128KB image in that
//  order. $4800 bit 6 picks A or C at $8000; $C000 has D whenever bit 6 or bit 7 is set and B
//  only when both are clear - the BIOS's PRG A15 is bit 6 OR (bit 7 AND CPU A14). It powers up
//  in A/B: B's reset copies "LDA #$C0 / STA $4800 / JMP ($FFFC)" to RAM, which hands over to D's.
//  The BIOS switches $8000 between A (its input method and editor, with $80) and C (with $C0)
//  while running in D.
//- $5500 bit 2 maps the extension instead: a write to $8000-$FFFF picks its 16KB page at $8000
//  (the BIOS writes $FFFF), and its last page sits at $C000. Bits 0-1 pick an 8KB page of 32KB
//  RAM at $6000, bit 3 the mirroring. The RAM keeps its contents with the power off: BASIC's
//  SYSTEM ends on "断电保持" (kept through power-off), and the program is there at the next start.
//- $4800 bits 0-5 also pick a 2KB page of a 128KB font ROM, which the CPU reads at
//  $4800-$4FFF: the BIOS copies its tiles from pages 0-4 into CHR RAM at boot and takes the
//  hanzi from the rest. The font ROM is the image's CHR ROM; the PPU sees only the 8KB of
//  CHR RAM.
//- $4800 bit 7 also turns on the YuXing bitmap screen: the 8KB of CHR RAM is a 1bpp picture,
//  each name table byte drawn twice (its first plane in the even column, its second in the odd
//  one) and the lower half from $1000 - the later YuXing machines' 2-screen split.
//- It runs on PAL timing, not the Dendy timing the rest of mapper 169 gets. Its boot waits for
//  vblank by polling $2002 while its NMI handler also reads $2002; on Dendy, whose frame is a
//  whole number of CPU cycles, the two lock into a phase where the poll never wins and the BIOS
//  hangs on a black screen. NTSC boots too, but PAL keeps the other YuXing machines' 50Hz and
//  312 lines, and nearly their CPU cycles per frame.
class YuxingV40 : public BaseMapper
{
private:
	uint8_t _reg4800 = 0;
	uint8_t _reg5500 = 0;
	uint8_t _extBank = 0;

	void UpdateState()
	{
		if(_reg5500 & 0x04) {
			SelectPrgPage(0, 4 | (_extBank & 0x03));
			SelectPrgPage(1, 7);
		} else {
			SelectPrgPage(0, (_reg4800 & 0x40) ? 2 : 0);
			SelectPrgPage(1, (_reg4800 & 0xC0) ? 3 : 1);
		}
		SetCpuMemoryMapping(0x6000, 0x7FFF, _reg5500 & 0x03, PrgMemoryType::SaveRam);
		SetMirroringType((_reg5500 & 0x08) ? MirroringType::Horizontal : MirroringType::Vertical);
		if(_console->GetPpu()) {
			_console->GetPpu()->SetSplitBgFetch((_reg4800 & 0x80) ? 1 : 0);
		}
	}

protected:
	uint16_t GetPrgPageSize() override { return 0x4000; }
	uint16_t GetChrPageSize() override { return 0x2000; }
	uint32_t GetChrRamSize() override { return 0x2000; }
	uint16_t GetChrRamPageSize() override { return 0x2000; }
	uint32_t GetWorkRamSize() override { return 0; }
	uint32_t GetSaveRamSize() override { return 0x8000; }
	uint32_t GetSaveRamPageSize() override { return 0x2000; }
	bool ForceSaveRamSize() override { return true; }
	bool ForceWorkRamSize() override { return true; }
	bool AllowRegisterRead() override { return true; }

	void InitMapper() override
	{
		AddRegisterRange(0x4800, 0x4FFF, MemoryOperation::Read);
		AddRegisterRange(0x4800, 0x4800, MemoryOperation::Write);
		AddRegisterRange(0x5500, 0x5500, MemoryOperation::Write);
		RemoveRegisterRange(0x8000, 0xFFFF, MemoryOperation::Read);
		SelectChrPage(0, 0, ChrMemoryType::ChrRam);
		UpdateState();
	}

	void Reset(bool softReset) override
	{
		_reg4800 = 0;
		_reg5500 = 0;
		_extBank = 0;
		UpdateState();
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		if(_chrRomSize == 0) {
			return _console->GetMemoryManager()->GetOpenBus();
		}
		uint32_t offset = ((_reg4800 & 0x3F) << 11) | (addr & 0x7FF);
		return _chrRom[offset % _chrRomSize];
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr == 0x4800) {
			_reg4800 = value;
		} else if(addr == 0x5500) {
			_reg5500 = value;
		} else if(_reg5500 & 0x04) {
			_extBank = value;
		}
		UpdateState();
	}

	//As YuxingMapper's 2-screen split: bit 12 from the name table row, bit 3 from the parity of
	//the screen column (the leftmost one is fetched at cycle 321 of the previous line)
	void ApplySplitBgFetch(uint8_t tileIndex, uint16_t videoRamAddr, uint16_t cycle, uint16_t& tileAddr) override
	{
		uint16_t halfSelect = (videoRamAddr & 0x200) ? 0x1000 : 0;
		uint16_t column = (uint16_t)((((cycle - 1) >> 3) & 1) << 3);
		tileAddr = halfSelect | ((uint16_t)tileIndex << 4) | column | (videoRamAddr >> 12);
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SV(_reg4800);
		SV(_reg5500);
		SV(_extBank);
		if(!s.IsSaving()) {
			UpdateState();
		}
	}

public:
	static constexpr uint32_t PrgCrc = 0xCEAC04C7;
};
