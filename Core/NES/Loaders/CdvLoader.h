#pragma once
#include "pch.h"
#include "NES/RomData.h"
#include "Utilities/CRC32.h"
#include "Shared/MessageManager.h"

//A .CDV is a game for the Dr. PC Jr. family, carried on the machine's own CD alongside the
//Video CD tracks. It is not an iNES file: a header of its own describes the sizes, and the
//bulk of it is the register settings the machine would otherwise get from a cartridge, so
//the file is loaded as mapper 173 with that header handed to the mapper to apply.
//Ported from the VirtuaNES-BBK fork (NES/ROM.cpp, author fanoble).
//
//The layout is
//    0                 "FC GAMES" and the header proper
//    HeaderSize        an optional block of code the machine copies into RAM before start
//    ...               PRG, then CHR
//and the header size is either 512 or 2048 bytes depending on where the file came from -
//a disc leaves the rest of a 2048-byte sector zeroed, a floppy does not.
class CdvLoader
{
private:
	static constexpr uint32_t SmallHeader = 0x200;
	static constexpr uint32_t LargeHeader = 0x800;

	//A disc-sourced file has a long run of zeroes after the header proper, because the
	//header only fills the start of a sector. A floppy-sourced one packs the code in right
	//behind it.
	static uint32_t GetHeaderSize(vector<uint8_t>& file)
	{
		if(file.size() < LargeHeader) {
			return SmallHeader;
		}

		for(uint32_t i = SmallHeader; i < LargeHeader; i++) {
			if(file[i] != 0) {
				return SmallHeader;
			}
		}
		return LargeHeader;
	}

public:
	void LoadRom(RomData& romData, vector<uint8_t>& file)
	{
		uint32_t headerSize = GetHeaderSize(file);
		if(file.size() < headerSize) {
			MessageManager::Log("[CDV] Invalid file (shorter than its header) - load operation cancelled.");
			romData.Error = true;
			return;
		}

		romData.Info.Format = RomFormat::iNes;
		romData.Info.MapperID = 173;
		romData.Info.SubMapperID = 0;
		romData.Info.System = GameSystem::Dendy;
		romData.Info.Mirroring = MirroringType::Horizontal;
		romData.Info.FilePrgOffset = headerSize;

		romData.CdvHeader.assign(file.begin(), file.begin() + headerSize);

		//The sizes are counted the way the machine counts them: PRG in 8KB units, CHR in 8KB
		uint32_t prgSize = (uint32_t)(file[0x08] >> 1) * 0x4000;
		uint32_t chrSize = (uint32_t)file[0x09] * 0x2000;
		uint32_t offset = headerSize;

		//A game may carry a block of code ahead of the PRG for the machine to place in RAM.
		//Its length is counted in 512-byte units from a floppy and 2KB units from a disc.
		if(file[0x0A] && file[0x0D]) {
			uint32_t trainerSize = (uint32_t)file[0x0D] << ((headerSize == LargeHeader) ? 11 : 9);
			if(offset + trainerSize <= file.size()) {
				romData.CdvTrainer.assign(file.begin() + offset, file.begin() + offset + trainerSize);
				offset += trainerSize;
			}
		}

		if(prgSize == 0 || offset + prgSize + chrSize > file.size()) {
			MessageManager::Log("[CDV] Invalid file (sizes do not match the file length) - load operation cancelled.");
			romData.Error = true;
			return;
		}

		uint8_t* prg = file.data() + offset;
		romData.PrgRom.insert(romData.PrgRom.end(), prg, prg + prgSize);

		//Not ChrRom: the machine has none, and the mapper is built around its CHR RAM
		romData.CdvChr.insert(romData.CdvChr.end(), prg + prgSize, prg + prgSize + chrSize);

		romData.Info.Hash.PrgCrc32 = CRC32::GetCRC(prg, prgSize);
		romData.Info.Hash.PrgChrCrc32 = CRC32::GetCRC(prg, prgSize + chrSize);

		MessageManager::Log("[CDV] Header size: " + std::to_string(headerSize) + " bytes");
		MessageManager::Log("[CDV] PRG: " + std::to_string(prgSize / 1024) + " KB");
		MessageManager::Log("[CDV] CHR: " + std::to_string(chrSize / 1024) + " KB");
		if(!romData.CdvTrainer.empty()) {
			MessageManager::Log("[CDV] Startup code: " + std::to_string(romData.CdvTrainer.size()) + " bytes");
		}
	}
};
