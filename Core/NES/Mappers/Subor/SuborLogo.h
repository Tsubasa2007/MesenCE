#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/NesConsole.h"
#include "NES/BaseNesPpu.h"
#include "Utilities/Serializer.h"

//The Subor LOGO V1.0 (小霸王) cartridge: 32KB of PRG with no banking, its header says mapper 0, and
//32KB of CHR RAM, which is what draws the turtle's bitmap, and 8KB of work RAM at $6000 that the
//LOGO interpreter uses. Ported from the reference emulator's Mapper000 special case.
//
//A write to $FFFF sets the screen mode, which takes effect the next time the PPU's address lands
//in the nametables - a $2006 write there, or a nametable fetch. The mode's low two bits:
// - 0: the first 8KB of the CHR RAM (initialization)
// - 1: $1000 holds the 4KB bank the register names (text screen)
// - 2: each 64-line band of the screen draws from its own 4KB bank at $1000, banks 2-5 - every
//   tile on screen is a different one, so the screen is a bitmap (full graphics screen)
// - 3: the same, except that the bottom band keeps bank 1's text (split screen)
//The band is bits 9-8 of the nametable address, so the bank always follows the row being drawn.
class SuborLogo : public BaseMapper
{
private:
	uint8_t _reg = 0;
	uint8_t _band = 0;

	void UpdateChr()
	{
		switch(_reg & 0x03) {
			case 0:
				//Bank numbers wrap in the 32KB, so this is always the first 8KB
				SelectChrPage(0, 0, ChrMemoryType::ChrRam);
				SelectChrPage(1, 1, ChrMemoryType::ChrRam);
				break;

			case 1:
				SelectChrPage(1, _reg & 0x07, ChrMemoryType::ChrRam);
				break;

			case 2:
				SelectChrPage(1, 2 + _band, ChrMemoryType::ChrRam);
				break;

			case 3:
				SelectChrPage(1, _band == 3 ? 1 : 2 + _band, ChrMemoryType::ChrRam);
				break;
		}
	}

protected:
	uint16_t GetPrgPageSize() override { return 0x8000; }
	uint16_t GetChrPageSize() override { return 0x1000; }
	uint32_t GetChrRamSize() override { return 0x8000; }
	uint32_t GetWorkRamSize() override { return 0x2000; }
	bool ForceWorkRamSize() override { return true; }

	uint16_t RegisterStartAddress() override { return 0xFFFF; }
	uint16_t RegisterEndAddress() override { return 0xFFFF; }
	bool EnableVramAddressHook() override { return true; }

	void InitMapper() override
	{
		SelectPrgPage(0, 0);
		SelectChrPage(0, 0, ChrMemoryType::ChrRam);
		SelectChrPage(1, 1, ChrMemoryType::ChrRam);
	}

	void Reset(bool softReset) override
	{
		BaseMapper::Reset(softReset);
		_reg = 0;
		_band = 0;
		SelectChrPage(0, 0, ChrMemoryType::ChrRam);
		SelectChrPage(1, 1, ChrMemoryType::ChrRam);
	}

	void NotifyVramAddressChange(uint16_t addr) override
	{
		if((addr & 0x3000) != 0x2000) {
			return;
		}
		//While the picture is drawn only the nametable fetches count: the attribute fetch that
		//follows each one sits in the last band and would pull every tile's pattern from there
		BaseNesPpu* ppu = _console->GetPpu();
		if((addr & 0x3FF) >= 0x3C0 && ppu->IsDisplayOn() && ppu->GetCurrentScanline() < 240) {
			return;
		}
		_band = (addr >> 8) & 0x03;
		UpdateChr();
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		_reg = value;
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SV(_reg);
		SV(_band);
	}

public:
	static constexpr uint32_t PrgCrc = 0x366C20D7;
};
