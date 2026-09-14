#pragma once
#include "pch.h"
#include "Shared/Video/BaseVideoFilter.h"

class Emulator;

class SacDefaultVideoFilter final : public BaseVideoFilter
{
private:
	Emulator* _emu = nullptr;
	uint32_t _calculatedPalette[0x8000] = {};
	VideoConfig _videoConfig = {};

	void InitLookupTable();

protected:
	void OnBeforeApplyFilter() override;

public:
	SacDefaultVideoFilter(Emulator* emu);

	void ApplyFilter(uint16_t* ppuOutputBuffer) override;
};
