#pragma once
#include "pch.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/StringUtilities.h"
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
struct DiscProgram
{
	uint32_t Lba;
	uint32_t Size;
	string Name;
	string Title;
};

class YuxingVcdDrive
{
private:
	//Commands the drive answers, as {command, byte count, status byte}
	static constexpr uint8_t CommandTable[6][3] = {
		{ 0x00, 1, 0x01 }, { 0x96, 1, 0x69 }, { 0xAA, 1, 0x06 },
		{ 0x06, 1, 0x00 }, { 0x15, 1, 0x00 }, { 0xA5, 6, 0x06 }
	};

	//What the drive serves: one program's bytes. A whole-disc image is kept beside it in
	//_image so another program can be selected without re-reading the file.
	vector<uint8_t> _disc;
	vector<uint8_t> _image;
	vector<DiscProgram> _programs;
	int32_t _programIndex = -1;
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

	//True once a program is being served. A disc can be mounted with none chosen yet, so
	//this is NOT the test for "is there a disc" - see HasDisc().
	bool IsDiscInserted() { return !_disc.empty(); }

	//True while any disc is mounted, chosen program or not
	bool HasDisc() { return !_disc.empty() || !_programs.empty(); }

	//A whole-disc image offers its programs to the media-swap UI; a single extracted
	//program has none to offer, and the folder's files are listed instead.
	uint32_t GetProgramCount() { return (uint32_t)_programs.size(); }
	//What the media list shows: the disc's own filename, the title if a sidecar gave one, and
	//the size, which is the one thing that says whether a program will run at all here
	string GetProgramName(uint32_t index)
	{
		if(index >= _programs.size()) {
			return string();
		}

		const DiscProgram& program = _programs[index];
		string label = program.Name;
		if(!program.Title.empty()) {
			label += " - " + program.Title;
		}
		return label + " (" + std::to_string(program.Size / 1024) + "KB)";
	}
	int32_t GetProgramIndex() { return _programIndex; }

	//Point the drive's view at one program without disturbing the protocol state - which is
	//what restoring a savestate needs, since Reset() would wipe what was just read back.
	bool PointAtProgram(uint32_t index)
	{
		if(index >= _programs.size()) {
			return false;
		}

		const DiscProgram& program = _programs[index];
		size_t start = (size_t)program.Lba * 0x800;
		if(start + program.Size > _image.size()) {
			return false;
		}

		_disc.assign(_image.begin() + start, _image.begin() + start + program.Size);
		_programIndex = (int32_t)index;
		return true;
	}

	bool SelectProgram(uint32_t index)
	{
		if(!PointAtProgram(index)) {
			return false;
		}
		Reset();
		return true;
	}
	string GetDiscFilename() { return _discPath; }
	bool IsReadComplete() { return _readComplete; }

	//An ISO9660 directory record: length at 0, extent LBA at 2, data length at 10, flags at
	//25, name length at 32, name at 33. Walks one directory looking for a name, and reports
	//where what it found lives.
	bool FindIsoEntry(uint32_t dirLba, uint32_t dirLen, string prefix, bool wantDir, uint32_t& outLba, uint32_t& outLen)
	{
		if((uint64_t)dirLba * 0x800 + dirLen > _image.size()) {
			return false;
		}

		const uint8_t* dir = _image.data() + (size_t)dirLba * 0x800;
		uint32_t pos = 0;
		while(pos < dirLen) {
			uint8_t recLen = dir[pos];
			if(recLen == 0) {
				//Records never straddle a sector - skip to the next one
				pos = (pos / 0x800 + 1) * 0x800;
				continue;
			}
			if(pos + recLen > dirLen || recLen < 34) {
				break;
			}

			uint8_t nameLen = dir[pos + 32];
			bool isDir = (dir[pos + 25] & 0x02) != 0;
			if(nameLen >= prefix.size() && isDir == wantDir && pos + 33 + nameLen <= dirLen &&
				memcmp(dir + pos + 33, prefix.c_str(), prefix.size()) == 0) {
				memcpy(&outLba, dir + pos + 2, 4);
				memcpy(&outLen, dir + pos + 10, 4);
				return true;
			}
			pos += recLen;
		}
		return false;
	}

	//A whole-disc image begins with the disc's own filesystem; the drive's view has to begin
	//with a program, which is what a single extracted .bin already hands it. Catalogue what the
	//disc holds so one of them can be picked.
	bool ScanIsoPrograms()
	{
		if(_image.size() < 17 * 0x800 || memcmp(_image.data() + 16 * 0x800 + 1, "CD001", 5) != 0) {
			return false;
		}

		const uint8_t* root = _image.data() + 16 * 0x800 + 156;
		uint32_t rootLba = 0, rootLen = 0;
		memcpy(&rootLba, root + 2, 4);
		memcpy(&rootLen, root + 10, 4);

		uint32_t dirLba = 0, dirLen = 0;
		if(!FindIsoEntry(rootLba, rootLen, "PROGRAMS", true, dirLba, dirLen)) {
			return false;
		}

		CollectIsoFiles(dirLba, dirLen);
		if(_programs.empty()) {
			return false;
		}

		MessageManager::Log("[YuXing] Disc holds " + std::to_string(_programs.size()) + " programs");
		return true;
	}

	//The disc carries no titles in text form - they only exist as pixels in the MPEG stills
	//its player menu was drawn from. A sidecar next to the image supplies them instead: lines
	//of "FILE0007=1944", blank lines and # comments ignored.
	void LoadProgramNames(string imagePath)
	{
		size_t dot = imagePath.find_last_of('.');
		string sidecar = (dot == string::npos ? imagePath : imagePath.substr(0, dot)) + ".txt";
		ifstream names(sidecar, ios::in);
		if(!names) {
			return;
		}

		uint32_t matched = 0;
		string line;
		while(std::getline(names, line)) {
			while(!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
				line.pop_back();
			}
			if(line.empty() || line[0] == '#') {
				continue;
			}

			size_t eq = line.find('=');
			if(eq == string::npos) {
				continue;
			}

			string key = line.substr(0, eq);
			string title = line.substr(eq + 1);

			//A trailing comment is not part of the title
			size_t comment = title.find('#');
			if(comment != string::npos) {
				title = title.substr(0, comment);
			}
			while(!title.empty() && (title.front() == ' ' || title.front() == '	')) {
				title.erase(title.begin());
			}
			while(!title.empty() && (title.back() == ' ' || title.back() == '	')) {
				title.pop_back();
			}
			if(title.empty()) {
				continue;
			}
			for(DiscProgram& program : _programs) {
				//Match on the stem, so "FILE0007" finds "FILE0007.BIN"
				if(program.Name.compare(0, key.size(), key) == 0) {
					program.Title = title;
					matched++;
					break;
				}
			}
		}

		if(matched > 0) {
			MessageManager::Log("[YuXing] " + std::to_string(matched) + " program names from " + sidecar);
		}
	}

	//Every file in one directory, in the order the disc lists them
	void CollectIsoFiles(uint32_t dirLba, uint32_t dirLen)
	{
		if((uint64_t)dirLba * 0x800 + dirLen > _image.size()) {
			return;
		}

		const uint8_t* dir = _image.data() + (size_t)dirLba * 0x800;
		uint32_t pos = 0;
		while(pos < dirLen) {
			uint8_t recLen = dir[pos];
			if(recLen == 0) {
				pos = (pos / 0x800 + 1) * 0x800;
				continue;
			}
			if(pos + recLen > dirLen || recLen < 34) {
				break;
			}

			uint8_t nameLen = dir[pos + 32];
			bool isDir = (dir[pos + 25] & 0x02) != 0;
			if(!isDir && nameLen > 0 && pos + 33 + nameLen <= dirLen) {
				DiscProgram program = {};
				memcpy(&program.Lba, dir + pos + 2, 4);
				memcpy(&program.Size, dir + pos + 10, 4);
				program.Name = string((const char*)dir + pos + 33, nameLen);

				//ISO9660 names carry a ";1" version suffix that means nothing here
				size_t version = program.Name.find(';');
				if(version != string::npos) {
					program.Name = program.Name.substr(0, version);
				}

				if(program.Size > 0 && (uint64_t)program.Lba * 0x800 + program.Size <= _image.size()) {
					_programs.push_back(program);
				}
			}
			pos += recLen;
		}
	}

	//A .cue names the image that goes with it; take the first FILE line and load that
	static string ResolveCueSheet(string path)
	{
		ifstream cue(path, ios::in);
		if(!cue) {
			return path;
		}

		string line;
		while(std::getline(cue, line)) {
			size_t start = line.find('"');
			size_t end = start == string::npos ? string::npos : line.find('"', start + 1);
			if(line.find("FILE") != string::npos && end != string::npos) {
				string named = line.substr(start + 1, end - start - 1);
				string resolved = FolderUtilities::CombinePath(FolderUtilities::GetFolderName(path), named);
				if(StringUtilities::IsValidUtf8(named) && ifstream(resolved, ios::in | ios::binary)) {
					return resolved;
				}

				//The name written inside a sheet is in whatever code page made it, and the
				//discs here name their image in GBK while the file on disk carries the same
				//characters as UTF-8 - the two never match, and feeding those bytes to
				//std::filesystem throws. The pair share a stem in every dump seen, so fall
				//back to the sheet's own name with the extension it named.
				size_t sep = path.find_last_of("/\\");
				size_t cueDot = path.find_last_of('.');
				size_t namedDot = named.find_last_of('.');
				if(namedDot != string::npos && cueDot != string::npos && (sep == string::npos || cueDot > sep)) {
					string alt = path.substr(0, cueDot) + named.substr(namedDot);
					if(ifstream(alt, ios::in | ios::binary)) {
						return alt;
					}
				}
				return resolved;
			}
		}
		return path;
	}

	bool LoadDisc(string path)
	{
		string ext = path.size() >= 4 ? path.substr(path.size() - 4) : string();
		std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return (char)::tolower((uint8_t)c); });
		string imagePath = ext == ".cue" ? ResolveCueSheet(path) : path;

		ifstream file(imagePath, ios::in | ios::binary);
		if(!file) {
			return false;
		}

		file.seekg(0, ios::end);
		size_t size = (size_t)file.tellg();
		file.seekg(0, ios::beg);

		//A whole disc is ~700MB at most; a single program is a few hundred KB
		if(size == 0 || size > 800 * 1024 * 1024) {
			return false;
		}

		vector<uint8_t> raw((size_t)size);
		file.read((char*)raw.data(), size);

		//A raw dump carries the full 2352-byte sector: 12 sync bytes, a 4-byte header, an
		//8-byte subheader on Mode 2, then the payload, then EDC/ECC. The drive only ever
		//serves payload, so flatten it here and everything downstream keeps working on plain
		//2KB sectors. Anything else (a .iso, or a single extracted program) is already flat.
		static const uint8_t Sync[12] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
		_image.clear();
		_programs.clear();
		_programIndex = -1;

		if(size >= 2352 && size % 2352 == 0 && memcmp(raw.data(), Sync, sizeof(Sync)) == 0) {
			size_t sectors = size / 2352;
			_image.resize(sectors * 0x800);
			for(size_t i = 0; i < sectors; i++) {
				//Form 2 carries 2324 bytes and no EDC/ECC, but only its first 2048 are ever
				//addressed the way this drive addresses data
				memcpy(_image.data() + i * 0x800, raw.data() + i * 2352 + 24, 0x800);
			}
			MessageManager::Log("[YuXing] Disc image: " + std::to_string(sectors) + " raw sectors flattened to 2KB");
		} else {
			_image = std::move(raw);
		}

		//A disc holds programs. None is started here - the machine comes up on its own side
		//and the disc's programs are offered through the media list, which is as close to the
		//player's own menu as this can get without decoding the MPEG stills it was drawn from.
		if(ScanIsoPrograms()) {
			LoadProgramNames(imagePath);
			_disc.clear();
			_programIndex = -1;
			Reset();
		} else {
			//Not a disc - a single extracted program, which is already the drive's view
			_disc = std::move(_image);
			_image.clear();
			Reset();
		}

		_discPath = path;
		return true;
	}

	void EjectDisc()
	{
		_disc.clear();
		_image.clear();
		_programs.clear();
		_programIndex = -1;
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
		SV(_programIndex);

		//The disc is not part of a savestate, so the positions above only mean anything if the
		//same disc is still mounted - re-point the view at the program they belong to. Without
		//this the drive keeps serving whatever was selected now (nothing, on a fresh load) and
		//every read comes back as zero.
		if(!s.IsSaving() && _programIndex >= 0) {
			PointAtProgram((uint32_t)_programIndex);
		}
	}
};
