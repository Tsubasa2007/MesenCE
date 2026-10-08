#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/Mappers/Subor/SuborCarts.h"

class Mapper241 : public BaseMapper
{
protected:
	//See SuborCarts::HasLongDendyFrame
	int32_t GetDendyScanlineCount() override { return SuborCarts::HasLongDendyFrame(_romInfo.Hash.PrgCrc32) ? 313 : 312; }
	int32_t GetDendyNmiScanline() override { return SuborCarts::HasLongDendyFrame(_romInfo.Hash.PrgCrc32) ? 292 : 291; }

	uint16_t GetPrgPageSize() override { return 0x8000; }
	uint16_t GetChrPageSize() override { return 0x2000; }

	void InitMapper() override
	{
		SelectPrgPage(0, 0);
		SelectChrPage(0, 0);
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		SelectPrgPage(0, value);
	}
};