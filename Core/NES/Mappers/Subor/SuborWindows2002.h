#pragma once
#include "pch.h"
#include "NES/Mappers/Unlicensed/Henggedianzi177.h"

//Subor Windows 2002 (小霸王) - a Subor learning machine on a stock mapper 177 board.
//
//The board is the Henggedianzi one and needs nothing added to it: 32KB at a time from a single
//write-anywhere register, mirroring from bit 5 of the same value. What the plain mapper cannot
//supply is the machine, and none of it is in the iNES header - not the Dendy timing, and not
//the two input devices, so the ROM is recognised by its BIOS's PRG CRC32.
//
//Its BIOS scans two devices over $4016/$4017, which is what its own traffic shows: the reduced
//Subor keyboard (the $05/$04/$06 counter written to $4016, read back on $4017) and a 24-bit
//serial mouse on $4016, latched by a write whose low three bits are 1 and then shifted out over
//exactly 24 reads. That mouse is NOT the one SuborMouse implements - see SuborMouse24.
//
//No speech: the mapper 177 boards can carry an LPC decoder on $5000-$5002, but this machine
//never addresses it. Nothing in the 512KB of PRG stores to $5002 or reads $5000, and a 900
//frame run makes no access anywhere in $4020-$5FFF.
class SuborWindows2002 : public Henggedianzi177
{
public:
	static bool IsSuborWindows2002(uint32_t prgCrc)
	{
		return prgCrc == 0x6058DB1C;
	}

protected:
	void InitMapper(RomData& romData) override
	{
		//Chinese famiclone hardware, timed like its siblings
		romData.Info.System = GameSystem::Dendy;
	}

	void InitMapper() override
	{
		//Both are needed: the line above settles what the rom is, this one settles what the
		//built mapper reports, and UpdateRegion() reads the region back off the mapper.
		_romInfo.System = GameSystem::Dendy;
		Henggedianzi177::InitMapper();
	}
};
