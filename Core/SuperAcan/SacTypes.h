#pragma once
#include "pch.h"
#include "Shared/BaseState.h"
#include "SuperAcan/SacCpu.h"

//Everything is derived from the one 53.693175MHz crystal. The pixel clock is /10 and a frame is
//342x262 pixel clocks of which 256x224 are visible (lines 8-231).
//The 68000 runs at /5 (10.74MHz), not MAME's /6: games pace their loops by vblank, and at /6 a
//palette blend one title screen does each pass takes 278 lines, so the pass slips to every other
//frame and its fade and the intro before it run at half speed. At /5 both match Bcan frame for frame.
//The sound processor keeps /12, which pitch and tempo were checked against.
class SacConstants
{
public:
	static constexpr uint32_t MasterClockRate = 53693175;
	static constexpr uint32_t CpuClockDivider = 5;
	//The sound processor and sound chip keep their own clock, whatever the 68000's
	static constexpr uint32_t SoundCpuClockDivider = 12;
	//Crystal ticks per step of the free-running counter at $A20F. MAME's rule makes a step 8192 cycles
	//of a 68000 at a sixth of the crystal (49152 ticks), which Bcan's pacing matches at $A201 (a
	//eighth of that per step, as MAME has it) but not here: one intro waits on it about 2250 times and
	//its scenes last 1.21 times as long in Bcan, where 59474 ticks keep the whole intro within two
	//frames of Bcan's recording.
	static constexpr uint64_t FrcLongStepClocks = 59474;
	static constexpr uint32_t PixelClockDivider = 10;

	static constexpr uint32_t ClocksPerScanline = 342;
	static constexpr uint32_t ScanlineCount = 262;
	static constexpr uint32_t MasterClocksPerFrame = PixelClockDivider * ClocksPerScanline * ScanlineCount;
	static constexpr uint32_t CpuCyclesPerLine = PixelClockDivider * ClocksPerScanline / CpuClockDivider;
	static constexpr uint32_t SoundCpuCyclesPerLine = PixelClockDivider * ClocksPerScanline / SoundCpuClockDivider;

	//MAME raises the vblank interrupt on line 240, and reports vblank from there on
	static constexpr uint32_t VblankLine = 240;

	static constexpr uint32_t ScreenWidth = 256;
	static constexpr uint32_t ScreenHeight = 224;
	static constexpr uint32_t PixelCount = ScreenWidth * ScreenHeight;

	//The video flags can widen the picture to 320 and show 240 lines instead of 224
	static constexpr uint32_t MaxScreenWidth = 320;
	static constexpr uint32_t MaxScreenHeight = 240;
	static constexpr uint32_t MaxPixelCount = MaxScreenWidth * MaxScreenHeight;

	//The frame buffer carries the frame's width and height after the pixels, so the video filter
	//sees the mode the frame was drawn in while the frame itself is always sent as 320x240
	static constexpr uint32_t FrameBufferSize = MaxPixelCount + 2;

	//The cartridge sits at the bottom of the 68000's space and the system's work RAM at the
	//top, mirrored four times across $FC0000-$FFFFFF
	static constexpr uint32_t CartEnd = 0x3FFFFF;
	static constexpr uint32_t WorkRamStart = 0xFC0000;
	static constexpr uint32_t WorkRamSize = 0x10000;
};

//What the debugger's video viewers are given: where the frame is, and the video registers
//($F00000-$F001FF) as words
struct SacPpuState : BaseState
{
	uint32_t FrameCount;
	uint16_t Scanline;
	uint16_t VideoRegs[0x100];
};

//The sound processor, a 65C02, and whether the 68000 lets it run
struct SacSoundCpuState : BaseState
{
	uint64_t CycleCount;
	uint16_t PC;
	uint8_t A;
	uint8_t X;
	uint8_t Y;
	uint8_t SP;
	uint8_t PS;
	bool Running;
	bool Waiting;
	bool Stopped;
	bool IrqLine;
	bool NmiPending;
};

//The UM6619's registers on the 68000's side ($E90000-$E9003F), the video chip's DMA and raster
//interrupt as they stand, and the sound registers at $400-$4FF of sound RAM
struct SacSystemState
{
	uint32_t DmaSource[2];
	uint32_t DmaDest[2];
	uint32_t SpriteDmaSource;
	uint32_t SpriteDmaDest;
	int32_t LineOnTarget;
	int32_t LineOffTarget;
	uint16_t DmaCount[2];
	uint16_t SoundCpuControl;
	uint16_t FrcControl;
	uint16_t FrcFrequency;
	uint16_t LatchedControls[2];
	uint8_t IrqMask;
	uint8_t IrqLines;
	uint8_t SoundIrqEnable;
	uint8_t SoundIrqSource;
	uint8_t SoundShiftControl;
	uint8_t SoundShiftRegs[2];
	uint8_t SoundStatus;
	uint8_t SoundRegAddress;
	uint8_t LockoutAddress;
	bool BootRomLow;
	bool BootRomHigh;
};

struct SacApuChannelState
{
	uint16_t Pitch;
	uint16_t Length;
	uint16_t StartAddr;
	uint16_t CurrAddr;
	uint16_t EndAddr;
	uint8_t Volume;
	uint8_t Streaming;
	bool OneShot;
	bool Active;
};

struct SacApuState
{
	SacApuChannelState Channels[16];
	uint16_t TimerPeriod;
	uint8_t TimerControl;
	bool TimerActive;
};

//Everything the register viewer shows
struct SacState : BaseState
{
	SacCpuState Cpu;
	SacPpuState Ppu;
	SacSoundCpuState SoundCpu;
	SacSystemState System;
	SacApuState Apu;
};
