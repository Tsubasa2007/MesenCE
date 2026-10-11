#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"

class Sachen_133 : public BaseMapper
{
private:
	//A learning cartridge on this board (PRG CRC 6922D4CE) has a second register at $6001: bits 7-5
	//pick a 16KB PRG bank mirrored at $8000 and $C000, bits 2-0 an 8KB CHR bank. Its 128KB of PRG
	//and 64KB of CHR are beyond what $4100 alone can reach.
	bool HasRegister6001() { return _romInfo.Hash.PrgCrc32 == 0x6922D4CE; }

protected:
	uint16_t GetPrgPageSize() override { return 0x4000; }
	uint16_t GetChrPageSize() override { return 0x2000; }
	uint16_t RegisterStartAddress() override { return 0x4100; }
	uint16_t RegisterEndAddress() override { return 0xFFFF; }

	void InitMapper() override
	{
		SelectPrgPage2x(0, 0);
		SelectChrPage(0, 0);
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if((addr & 0x6100) == 0x4100) {
			SelectPrgPage2x(0, ((value >> 2) & 0x01) << 1);
			SelectChrPage(0, value & 0x03);
		} else if(addr >= 0x6000 && addr < 0x8000) {
			WritePrgRam(addr, value);
			if(addr == 0x6001 && HasRegister6001()) {
				SelectPrgPage(0, value >> 5);
				SelectPrgPage(1, value >> 5);
				SelectChrPage(0, value & 0x07);
			}
		}
	}
};
