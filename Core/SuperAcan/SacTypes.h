#pragma once
#include "pch.h"

//Timing is MAME's supracan driver: everything is derived from the one 53.693175MHz crystal.
//The 68000 runs at /6, the pixel clock at /10, and a frame is 342x262 pixel clocks of which
//256x224 are visible (lines 8-231).
class SacConstants
{
public:
	static constexpr uint32_t MasterClockRate = 53693175;
	static constexpr uint32_t CpuClockDivider = 6;
	static constexpr uint32_t PixelClockDivider = 10;

	static constexpr uint32_t ClocksPerScanline = 342;
	static constexpr uint32_t ScanlineCount = 262;
	static constexpr uint32_t MasterClocksPerFrame = PixelClockDivider * ClocksPerScanline * ScanlineCount;
	static constexpr uint32_t CpuCyclesPerLine = PixelClockDivider * ClocksPerScanline / CpuClockDivider;

	//MAME raises the vblank interrupt on line 240, and reports vblank from there on
	static constexpr uint32_t VblankLine = 240;

	static constexpr uint32_t ScreenWidth = 256;
	static constexpr uint32_t ScreenHeight = 224;
	static constexpr uint32_t PixelCount = ScreenWidth * ScreenHeight;

	//The video flags can widen the picture to 320 and show 240 lines instead of 224
	static constexpr uint32_t MaxScreenWidth = 320;
	static constexpr uint32_t MaxScreenHeight = 240;
	static constexpr uint32_t MaxPixelCount = MaxScreenWidth * MaxScreenHeight;

	//The cartridge sits at the bottom of the 68000's space and the system's work RAM at the
	//top, mirrored four times across $FC0000-$FFFFFF
	static constexpr uint32_t CartEnd = 0x3FFFFF;
	static constexpr uint32_t WorkRamStart = 0xFC0000;
	static constexpr uint32_t WorkRamSize = 0x10000;
};

struct SacState
{
	uint32_t FrameCount;
};
