#pragma once
#include "pch.h"
#include "NES/Mappers/CdImageFile.h"
#include "NES/Mappers/Bung/DrPcJrCdDrive.h"
#include "Utilities/ISerializable.h"
#include "Utilities/Serializer.h"
#include "Utilities/FolderUtilities.h"
#include "Shared/MessageManager.h"

//The 中索 (Zoson) G2002's CD drive, as its BIOS talks to it. The G2002 is a VCD player with a
//灵童 computer built in, and the computer reaches the disc through the player's controller
//over two ports:
//  $5680  write: a command byte. Read: the next byte of the answer.
//  $5681  read: bit 7 = an answer byte is waiting, bit 6 = not ready for a command byte.
//         Write: $C0 while the computer has the screen, 0 as it hands over to the player.
//
//What the BIOS sends - a single byte, except the three that carry five more:
//  $AA m s f n 0   read n sectors from m:s:f - binary, not BCD, counted from the 150 frames
//                  before the first sector - and answer with the 2048 data bytes of each
//  $A6             status, six bytes. 0-2: track, minute, second for the player's display
//                  ($FF shows as blank). Byte 4 = $08: no disc (the BIOS shows 没有盘片 and
//                  asks again until there is one). Byte 5 = $20: tray open (开门).
//  $B2             six bytes; the first is nonzero when there is a disc to read as data. Zero
//                  sends the BIOS on to the player program.
//  $B7             the player's keys, six bytes: byte 3 nonzero = a key, byte 0 its code
//  $82             six bytes of status; nothing found that reads them
//  $B0 S T A R 0   answered "STAR" and two more bytes
//  $B4 k ....      a player button; no answer
//  $B8 $B9 $BA     no answer
//  $FF             no answer. The BIOS opens with seven of them, so one drops whatever was
//                  half sent or not yet read.
//The BIOS itself checks the disc is one of its own: the volume name must start "STAR", and its
//fifth letter tells the disc of office programs (D) from the menu discs (C).
//
//Only what the computer side needs is here. The player's own work - playing a VCD and putting
//its picture on the screen - is not emulated, so a video disc answers as data and the BIOS's
//volume check is what turns it away.
class ZosonCdDrive final : public ISerializable
{
private:
	static constexpr uint32_t SectorSize = 0x800;
	static constexpr uint32_t LeadIn = 150;

	CdImageFile _image;
	string _discPath;

	uint8_t _command[6] = {};
	uint8_t _commandLength = 0;
	uint8_t _commandSize = 0;

	uint8_t _reply[6] = {};
	uint8_t _replyLength = 0;
	uint8_t _replyPos = 0;

	//A read in progress: the sector being sent and how much of it is left
	uint32_t _readLba = 0;
	uint32_t _readSectorsLeft = 0;
	uint32_t _readPos = 0;
	uint8_t _sector[SectorSize] = {};
	uint32_t _sectorLba = UINT32_MAX;

	uint8_t _control = 0;

	static uint8_t CommandSize(uint8_t command)
	{
		switch(command) {
			case 0xAA: case 0xB0: case 0xB4: return 6;
			default: return 1;
		}
	}

	void Answer(std::initializer_list<uint8_t> bytes)
	{
		_replyLength = 0;
		for(uint8_t b : bytes) {
			_reply[_replyLength++] = b;
		}
		_replyPos = 0;
	}

	void DropAnswer()
	{
		_replyLength = 0;
		_replyPos = 0;
		_readSectorsLeft = 0;
		_readPos = 0;
	}

	void RunCommand()
	{
		DropAnswer();
		switch(_command[0]) {
			case 0xAA: {
				uint32_t frames = ((uint32_t)_command[1] * 60 + _command[2]) * 75 + _command[3];
				_readLba = frames >= LeadIn ? frames - LeadIn : 0;
				_readSectorsLeft = _command[4];
				_readPos = 0;
				break;
			}

			case 0xA6: Answer({ 0xFF, 0xFF, 0xFF, 0, (uint8_t)(IsMounted() ? 0 : 0x08), 0 }); break;
			case 0xB2: Answer({ (uint8_t)(IsMounted() ? 0x01 : 0), 0, 0, 0, 0, 0 }); break;
			case 0xB7: case 0x82: Answer({ 0, 0, 0, 0, 0, 0 }); break;
			case 0xB0: Answer({ 'S', 'T', 'A', 'R', 0, 0 }); break;
			default: break;
		}
	}

	uint8_t NextDataByte()
	{
		if(_sectorLba != _readLba) {
			memset(_sector, 0, sizeof(_sector));
			if(IsMounted()) {
				_image.Read((uint64_t)_readLba * SectorSize, SectorSize, _sector);
			}
			_sectorLba = _readLba;
		}
		uint8_t value = _sector[_readPos++];
		if(_readPos >= SectorSize) {
			_readPos = 0;
			_readLba++;
			_readSectorsLeft--;
		}
		return value;
	}

public:
	bool IsMounted() { return _image.IsOpen(); }
	string GetDiscPath() { return _discPath; }

	bool LoadDisc(string path)
	{
		string ext = FolderUtilities::GetExtension(path);
		string imagePath = ext == ".cue" ? DrPcJrCdDrive::ResolveCueSheet(path) : path;
		if(!_image.Open(imagePath)) {
			return false;
		}
		_discPath = path;
		_sectorLba = UINT32_MAX;
		MessageManager::Log("[Zoson] Disc mounted: " + FolderUtilities::GetFilename(path, true));
		return true;
	}

	void EjectDisc()
	{
		_image.Close();
		_discPath.clear();
		_sectorLba = UINT32_MAX;
	}

	void Reset()
	{
		_commandLength = 0;
		_commandSize = 0;
		DropAnswer();
		_control = 0;
	}

	uint8_t ReadStatus()
	{
		return (_replyPos < _replyLength || _readSectorsLeft > 0) ? 0x80 : 0x00;
	}

	uint8_t ReadData()
	{
		if(_replyPos < _replyLength) {
			return _reply[_replyPos++];
		}
		if(_readSectorsLeft > 0) {
			return NextDataByte();
		}
		return 0;
	}

	void WriteCommand(uint8_t value)
	{
		if(_commandLength == 0) {
			if(value == 0xFF) {
				DropAnswer();
				return;
			}
			_commandSize = CommandSize(value);
		}
		_command[_commandLength++] = value;
		if(_commandLength >= _commandSize) {
			_commandLength = 0;
			RunCommand();
		}
	}

	void WriteControl(uint8_t value) { _control = value; }

	void Serialize(Serializer& s) override
	{
		SVArray(_command, 6);
		SV(_commandLength);
		SV(_commandSize);
		SVArray(_reply, 6);
		SV(_replyLength);
		SV(_replyPos);
		SV(_readLba);
		SV(_readSectorsLeft);
		SV(_readPos);
		SV(_control);
		if(!s.IsSaving()) {
			_sectorLba = UINT32_MAX;
		}
	}
};
