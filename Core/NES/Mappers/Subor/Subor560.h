#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/NesConsole.h"
#include "NES/BaseNesPpu.h"
#include "Utilities/Serializer.h"

//NES 2.0 mapper 560, submapper 1: the Subor V1.0 (小霸王, SB-218) learning cartridge - a C/E BASIC
//board with a character generator. Ported from the reference emulator's Mapper560.
//
//- 64KB of PRG in two 32KB halves; any write to $8000-$FFFF switches to the other one. It starts
//  in the second.
//- 8KB of CHR RAM for ordinary tiles, which comes up holding the first 8KB of the CHR ROM, and the
//  128KB CHR ROM itself as a font: 8192 tiles' worth of bit planes.
//- While the second half is in, each background tile may come from the font instead. The byte at
//  the same position in nametable 1 picks it: 1 means the ordinary tile, anything else is a font
//  page - its low five bits are bits 12-8 of a font tile whose low bits are the nametable byte,
//  and bit 5 picks that tile's first or second plane. A font tile is drawn 1 bit per pixel in
//  colour 1. The board has four nametables, so nametable 1 is its own memory.
//
//The reference emulator's other special cases (OPT1/OPT2 and two patched bytes) are for the plain
//C/E BASIC cartridges, submapper 0, and never apply to this one.
class Subor560 : public BaseMapper
{
private:
	bool _secondHalf = true;

	//The last nametable byte the picture fetched, whose position picks the font page
	uint16_t _ntAddr = 0;

	void UpdateState()
	{
		SelectPrgPage(0, _secondHalf ? 1 : 0);
	}

	bool IsBackgroundFetch()
	{
		uint32_t cycle = _console->GetPpu()->GetCurrentCycle();
		return cycle <= 256 || cycle >= 321;
	}

protected:
	uint16_t GetPrgPageSize() override { return 0x8000; }
	uint16_t GetChrPageSize() override { return 0x2000; }
	uint32_t GetChrRamSize() override { return 0x2000; }
	bool EnableCustomVramRead() override { return true; }

	void InitMapper(RomData& romData) override
	{
		romData.Info.System = GameSystem::Dendy;
	}

	void InitMapper() override
	{
		_romInfo.System = GameSystem::Dendy;
		if(_chrRam && _chrRom) {
			memcpy(_chrRam, _chrRom, std::min(_chrRamSize, _chrRomSize));
		}
		SelectChrPage(0, 0, ChrMemoryType::ChrRam);
		UpdateState();
	}

	void Reset(bool softReset) override
	{
		BaseMapper::Reset(softReset);
		_secondHalf = true;
		UpdateState();
	}

	uint8_t MapperReadVram(uint16_t addr, MemoryOperationType type) override
	{
		if(type == MemoryOperationType::PpuRenderingRead) {
			if(addr >= 0x2000) {
				if((addr & 0x3FF) < 0x3C0) {
					_ntAddr = addr;
				}
			} else if(_secondHalf && IsBackgroundFetch()) {
				uint8_t page = InternalReadVram(0x2400 | (_ntAddr & 0x3FF));
				if(page != 1) {
					if(addr & 0x08) {
						//Font tiles are 1bpp: their second plane is empty
						return 0;
					}
					uint32_t tile = ((page & 0x1F) << 8) | ((addr >> 4) & 0xFF);
					uint32_t offset = (tile << 4) | ((page & 0x20) ? 0x08 : 0) | (addr & 0x07);
					return _chrRomSize ? _chrRom[offset % _chrRomSize] : 0;
				}
			}
		}
		return InternalReadVram(addr);
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		_secondHalf = !_secondHalf;
		UpdateState();
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SV(_secondHalf);
		SV(_ntAddr);
		if(!s.IsSaving()) {
			UpdateState();
		}
	}
};
