#pragma once
#include "pch.h"
#include "NES/RomData.h"
#include "Utilities/CRC32.h"
#include "Shared/MessageManager.h"

//A .CDV off a 中索 (Zoson) VCD - a game for the LingTong-family machine the discs came with.
//Unlike the Dr. PC Jr. "FC GAMES" files there is no header to read: the file is
//    0       2KB of 6502 code, run at $6200
//    $800    the game, laid into DRAM from its start
//The machine's own loader (part of its resident DOS, which no dump has) reads the file off
//the disc, puts it there and jumps to $6200. The code then does the rest itself - it picks
//the DRAM layout and mirroring, may leave a CHR helper at $7800 or copy graphics into CHR
//RAM, and ends with JMP ($FFFC) into the game. So a file opened on its own needs nothing
//but that placement, and LingTongMapper does it at every power-on and reset.
class ZosonCdvLoader
{
private:
	static constexpr uint32_t StubSize = 0x800;

	//The DRAM the mapper fits; an image past it is not a game (disc 1's STARTWPS.CDV is a
	//disk image for the machine's DOS, and needs that DOS)
	static constexpr uint32_t MaxImageSize = 0x100000;

public:
	void LoadRom(RomData& romData, vector<uint8_t>& file)
	{
		if(file.size() <= StubSize || ((file.size() - StubSize) & 0x3FFF) || file.size() - StubSize > MaxImageSize) {
			MessageManager::Log("[CDV] Not a game this machine can start on its own (no \"FC GAMES\" header, and not 2KB of code plus a game of 16KB units up to 1MB) - load operation cancelled.");
			romData.Error = true;
			return;
		}

		romData.Info.Format = RomFormat::iNes;
		romData.Info.MapperID = 174;
		romData.Info.SubMapperID = 0;
		romData.Info.System = GameSystem::Dendy;
		romData.Info.Mirroring = MirroringType::Vertical;
		romData.Info.FilePrgOffset = StubSize;

		//The code goes where the header of an "FC GAMES" file would - see LingTongMapper
		romData.CdvHeader.assign(file.begin(), file.begin() + StubSize);

		//Padded to a power of two only so the mapper's ROM-side masks hold; the code never runs
		//the ROM side, it selects DRAM before anything else
		uint32_t imageSize = (uint32_t)(file.size() - StubSize);
		uint32_t prgSize = 0x4000;
		while(prgSize < imageSize) {
			prgSize <<= 1;
		}
		romData.PrgRom.assign(file.begin() + StubSize, file.end());
		romData.PrgRom.resize(prgSize, 0);

		romData.Info.Hash.PrgCrc32 = CRC32::GetCRC(file.data() + StubSize, imageSize);
		romData.Info.Hash.PrgChrCrc32 = romData.Info.Hash.PrgCrc32;

		MessageManager::Log("[CDV] 中索 (Zoson) disc game: 2KB start code at $6200, " + std::to_string(imageSize / 1024) + " KB into DRAM");
	}
};
