#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "Utilities/Serializer.h"

//诚信达 (ChengXinDa) home-tutor cartridges: a 1MB mask ROM, a custom chip and two 6264s (8KB of
//RAM at $6000 and 8KB of CHR RAM). Their iNES headers in circulation say mapper 179, which is an
//unrelated board, so the dumps are recognised by their PRG CRC32.
//
// - PRG: writes to $5FFF pick the 16KB page at $8000 (bits 0-5); the last 16KB is fixed at $C000.
// - CHR: the 8KB of RAM is laid out two ways, picked by $5FFF bit 6. Either way, what $0000-$0FFF
//   shows depends on where the PPU last stepped onto the nametable, latched as the bus moves
//   there from elsewhere - a tile fetch, or the CPU setting $2006 (much as on Bandai's Oeka Kids
//   board). The program fills a part by pointing $2006 into the nametable and then writing the
//   tiles at $0000.
//     bit 6 clear: by half. The top half of the screen ($2000-$21FF) takes its 256 tiles from
//       the first 4KB and the bottom half from the second, which $1000-$1FFF (the sprites) also
//       shows. The title and the menus.
//     bit 6 set: by quarter, 2KB each, one bit per pixel - a tile is 8 bytes and both of the
//       PPU's planes read the same ones. Every quarter has 256 tiles of its own, so the whole
//       screen is a bitmap. The question pages draw their text into it. The sprites ($1000-$1FFF)
//       read the last quarter the same way: the unit pages keep their selection dot there, as
//       tile $C1 of that quarter, and nowhere else.
class ChengXinDa : public BaseMapper
{
private:
	static constexpr uint32_t CrcGrade1 = 0xA4F0B21A;
	static constexpr uint32_t CrcStudyCard = 0x024154BA;

	uint8_t _reg = 0;
	uint8_t _quarter = 0;
	uint16_t _lastAddress = 0;

	uint32_t ChrAddress(uint16_t addr)
	{
		if(_reg & 0x40) {
			uint32_t quarter = (addr & 0x1000) ? 0x03 : _quarter;
			return (quarter << 11) | ((addr >> 1) & 0x07F8) | (addr & 0x07);
		}
		if(addr & 0x1000) {
			return 0x1000 | (addr & 0x0FFF);
		}
		return ((_quarter & 0x02) ? 0x1000 : 0) | (addr & 0x0FFF);
	}

protected:
	uint16_t GetPrgPageSize() override { return 0x4000; }
	uint16_t GetChrPageSize() override { return 0x2000; }
	uint32_t GetChrRamSize() override { return 0x2000; }
	uint16_t GetChrRamPageSize() override { return 0x2000; }
	uint32_t GetWorkRamSize() override { return 0x2000; }
	uint16_t RegisterStartAddress() override { return 0x5FFF; }
	uint16_t RegisterEndAddress() override { return 0x5FFF; }
	bool EnableVramAddressHook() override { return true; }
	bool EnableCustomVramRead() override { return true; }

	void InitMapper() override
	{
		_reg = 0;
		_quarter = 0;
		_lastAddress = 0;
		SelectPrgPage(0, 0);
		SelectPrgPage(1, -1);

		//Only for the debugger's views - the PPU goes through MapperReadVram below
		SetPpuMemoryMapping(0x0000, 0x1FFF, ChrMemoryType::ChrRam, 0, MemoryAccessType::ReadWrite);
	}

	void NotifyVramAddressChange(uint16_t addr) override
	{
		//A step onto a nametable from elsewhere
		if((_lastAddress & 0x3000) != 0x2000 && (addr & 0x3000) == 0x2000) {
			_quarter = (addr >> 8) & 0x03;
		}
		_lastAddress = addr;
	}

	uint8_t MapperReadVram(uint16_t addr, MemoryOperationType type) override
	{
		if(addr < 0x2000) {
			return _chrRam[ChrAddress(addr)];
		}
		return InternalReadVram(addr);
	}

	void MapperWriteVram(uint16_t addr, uint8_t value) override
	{
		if(addr < 0x2000) {
			_chrRam[ChrAddress(addr)] = value;
			return;
		}
		InternalWriteVram(addr, value);
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		_reg = value;
		SelectPrgPage(0, value & 0x3F);
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SV(_reg);
		SV(_quarter);
		SV(_lastAddress);
	}

public:
	static bool IsChengXinDa(uint32_t prgCrc) { return prgCrc == CrcGrade1 || prgCrc == CrcStudyCard; }
};
