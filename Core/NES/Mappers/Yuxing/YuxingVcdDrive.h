#pragma once
#include "pch.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/StringUtilities.h"
#include "Utilities/Serializer.h"
#include "NES/Mappers/CdImageFile.h"
#include "NES/Mappers/CdSegmentIndex.h"
#include "NES/Mappers/Yuxing/YuxingVcdMenu.h"

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
	static constexpr uint8_t CommandTable[11][3] = {
		{ 0x00, 1, 0x01 }, { 0x96, 1, 0x69 }, { 0xAA, 1, 0x06 },
		{ 0x06, 1, 0x00 }, { 0x15, 1, 0x00 }, { 0xA5, 6, 0x06 },

		//The set above is everything the BIOS asks for, which is all a program that only
		//wants its own bytes off the disc ever needs. A program that drives the disc as a
		//player - telling it what to put on screen and where to put the pointer over it -
		//speaks a wider vocabulary, and a command missing from this table is not merely
		//ignored: the status byte keeps whatever the previous command left, so the answer
		//the program reads back is a stale one from a question it did not ask. It retries
		//forever.
		//
		//Every length here is the packet the program itself assembles, counted from the
		//code that builds it: a command byte, its parameters, and a checksum that is the
		//sum of everything before it. Each is acknowledged with $06, which is the value
		//the program compares against in each case.
		//
		//Answering them is not the same as showing anything. A program written this way
		//draws nothing of its own - no pattern-table writes, no sprites, rendering left on
		//over an empty name table - because every pixel it means the machine to show is a
		//still or a sequence off the disc, composited by the drive. Until that is decoded
		//the screen stays the colour of the backdrop, which is what the hardware itself
		//would show with the video switched off - not a fault in the link below.
		{ 0xA6, 6, 0x06 },  //place the pointer: a shape number, then its two coordinates -
		                    //one byte for the short axis, a 16-bit pair for the long one.
		                    //The program keeps all three in zero page and steps them by a
		                    //fixed amount per key press, which is its whole response to
		                    //input: it hit-tests the pointer against a table of rectangles
		                    //and never puts it on screen itself.
		{ 0xAC, 6, 0x06 },  //show something: a type in the low bits of the first parameter,
		                    //then a 16-bit number naming what
		{ 0xA8, 3, 0x06 },  //one parameter byte
		{ 0xAD, 2, 0x06 },  //report status - answered again below
		{ 0xAF, 2, 0x06 }
	};

	//Where the machine last said to put the pointer, and which shape to put there
	static constexpr uint32_t PointerLeft = 32;
	static constexpr uint32_t PointerRight = 642;
	static constexpr uint32_t PointerTop = 20;
	static constexpr uint32_t PointerBottom = 254;
	//Which channels the machine last asked to hear - see the $AC handler
	uint8_t _audioChannels = 3;
	bool _pointerShown = false;
	uint8_t _pointerShape = 0;
	uint32_t _pointerX = PointerLeft;
	uint32_t _pointerY = PointerTop;

	//The menu the disc itself carries, for a disc that holds a library of programs - see
	//YuxingVcdMenu. A disc with one program has nothing to choose and never opens one.
	YuxingVcdMenu _menu;

	//What the drive serves: one program's bytes. The disc it came off stays on disk and is
	//read from as programs are picked, rather than being held here - see CdImageFile.
	vector<uint8_t> _disc;
	CdImageFile _image;
	vector<DiscProgram> _programs;
	int32_t _programIndex = -1;
	string _discPath;
	vector<CdTrack> _tracks;

	uint8_t _cmd[20] = {};
	uint8_t _baseSector[3] = {};
	//Status byte per $4016 command value. The reference emulator sizes this 0xFF even
	//though the index is a full byte; widened to 0x100 so a stray value cannot run off it.
	uint8_t _statusByCmd[0x100] = {};
	uint8_t _cmdSel = 0;
	uint8_t _keyByteIndex = 0;
	uint8_t _shiftIn = 0;

	//A second answer, for the one command that is asked a question rather than told to do
	//something. See UpdateStatus.
	uint8_t _followUp = 0;
	bool _hasFollowUp = false;
	int32_t _shiftCount = 0;
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

	//Segment items are allocated a fixed stride each, so item N lives at a constant distance
	//from the first one. The directory is not a reliable way in: one disc lists 1081 items of
	//which 801 are placeholders with no extent at all, while the streams they name are
	//present and readable at exactly the address this gives. Where the first one is comes
	//from CdSegmentIndex, which works it out from the records that do describe one.
	static constexpr uint32_t SegmentStride = CdSegmentIndex::SegmentStride;
	uint32_t _segmentOrigin = 0;

	//Where each of the disc's segment items really is, taken from the directory that lists
	//them rather than worked out from a stride.
	//
	//Most of them are one fixed allocation each and the two ways of counting agree, which is
	//why a stride carried this far. They part company at the first item longer than one: it
	//takes as many allocations as it needs, the numbering of the files carries on past them,
	//and from there a stride is short by the difference. On one disc it is the 181st item that
	//is long, and every menu page after it came out as the middle of that item instead - one
	//of them a stretch with no picture in it at all.
	struct DiscSegmentItem
	{
		uint32_t Lba;
		uint32_t Sectors;
	};
	vector<DiscSegmentItem> _segmentItems;

	//And the same files in the order the directory lists them. A disc whose items are all one
	//allocation each numbers them the same way twice over; one that has a long item does not,
	//because a long item takes several numbers' worth of room and the files carry on past
	//them - so ITEM0181 is followed by ITEM0196, and the item numbered 182 is the one after
	//the long one rather than a file of that name.
	vector<DiscSegmentItem> _segmentOrder;

	//The shape of the disc's own layout, worked out from the records that DID read back
	//cleanly rather than assumed. Where every one of them sits at base + (n-1)*step the
	//disc lays its items out evenly, and that arithmetic answers for the numbers whose
	//own record is unreadable - which counting cannot, because dropping a bad record
	//moves every item after it up a place. Zero when the records do not agree on a
	//single stride, which is what a disc with a long item looks like.
	uint32_t _segmentEvenBase = 0;
	uint32_t _segmentEvenStep = 0;

	//Whether the records that read back cleanly are listed in ascending order of their own
	//numbers. When they are, counting down the list is meaningful and a number with no file
	//of its own is a disc whose numbering has run ahead of its names. When they are NOT -
	//one disc lists ITEM1083 seventh - the ordering carries no information at all, and only
	//the arithmetic can be trusted.
	bool _segmentNamesInOrder = true;

	//A video the machine has asked to show, waiting for the front end to take it
	bool _playPending = false;
	uint8_t _playTrack = 0xFF;
	uint32_t _playLba = 0;
	uint32_t _playSectors = 0;

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
			case 0xAD:
				//The only command that answers twice: the acknowledgement, and then a
				//byte the program reads straight afterwards. It keeps the low seven bits
				//and requires the top nibble to be $A, so the shape of the answer is
				//fixed even though what the drive would put in the rest of it is not.
				//The two bits it then tests are the ones that would say the drive is
				//busy; nothing here is, so they stay clear.
				_followUp = 0xA0;
				_hasFollowUp = true;
				_shiftCount = 0;
				break;

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

			case 0xA6:
				//"Put the pointer here": a shape, then where. The machine keeps the position
				//itself and only ever tells the drive about it, because the drive is what
				//draws it - nothing on this side of the link puts a single pixel on screen
				//while a disc program is running.
				//
				//The travel is what says how far it can go: driven into each corner it runs
				//32..642 across and 20..254 down, those being the limits the BIOS clamps to
				//so that the pointer stays on the picture. Taking them as the edges is what
				//turns them into somewhere on a 352x288 frame.
				_pointerShape = _cmd[1];
				_pointerX = ((uint32_t)_cmd[3] << 8) | _cmd[4];
				_pointerY = _cmd[2];
				_pointerShown = true;
				break;

			case 0xAC: {
				//"Show this", and what to show is named by the two bytes after the type. The
				//type's low two bits say which audio channels to play, bit 0 left and bit 1
				//right: a dictionary page carries its two words spoken at the same time, one
				//in each channel, and the program asks for the same item again with 1 or 2 to
				//say the word on that side. 3 is both, which is what a video or a menu page
				//asks for, and 0 is silence - a page turned without saying anything.
				_audioChannels = _cmd[1] & 0x03;
				RequestShow(((uint32_t)_cmd[2] << 8) | _cmd[3]);
				break;
			}

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

	//A disc numbers everything it can be told to show in one series: 1 to 99 are the
	//sequence items, which are its video tracks, and 1000 upwards are the segment items -
	//the stills and short pieces kept in /SEGMENT. So a number says which of the two kinds
	//it is as well as which one, and the segment items count from 1000 rather than from 1.
	//
	//Read as plain segment numbers instead, everything is off by the best part of a
	//thousand: a menu asked for by number 1002 came out as whatever lies 999 items further
	//along, which on one disc is a page from the middle of its dictionary and on the same
	//disc's game screens is past the end of the disc altogether.
	static constexpr uint32_t FirstSegmentNumber = 1000;
	static constexpr uint32_t FirstSequenceNumber = 2;
	static constexpr uint32_t LastSequenceNumber = 99;

	//Where a segment item's stream begins and how far it runs, or nothing if this disc has
	//none. The directory is the answer where there is one; the stride is what is left when a
	//disc keeps its items somewhere this cannot read.
	bool SegmentItem(uint32_t item, uint32_t& lba, uint32_t& sectors)
	{
		if(item == 0) {
			return false;
		}
		//The file of that number if the disc has one - which is the answer that survives a
		//directory this cannot read cleanly all the way through
		if(item <= _segmentItems.size() && _segmentItems[item - 1].Sectors > 0) {
			lba = _segmentItems[item - 1].Lba;
			sectors = _segmentItems[item - 1].Sectors;
			return true;
		}

		//Otherwise the disc's own spacing, where its readable records all agree on one. This
		//has to come before counting: on a disc whose directory does not read cleanly the
		//n'th SURVIVING record is not the n'th item, and counting quietly answers with a
		//neighbour - asked for the letter A it handed back the page for G, several hundred
		//items away, because 801 of that disc's 1081 records carry an impossible address.
		if(_segmentEvenStep > 0) {
			lba = _segmentEvenBase + (item - 1) * _segmentEvenStep;
			sectors = _segmentEvenStep;
			return true;
		}

		//Otherwise the n'th file there is. A number with no file of its own belongs to a disc
		//whose numbering has run ahead of its names, and counting is what catches up - and a
		//disc like that is exactly the one whose records do not agree on a single stride.
		if(item <= _segmentOrder.size()) {
			lba = _segmentOrder[item - 1].Lba;
			sectors = _segmentOrder[item - 1].Sectors;
			return true;
		}
		if(_segmentOrigin == 0) {
			return false;
		}
		lba = _segmentOrigin + (item - 1) * SegmentStride;
		sectors = SegmentStride;
		return true;
	}

	//What the machine asked to show. Whether it is worth opening a player for is the front
	//end's judgement, not the drive's: most of what these discs carry is a single frame, and
	//it is the front end that knows what it can do with one.
	void RequestShow(uint32_t number)
	{
		if(number >= FirstSegmentNumber) {
			uint32_t item = number - FirstSegmentNumber + 1;
			uint32_t lba = 0, sectors = 0;
			if(!SegmentItem(item, lba, sectors)) {
				MessageManager::Log("[YuXing] Segment item " + std::to_string(item) +
					" asked for, but this disc's item layout was not recognised");
				return;
			}

			_playTrack = SegmentTrack;
			_playLba = lba;
			_playSectors = sectors;
			_playPending = true;
			MessageManager::Log("[YuXing] Program asked for segment item " + std::to_string(item) +
				" (number " + std::to_string(number) + ", sector " + std::to_string(lba) + ")");
			return;
		}

		if(number < FirstSequenceNumber || number > LastSequenceNumber) {
			MessageManager::Log("[YuXing] Program asked to show " + std::to_string(number) +
				", which is neither a track nor a segment item");
			return;
		}

		//A sequence item is one of the disc's own tracks, played through from its beginning,
		//and its number is that track's number. They start at two because the first track is
		//where the disc keeps its files rather than any video - which is also why the front
		//end counts videos from the second track, so what it wants is one less.
		_playTrack = (uint8_t)(number - 1);
		_playLba = 0;
		_playSectors = 0;
		_playPending = true;
		MessageManager::Log("[YuXing] Program asked for track " + std::to_string(number));
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
		_pos = _basePos = _seekPos = _cmdIndex = _keySendBit = _shiftCount = 0;
		_followUp = 0;
		_hasFollowUp = false;
		_move = _shifting = _canReadData = _seekOk = _readComplete = _keyboardSelected = false;
		_audioChannels = 3;
		_pointerShown = false;
		_pointerShape = 0;
		_pointerX = PointerLeft;
		_pointerY = PointerTop;
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

	//Where the pointer belongs on the picture, in the picture's own pixels. The machine has
	//to have placed it at least once - before that there is no pointer to draw.
	//
	//The long axis is counted in half pixels: the program doubles every step it takes before
	//adding it, so its 32..642 is the picture's 16..321, while the short axis is already in
	//lines. Stretching the ends of that travel to the edges of the picture instead - which is
	//what this did - put the arrow up to thirty pixels away from the place the program was
	//really pointing at, worst at the edges, so it never quite clicked what it pointed to.
	//The travel stops inside the picture, not at its border: on a 352x288 disc it stays
	//within the white panel the menu draws, rows 20..258 and columns 14..333.
	uint8_t GetAudioChannels() { return _audioChannels; }
	uint8_t GetPointerShape() { return _pointerShape; }

	bool GetPointer(double& x, double& y)
	{
		if(!_pointerShown) {
			return false;
		}
		//No clamping here: the machine has already stopped the pointer at the edge of its
		//own travel, and what draws it bounds-checks every pixel. A clamp to 1.0 belonged
		//to the old reading of these as fractions, and left behind it pinned the arrow to
		//the corner - the position was right all the way down and thrown away at the end.
		x = _pointerX / 2.0;
		y = _pointerY;
		return true;
	}

	//A menu is only worth walking when there is more than one program to reach through it,
	//and only while none has been picked - once one is running it owns the screen.
	bool HasMenu() { return _menu.IsOpen() && _programs.size() > 1 && _disc.empty(); }
	YuxingVcdMenu& GetMenu() { return _menu; }

	//The picture the menu wants up, put through the same path as a program's own request.
	//Answers with how long the disc gives it, in sectors, or nought if it wanted nothing.
	uint32_t ShowMenuStill()
	{
		uint32_t item = 0;
		if(!_menu.TakeShow(item)) {
			return 0;
		}
		RequestShow(item);
		return _playPending ? _playSectors : 0;
	}

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
		_disc = _image.ReadRange((uint64_t)program.Lba * 0x800, program.Size);
		if(_disc.empty()) {
			return false;
		}

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

	//The disc's video tracks - see CdSegmentIndex::ReadTracks. These machines keep their
	//video as segment items rather than tracks, but the discs they read still have them.
	const vector<CdTrack>& GetTracks()
	{
		if(_tracks.empty() && _image.IsOpen()) {
			_tracks = CdSegmentIndex::ReadTracks(_image, _discPath);
		}
		return _tracks;
	}
	bool IsReadComplete() { return _readComplete; }

	//The video the machine asked to show. A segment item is not a track of its own - it
	//sits inside the data track at an absolute address - so it is handed over as one,
	//which is what the sentinel track number says.
	static constexpr uint8_t SegmentTrack = 0xFF;
	bool TakePlayRequest(uint8_t& track, uint32_t& lba, uint32_t& sectors)
	{
		if(!_playPending) {
			return false;
		}
		_playPending = false;
		track = _playTrack;
		lba = _playLba;
		sectors = _playSectors;
		return true;
	}

	void EndPlayback(bool) { }

	//An ISO9660 directory record: length at 0, extent LBA at 2, data length at 10, flags at
	//25, name length at 32, name at 33. Walks one directory looking for a name, and reports
	//where what it found lives.
	bool FindIsoEntry(uint32_t dirLba, uint32_t dirLen, string prefix, bool wantDir, uint32_t& outLba, uint32_t& outLen)
	{
		vector<uint8_t> record = _image.ReadRange((uint64_t)dirLba * 0x800, dirLen);
		if(record.empty()) {
			return false;
		}

		const uint8_t* dir = record.data();
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
	//Where /SEGMENT's items begin. Every item the directory describes properly sits on a
	//150-sector boundary from the first, so the origin is read off the lowest one described
	//and then checked against the rest: if they do not all agree this is not a disc laid out
	//the way the arithmetic assumes, and nothing is claimed for it.
	void ScanSegmentOrigin()
	{
		uint32_t confirming = 0;
		_segmentOrigin = CdSegmentIndex::FindOrigin(_image, confirming);
		if(_segmentOrigin > 0) {
			MessageManager::Log("[YuXing] Segment items begin at sector " + std::to_string(_segmentOrigin) +
				" (" + std::to_string(confirming) + " confirming)");
		}
	}

	//The disc itself, for whoever needs to measure or read what is on it
	CdImageFile& GetImage() { return _image; }

	//The stretches of the segment area that hold video, for a front end to offer
	vector<CdVideoReel> GetVideoReels()
	{
		return CdSegmentIndex::FindVideoReels(_image);
	}

	bool ScanIsoPrograms()
	{
		//The primary volume descriptor at sector 16, and the root directory record inside it
		vector<uint8_t> pvd = _image.ReadRange(16 * 0x800, 0x800);
		if(pvd.empty() || memcmp(pvd.data() + 1, "CD001", 5) != 0) {
			return false;
		}

		const uint8_t* root = pvd.data() + 156;
		uint32_t rootLba = 0, rootLen = 0;
		memcpy(&rootLba, root + 2, 4);
		memcpy(&rootLen, root + 10, 4);

		//The disc's own menu, which is what the machine browses a library of programs with.
		//It is looked for whatever the programs turn out to be: whether there is a menu at
		//all is the descriptor's own answer, not something to guess from the file layout.
		uint32_t psdLba = 0, psdLen = 0;
		if(FindIsoEntry(rootLba, rootLen, "EXT", true, psdLba, psdLen)) {
			uint32_t fileLba = 0, fileLen = 0;
			if(FindIsoEntry(psdLba, psdLen, "PSD_X.VCD", false, fileLba, fileLen)) {
				_menu.Open(_image, fileLba, fileLen);
			}
		}

		uint32_t segLba = 0, segLen = 0;
		if(FindIsoEntry(rootLba, rootLen, "SEGMENT", true, segLba, segLen)) {
			CollectSegmentItems(segLba, segLen);
		}

		uint32_t dirLba = 0, dirLen = 0;
		if(FindIsoEntry(rootLba, rootLen, "PROGRAMS", true, dirLba, dirLen)) {
			CollectIsoFiles(dirLba, dirLen);
		} else {
			//Not every disc keeps its programs in a directory of their own. One carries a
			//single one at the root of the filesystem instead, and without this the disc
			//falls through to the "not a disc at all" path below, where the whole image is
			//served as though it were that one program - which is not something the machine
			//can load. The root holds the player's own directories beside it, so only the
			//files are taken, and only those named the way a program is.
			CollectIsoFiles(rootLba, rootLen);
			for(size_t i = _programs.size(); i > 0; i--) {
				const string& name = _programs[i - 1].Name;
				size_t dot = name.find_last_of('.');
				string ext = dot == string::npos ? string() : name.substr(dot);
				std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return (char)::tolower((uint8_t)c); });
				if(ext != ".bin") {
					_programs.erase(_programs.begin() + (i - 1));
				}
			}
		}

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
	//An item's own number, from a name shaped like ITEM0028.DAT. Nought if it is not one.
	static uint32_t ItemNumber(const uint8_t* name, uint8_t length)
	{
		if(length < 8 || memcmp(name, "ITEM", 4) != 0) {
			return 0;
		}
		uint32_t number = 0;
		for(uint8_t i = 4; i < 8; i++) {
			if(name[i] < '0' || name[i] > '9') {
				return 0;
			}
			number = number * 10 + (uint32_t)(name[i] - '0');
		}
		return number;
	}

	//Where the disc's segment items are, kept at their own numbers. Only where they are and
	//how far they run; what is in them is the decoder's business.
	//Does this disc space its items evenly? Answered from the records that read back
	//cleanly, and only accepted when EVERY one of them fits - two agreeing records prove
	//nothing, and a disc with one long item in it has to fall through to counting.
	void FindEvenSpacing()
	{
		_segmentEvenBase = 0;
		_segmentEvenStep = 0;

		//A directory that lists its items in order is trustworthy enough to count down, and
		//counting is what a disc whose numbering has run ahead of its names needs: there the
		//slot arithmetic lands in the middle of a long item, a stretch with no picture in it.
		//So this is only for the disc whose ordering is scrambled.
		if(_segmentNamesInOrder) {
			return;
		}

		uint32_t firstNumber = 0, lastNumber = 0;
		for(uint32_t i = 0; i < _segmentItems.size(); i++) {
			if(_segmentItems[i].Sectors == 0) {
				continue;
			}
			if(firstNumber == 0) {
				firstNumber = i + 1;
			}
			lastNumber = i + 1;
		}
		if(firstNumber == 0 || lastNumber <= firstNumber) {
			return;
		}

		uint32_t firstLba = _segmentItems[firstNumber - 1].Lba;
		uint32_t lastLba = _segmentItems[lastNumber - 1].Lba;
		uint32_t span = lastNumber - firstNumber;
		if(lastLba <= firstLba || (lastLba - firstLba) % span != 0) {
			return;
		}
		uint32_t step = (lastLba - firstLba) / span;
		if(step == 0 || firstLba < (firstNumber - 1) * step) {
			return;
		}
		uint32_t base = firstLba - (firstNumber - 1) * step;

		uint32_t fitted = 0;
		for(uint32_t i = 0; i < _segmentItems.size(); i++) {
			if(_segmentItems[i].Sectors == 0) {
				continue;
			}
			if(_segmentItems[i].Lba != base + i * step) {
				return;
			}
			fitted++;
		}

		_segmentEvenBase = base;
		_segmentEvenStep = step;
		MessageManager::Log("[YuXing] Disc spaces its items evenly: " + std::to_string(fitted) +
			" records agree on " + std::to_string(step) + " sectors from " + std::to_string(base));
	}

	void CollectSegmentItems(uint32_t dirLba, uint32_t dirLen)
	{
		_segmentItems.clear();
		_segmentOrder.clear();
		_segmentNamesInOrder = true;
		uint32_t highestSoFar = 0;
		vector<uint8_t> record = _image.ReadRange((uint64_t)dirLba * 0x800, dirLen);
		if(record.empty()) {
			return;
		}

		const uint8_t* dir = record.data();
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
			//The two entries every directory begins with name themselves and their parent
			bool isSelfOrParent = nameLen == 1 && (dir[pos + 33] == 0 || dir[pos + 33] == 1);
			if(!isDir && !isSelfOrParent && nameLen > 0 && pos + 33 + nameLen <= dirLen) {
				//Which item this is, taken from its own name rather than from where it sits in
				//the list. One disc's directory does not read cleanly all the way through -
				//some records come out with an impossible address and a size larger than the
				//disc - and dropping those silently moves every item after them up a place.
				//Asked for the twenty-eighth, the drive then answers with some other item, or
				//with one of the unreadable ones, whose address is nothing at all: a black
				//screen where a menu page should be.
				uint32_t number = ItemNumber(dir + pos + 33, nameLen);
				uint32_t lba = 0, size = 0;
				memcpy(&lba, dir + pos + 2, 4);
				memcpy(&size, dir + pos + 10, 4);
				if(lba > 0 && size >= 0x800 && (uint64_t)lba * 0x800 + size <= _image.Size()) {
					DiscSegmentItem item = { lba, size / 0x800 };
					_segmentOrder.push_back(item);
					if(number > 0) {
						if(number <= highestSoFar) {
							_segmentNamesInOrder = false;
						}
						highestSoFar = number;
						if(_segmentItems.size() < number) {
							_segmentItems.resize(number);
						}
						_segmentItems[number - 1] = item;
					}
				}
			}
			pos += recLen;
		}

		_segmentOrder.shrink_to_fit();
		FindEvenSpacing();
		uint32_t known = 0;
		for(DiscSegmentItem& item : _segmentItems) {
			known += item.Sectors > 0 ? 1 : 0;
		}
		if(known > 0) {
			MessageManager::Log("[YuXing] Disc lists " + std::to_string(known) + " of " +
				std::to_string(_segmentItems.size()) + " segment items");
		}
	}

	void CollectIsoFiles(uint32_t dirLba, uint32_t dirLen)
	{
		vector<uint8_t> record = _image.ReadRange((uint64_t)dirLba * 0x800, dirLen);
		if(record.empty()) {
			return;
		}

		const uint8_t* dir = record.data();
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

				if(program.Size > 0 && (uint64_t)program.Lba * 0x800 + program.Size <= _image.Size()) {
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

		_programs.clear();
		_programIndex = -1;
		if(!_image.Open(imagePath)) {
			return false;
		}

		MessageManager::Log("[YuXing] Disc image: " + std::to_string(_image.SectorCount()) + " sectors");

		//A disc holds programs. None is started here - the machine comes up on its own side
		//and the disc's programs are offered through the media list, which is as close to the
		//player's own menu as this can get without decoding the MPEG stills it was drawn from.
		if(ScanIsoPrograms()) {
			ScanSegmentOrigin();
			LoadProgramNames(imagePath);
			_disc.clear();
			_programIndex = -1;
			Reset();
		} else {
			//Not a disc - a single extracted program, which is the whole of the drive's view
			_disc = _image.ReadRange(0, _image.Size());
			_image.Close();
			Reset();
		}

		_discPath = path;
		_tracks.clear();
		return true;
	}

	void EjectDisc()
	{
		_menu.Close();
		_segmentItems.clear();
		_segmentOrder.clear();
		_segmentEvenBase = 0;
		_segmentEvenStep = 0;
		_segmentNamesInOrder = true;
		_segmentOrigin = 0;
		_playPending = false;
		_disc.clear();
		_image.Close();
		_programs.clear();
		_programIndex = -1;
		_discPath.clear();
		_tracks.clear();
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
					case 4:
						_status >>= 1; _shifting = true;
						//The status byte leaves one bit at a time, so the eighth shift is
						//the end of it and where a second answer has to be waiting.
						if(++_shiftCount >= 8) {
							_shiftCount = 0;
							if(_hasFollowUp) {
								_status = _followUp;
								_hasFollowUp = false;
							}
						}
						break;
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
		SV(_cmdSel); SV(_keyByteIndex); SV(_shiftIn); SV(_status); SV(_cmdIndex); SV(_audioChannels);
		SV(_pos); SV(_basePos); SV(_seekPos);
		SV(_keySend); SV(_keySendBit); SV(_keySelect);
		SV(_followUp); SV(_hasFollowUp); SV(_shiftCount);
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
