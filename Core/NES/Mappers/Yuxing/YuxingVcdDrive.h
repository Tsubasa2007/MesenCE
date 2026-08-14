#pragma once
#include "pch.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/Serializer.h"

//YuXing VCD drive - the CD-ROM the V9.2 models boot their software from, emulated at the
//command level rather than the disc level. Ported from the VirtuaNES-BBK fork
//(NES/VCD.hpp, YXCDDriver).
//
//The BIOS talks to the drive over the controller port: a command byte is shifted out one
//bit at a time through $4016 writes, the drive's status byte is shifted back through
//$4016/$4017 reads, and each byte of the addressed sector is then fetched from $4207.
//The drive's own firmware is not emulated - only the handful of commands the BIOS issues
//are recognised, and the sector address they carry is turned into an offset into the
//disc image file:
// - $A5: seek. The first one homes to the image's base sector (which of three offsets
//   depends on a signature at offset $10); later ones carry a minute/second/frame address
//   that is converted relative to the base sector recorded by the following $06.
// - $06: acknowledge the seek and latch the base sector on the first pass.
// - $15: step to the next sector, or restart at $4800 after a successful $06.
//
//The same serial link also carries the machine's keyboard while the VCD screen is up: the
//BIOS selects it by writing $FF then $FE to $4016, and clocks a 16-bit word back out
//holding one key's matrix position and the modifier state.
class YuxingVcdDrive
{
private:
	//Commands the drive answers, as {command, byte count, status byte}
	static constexpr uint8_t CommandTable[6][3] = {
		{ 0x00, 1, 0x01 }, { 0x96, 1, 0x69 }, { 0xAA, 1, 0x06 },
		{ 0x06, 1, 0x00 }, { 0x15, 1, 0x00 }, { 0xA5, 6, 0x06 }
	};

	vector<uint8_t> _disc;
	string _discPath;

	uint8_t _cmd[20] = {};
	uint8_t _baseSector[3] = {};
	//Status byte per $4016 command value. The reference emulator sizes this 0xFF even
	//though the index is a full byte; widened to 0x100 so a stray value cannot run off it.
	uint8_t _statusByCmd[0x100] = {};
	uint8_t _cmdSel = 0;
	uint8_t _keyByteIndex = 0;
	uint8_t _shiftIn = 0;
	uint8_t _status = 0;
	int32_t _cmdIndex = 0;

	int32_t _pos = 0;
	int32_t _basePos = 0;
	int32_t _seekPos = 0;

	uint16_t _keySend = 0;
	int32_t _keySendBit = 0;
	uint16_t _keySelect = 0;

	bool _move = false;
	bool _shifting = false;
	bool _canReadData = false;
	bool _seekOk = false;
	bool _driveSelected = true;
	bool _keyboardSelected = false;
	bool _readComplete = false;

	const uint8_t* FindCommand(uint8_t cmd)
	{
		for(uint32_t i = 0; i < sizeof(CommandTable) / sizeof(CommandTable[0]); i++) {
			if(CommandTable[i][0] == cmd) {
				return CommandTable[i];
			}
		}
		return nullptr;
	}

	//A complete command has been shifted in - act on it and latch the status byte
	void UpdateStatus()
	{
		_cmd[_cmdIndex] = _shiftIn;

		const uint8_t* pCmd = _cmdIndex ? FindCommand(_cmd[0]) : FindCommand(_shiftIn);
		if(pCmd) {
			_status = pCmd[2];
		} else {
			_cmdIndex = 0;
			return;
		}

		if(++_cmdIndex < pCmd[1]) {
			return;
		}
		_cmdIndex = 0;

		switch(pCmd[0]) {
			case 0x06:
				if(_baseSector[2] == 0xFE) {
					memcpy(_baseSector, &_cmd[1], 3);
					_basePos = _seekPos;
				}
				_seekOk = true;
				break;

			case 0x15:
				//After a good seek the BIOS expects the first data sector, otherwise this
				//just walks forward one sector at a time
				_seekPos = _seekOk ? 0x4800 : (_seekPos + 0x800);
				_seekOk = false;
				if(_seekPos + 0x800 > (int32_t)_disc.size()) {
					_seekPos = 0;
				}
				_pos = _seekPos;
				break;

			case 0xA5:
				if(_baseSector[2] == 0xFF) {
					//First seek of the session - home to the image's base sector
					_baseSector[2] = 0xFE;
					_pos = 0xC800;
					if(_disc.size() > 0x12) {
						if(_disc[0x10] == 0x46 && _disc[0x11] == 0xFC) { _pos = 0x8800; }
						if(_disc[0x10] == 0x15 && _disc[0x11] == 0xDF) { _pos = 0x9000; } //older images
					}
				} else {
					//Minute/second/frame address, relative to the latched base sector
					int32_t minutes = (int32_t)_cmd[1] - (int32_t)_baseSector[0];
					int32_t seconds = (int32_t)_cmd[2] - (int32_t)_baseSector[1];
					int32_t frames = (int32_t)_cmd[3] - (int32_t)_baseSector[2];
					_pos = ((seconds + (minutes & 1) * 0x3C) * 0x4B + frames) * 0x800 + _basePos;
					//The reference emulator clamps this at zero only; a sector address past
					//the end of the image would then index straight off the buffer. Nothing
					//seen so far seeks there, but the drive must not be able to read out of
					//bounds on a malformed or truncated image.
					if(_pos < 0 || _pos >= (int32_t)_disc.size()) {
						_pos = 0;
					}
				}
				_seekPos = _pos;
				break;
		}
	}

	uint8_t KeyRead()
	{
		if(--_keySendBit <= 0) {
			_keyboardSelected = false;
		}
		return (uint8_t)((_keySend >> _keySendBit) & 1);
	}

public:
	//Set by the mapper before every KeyWrite, from the YuxingKeyboard device
	int32_t PendingKeyCell = -1;
	uint8_t PendingModifiers = 0;

	void Reset()
	{
		memset(_cmd, 0, sizeof(_cmd));
		memset(_baseSector, 0, sizeof(_baseSector));
		memset(_statusByCmd, 0, sizeof(_statusByCmd));
		_cmdSel = _keyByteIndex = _shiftIn = _status = 0;
		_keySend = 0;
		_keySelect = 0;
		_pos = _basePos = _seekPos = _cmdIndex = _keySendBit = 0;
		_move = _shifting = _canReadData = _seekOk = _readComplete = _keyboardSelected = false;
		_driveSelected = true;
		_baseSector[2] = 0xFF;
		_statusByCmd[2] = _statusByCmd[6] = 0x0F;

		const uint8_t* pCmd = FindCommand(0);
		_status = pCmd ? pCmd[2] : 0;
	}

	YuxingVcdDrive() { Reset(); }

	bool IsDiscInserted() { return !_disc.empty(); }
	string GetDiscFilename() { return _discPath; }
	bool IsReadComplete() { return _readComplete; }

	bool LoadDisc(string path)
	{
		ifstream file(path, ios::in | ios::binary);
		if(!file) {
			return false;
		}

		file.seekg(0, ios::end);
		size_t size = (size_t)file.tellg();
		file.seekg(0, ios::beg);

		if(size == 0 || size > 8 * 1024 * 1024) {
			return false;
		}

		_disc.resize(size);
		file.read((char*)_disc.data(), size);
		_discPath = path;
		Reset();
		return true;
	}

	void EjectDisc()
	{
		_disc.clear();
		_discPath.clear();
		Reset();
	}

	//Returns true when the drive drove the bus - the caller must not fall through to the
	//key matrix or the controller port in that case
	bool Read(uint16_t addr, uint8_t& data)
	{
		if(_disc.empty()) {
			return false;
		}

		switch(addr) {
			case 0x4016:
				if(_keyboardSelected) {
					if(_keyByteIndex != 1 && _keyByteIndex != 2) {
						return false;
					}
					_keyByteIndex = 2;
					data = KeyRead();
					return true;
				}
				if(_driveSelected) {
					data = (_status & 1) ? 0 : 0x0F;
					_shifting = false;
				}
				return true;

			case 0x4017:
				if(_keyboardSelected) {
					if(_keyByteIndex != 2 && _keyByteIndex) {
						_keyByteIndex = 0;
						return false;
					}
					data = KeyRead();
					_keyByteIndex = 0;
					return true;
				}
				if(_driveSelected) {
					data = _statusByCmd[_cmdSel];
					_canReadData = true;
				}
				return true;

			case 0x4207:
				if(!_canReadData || !_driveSelected) {
					return false;
				}
				_canReadData = false;
				//The base-sector seek homes to a fixed offset that several of the smaller
				//images are shorter than (the 34KB mouse titles are seeked to $C800), so the
				//position genuinely can sit past the end - read those as blank rather than
				//off the end of the buffer.
				data = _pos >= 0 && _pos < (int32_t)_disc.size() ? _disc[_pos] : 0;
				if(++_pos >= (int32_t)_disc.size()) {
					_readComplete = true;
				}
				return true;
		}
		return false;
	}

	bool Write(uint16_t addr, uint8_t value)
	{
		if(_disc.empty()) {
			return false;
		}

		switch(addr) {
			//A key matrix row select cancels a pending data fetch
			case 0x4202: case 0x4203: case 0x4302: case 0x4303:
				_canReadData = false;
				return false;

			case 0x4016: {
				if(_move) {
					_move = false;
					_cmdSel = value;
					if(value == 0 || value == 4) {
						//One more command bit, MSB first
						_shiftIn = (uint8_t)((_shiftIn >> 1) | (value << 5));
						return true;
					}
				}

				if(value == 0 || value == 1) {
					//Plain controller strobe
					_driveSelected = false;
					return true;
				}

				if(value == 0xFF || value == 0xFE) {
					_driveSelected = false;
					KeyWrite(value);
					return true;
				}

				bool used = true;
				switch(value) {
					case 2: _move = true; _shifting = false; break;
					case 4: _status >>= 1; _shifting = true; break;
					case 6: if(!_shifting) { UpdateStatus(); } break;
					default: used = false; break;
				}
				if(used) {
					_driveSelected = true;
					_cmdSel = value;
				}
				return true;
			}
		}
		return false;
	}

	//$FF followed by $FE selects the keyboard and latches one key for transmission
	void KeyWrite(uint8_t value)
	{
		_keySelect = (uint16_t)((_keySelect << 8) | value);
		if(_keySelect != 0xFFFE) {
			return;
		}

		_keyboardSelected = true;
		if(_keyByteIndex) {
			return;
		}
		_keyByteIndex = 1;

		uint8_t key = PendingKeyCell >= 0 ? (uint8_t)~PendingKeyCell : 0;
		uint8_t mod = PendingModifiers;

		uint32_t high = ((((key >> 2) & 0x18) | 3) | (~mod & 0xE0)) & (key ? 0xFF : 0xFC);
		uint32_t low = (((uint32_t)key << 3) | 3) & 0xFB;
		_keySend = (uint16_t)(((high << 8) & 0xFF00) | low);
		_keySendBit = 16;
	}

	void Serialize(Serializer& s)
	{
		SVArray(_cmd, 20); SVArray(_baseSector, 3); SVArray(_statusByCmd, 0x100);
		SV(_cmdSel); SV(_keyByteIndex); SV(_shiftIn); SV(_status); SV(_cmdIndex);
		SV(_pos); SV(_basePos); SV(_seekPos);
		SV(_keySend); SV(_keySendBit); SV(_keySelect);
		SV(_move); SV(_shifting); SV(_canReadData); SV(_seekOk);
		SV(_driveSelected); SV(_keyboardSelected); SV(_readComplete);
	}
};
