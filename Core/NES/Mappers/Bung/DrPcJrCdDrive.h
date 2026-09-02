#pragma once
#include "pch.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/StringUtilities.h"
#include "Utilities/Serializer.h"

//The CD drive the KW machines' player front end talks to, over $41AE/$41AF.
//
//The link is a byte port with a status register, and the BIOS drives it three ways:
//
//  BIT $41AF / BPL -5 / LDA $41AE      wait for bit 7, then take a byte
//  BIT $41AF / BVS -5 / STA $41AE      wait for bit 6 to clear, then send a byte
//  ... eight of either in a row                 a command packet, or its reply
//
//So bit 7 of $41AF says a byte is waiting to be read and bit 6 says the drive cannot take
//one yet. Bits 0-1 of the same register are the BIOS group and belong to the mapper, so
//only 6 and 7 are answered here. Bulk transfers (a sector, or 8KB straight into $2007) use
//the same read loop, one byte at a time.
//
//WHAT THIS DOES NOT DO YET: the command set. The eight bytes of a packet are assembled in
//the machine's own RAM and their meaning is not documented anywhere - the reference
//emulator has no CD code for these machines at all, so there is nothing to port. This class
//therefore accepts packets, records them, and never answers. That is enough to boot exactly
//as before (the player already retries forever when the drive stays silent) while making
//the machine's own requests readable through the mapper's state, which is how the command
//set is meant to be worked out. See the vault note "Dr. PC Jr. family - the four BIOS
//images and what each needs".
class DrPcJrCdDrive final : public ISerializable
{
private:
	//Flattened to 2KB user data per sector, the way every caller wants to address it
	vector<uint8_t> _image;
	bool _present = false;
	string _discPath;

	//What the machine sends, kept flat rather than split into packets - the packet length
	//was one of the first things to get wrong, so imposing one here would only hide it.
	//_open holds the start of the conversation and _recent is a ring of the last bytes, so
	//the opening handshake and the steady state can both be read. _sent counts every byte.
	static constexpr uint32_t LogSize = 32;
	uint8_t _open[LogSize] = {};
	uint8_t _openCount = 0;
	uint8_t _recent[LogSize] = {};
	uint8_t _recentPos = 0;
	uint32_t _sent = 0;

	uint8_t _reply[32] = {};
	uint8_t _replyLen = 0;
	uint8_t _paramsLeft = 0;

	//A command of $E0 and above is a sector read: $59AB hands off to $5A29, which pulls
	//block[4] lots of 2048 bytes off the port. block[1..3] address the disc.
	uint8_t _cmd = 0;
	uint8_t _params[8] = {};
	uint8_t _paramCount = 0;
	uint32_t _streamPos = 0;
	uint32_t _streamLeft = 0;

	//A transfer the drive carries out itself rather than handing over a byte at a time -
	//see the $E1 case in StartTransfer
	uint32_t _placePos = 0;
	uint32_t _placeLen = 0;

	//How many tracks the cue sheet lists, for the drive's table of contents
	uint32_t _trackCount = 0;

	//A play request the machine has made that the front end has not collected yet, and the
	//play itself. See the $D0 case in WriteByte for the packet these come out of.
	//
	//The track is which video is playing, and before anything has been asked for, which one
	//the drive is sitting on. That has to be the first one rather than nothing: the play key
	//does not name a video, it asks $A0 which one the drive is on ($6DE9) and plays that, so
	//a zero here would have it ask for a video that is not on the disc.
	uint8_t _playTrack = 1;
	uint8_t _playStart[3] = {};
	uint8_t _playEnd[3] = {};
	bool _playPending = false;
	bool _playBusy = false;
	uint16_t _playOffered = 0;

	//How many pictures the play has been running for, which is where the drive says it is
	uint32_t _playElapsed = 0;

	//Whether the play came from a key on the panel rather than a $D0 packet. A key means the
	//transport was told to run, so it carries on into the next video when this one ends; a
	//packet names a stretch of one video and stops at the end of it.
	bool _panelPlay = false;

	//The last byte handed over, and whether nothing has been written since. $595E will not
	//start a command while a byte is waiting: it reads whatever is there and echoes it
	//straight back with STA $41AE, so the drive sees its own byte arrive as if it were a
	//command. That matters now that a byte below $A0 does something.
	uint8_t _lastRead = 0;
	bool _echoPossible = false;

	//How long a request is held out before the drive gives up on anyone taking it. The
	//machine sits in its wait loop for as long as the drive says it is still playing, so
	//with nothing listening - a headless run, or any front end that does not implement this
	//- that wait would never end. Two seconds is far longer than the single frame a front
	//end needs to notice.
	static constexpr uint16_t OfferTimeout = 120;


public:
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

	bool IsMounted() { return !_image.empty(); }

	//The drive has a transfer to carry out itself. The mapper owns the memory, so it does
	//the placing; this only says what and how much.
	bool TakePlacedTransfer(const uint8_t*& data, uint32_t& len)
	{
		if(_placeLen == 0 || _placePos >= _image.size()) {
			_placeLen = 0;
			return false;
		}
		len = std::min(_placeLen, (uint32_t)(_image.size() - _placePos));
		data = _image.data() + _placePos;
		_placeLen = 0;
		return true;
	}

	//The drive itself is always fitted on these machines; only the disc comes and goes. The
	//mapper drives the port whenever the machine is a KW one, and this says so; IsMounted
	//says only whether there is a disc in the tray.
	bool IsPresent() { return _present; }
	void SetPresent(bool present) { _present = present; }
	string GetDiscPath() { return _discPath; }
	uint32_t GetSectorCount() { return (uint32_t)(_image.size() / 0x800); }

	//The sheet's INDEX 01 lines, counted. Only how many there are is wanted here - the
	//machine asks for a track count and a running time, not for where each one starts.
	uint32_t CountCueTracks(string path)
	{
		ifstream cue(path, ios::in);
		uint32_t count = 0;
		string line;
		while(cue && std::getline(cue, line)) {
			if(line.find("INDEX 01") != string::npos) {
				count++;
			}
		}
		return count;
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
		if(size == 0 || size > 800 * 1024 * 1024) {
			return false;
		}

		vector<uint8_t> raw(size);
		file.read((char*)raw.data(), size);

		//A raw dump carries the whole 2352-byte sector: 12 sync bytes, a 4-byte header, an
		//8-byte subheader on Mode 2, then the payload. Only the payload is ever addressed,
		//so flatten it and let everything downstream work in plain 2KB sectors.
		static const uint8_t Sync[12] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
		if(size >= 2352 && size % 2352 == 0 && memcmp(raw.data(), Sync, sizeof(Sync)) == 0) {
			size_t sectors = size / 2352;
			_image.resize(sectors * 0x800);
			for(size_t i = 0; i < sectors; i++) {
				memcpy(_image.data() + i * 0x800, raw.data() + i * 2352 + 24, 0x800);
			}
		} else {
			_image = std::move(raw);
		}

		_discPath = path;
		//Only now: the mount loop tries a list of candidate names across more than one folder
		//and its break leaves the outer loop running, so several loads of paths that do not
		//exist happen after the real disc is already in.
		_trackCount = ext == ".cue" ? CountCueTracks(path) : 1;

		//Deliberately NOT Reset(): a disc can go in while the machine is part way through an
		//exchange, and clearing the link state there loses reply bytes it is already waiting
		//on - it then sits in the read loop at $59E1 for ever. Swapping the disc changes what
		//the drive will serve, not what it has already been asked for.
		_streamPos = 0;
		_streamLeft = 0;
		MessageManager::Log("[Dr. PC Jr.] Disc mounted: " + FolderUtilities::GetFilename(path, true) +
			" (" + std::to_string(GetSectorCount()) + " sectors)");
		return true;
	}

	void EjectDisc()
	{
		_image.clear();
		_discPath.clear();
		Reset();
	}

	void Reset()
	{
		_openCount = 0;
		_recentPos = 0;
		_sent = 0;
		_replyLen = 0;
		_paramsLeft = 0;
		_paramCount = 0;
		_cmd = 0;
		_streamPos = 0;
		_streamLeft = 0;
		memset(_params, 0, sizeof(_params));
		memset(_open, 0, sizeof(_open));
		memset(_recent, 0, sizeof(_recent));
		memset(_reply, 0, sizeof(_reply));
	}

	//$41AF bit 7 - a byte is waiting to be read. The drive must go quiet when it has
	//nothing to say: the BIOS drains the port in a tight loop at $FDDA until it does, and a
	//port that always answers traps it there for ever.
	bool DataAvailable() { return _replyLen > 0 || _streamLeft > 0; }

	//$41AF bit 6 - the drive cannot take a byte. Always ready, so the machine gets its
	//whole packet out instead of spinning on the send loop.
	bool Busy() { return false; }

	uint8_t ReadByte()
	{
		if(_replyLen == 0) {
			if(_streamLeft > 0) {
				_streamLeft--;
				return _streamPos < _image.size() ? _image[_streamPos++] : 0x00;
			}
			return 0;
		}
		uint8_t value = _reply[0];
		memmove(_reply, _reply + 1, --_replyLen);
		_lastRead = value;
		_echoPossible = true;
		return value;
	}

	void WriteByte(uint8_t value)
	{
		//A drive left holding a transfer nobody is reading poisons the link: $595E finds a
		//byte waiting before the next command, echoes anything that is not $80 straight back
		//with STA $41AE, and that echo arrives here looking like traffic. So a queued
		//transfer is dropped when a new COMMAND arrives - see below, and note that it is the
		//command that does it, not any write at all.
		_sent++;
		bool echo = _echoPossible && value == _lastRead;
		_echoPossible = false;
		if(_openCount < LogSize) {
			_open[_openCount++] = value;
		}
		_recent[_recentPos] = value;
		_recentPos = (uint8_t)((_recentPos + 1) % LogSize);

		//The machine will not issue a command while a byte is waiting to be read: $595E
		//tests bit 7 first and treats anything there as unsolicited, echoing it back and
		//failing the command. So the drive answers only when it has been asked, and never
		//acknowledges - an earlier model that replied $80 to every byte left one sitting in
		//the port and made every command abort and retry for ever.
		if(_paramsLeft > 0) {
			if(_paramCount < sizeof(_params)) {
				_params[_paramCount++] = value;
			}
			_paramsLeft--;
			if(_paramsLeft == 0) {
				StartPlayback();
				QueueReply();
				StartTransfer();
			}
			return;
		}

		//$5977 sends the command byte, then only enters the block exchange when it is $A0 or
		//above; $59AB sends seven more bytes first when it is $C0 or above, and treats $FF
		//as "nothing to do" and returns without reading anything.
		if(value == 0xFF || value < 0xA0) {
			if(!echo) {
				TakePanelKey(value);
			}
			return;
		}

		//A command, so whatever is still queued is stale. This has to be the test - the
		//machine does write to the port in the middle of a transfer it asked for, and
		//dropping the rest of that transfer on ANY write is what left the larger machine
		//rebooting in a loop on a large disc: it asked for 56 sectors, took 576 bytes,
		//wrote one byte from an unrelated routine, and got silence for the other 114,112.
		_streamLeft = 0;
		_replyLen = 0;

		if(value >= 0xC0) {
			_cmd = value;
			_paramCount = 0;
			_paramsLeft = 7;
			return;
		}

		//A command below $C0 carries no parameters and is answered at once - but it is still
		//what was asked, and the reply depends on it, so record it the same way.
		_cmd = value;
		QueueReply();
	}

	//Where in the track the drive says it is: the start of the stretch it was asked for plus
	//however long it has been playing, in minutes, seconds and sectors - 75 to the second,
	//all in plain binary. Nothing here decodes the picture, so the only clock available is
	//how long the machine has been waiting, and at a picture a frame that is near enough the
	//clock the decoder is running on.
	void GetPlayPosition(uint8_t pos[3])
	{
		uint32_t start = ((uint32_t)_playStart[0] * 60 + _playStart[1]) * 75 + _playStart[2];
		uint32_t end = ((uint32_t)_playEnd[0] * 60 + _playEnd[1]) * 75 + _playEnd[2];
		uint32_t now = start + _playElapsed * 75 / 60;
		if(end > start && now > end) {
			//Never past the end of what was asked for
			now = end;
		}
		pos[0] = (uint8_t)(now / (60 * 75));
		pos[1] = (uint8_t)(now / 75 % 60);
		pos[2] = (uint8_t)(now % 75);
	}

	//The eight bytes the machine reads back into block[8..15] after a command.
	//
	//Byte 0 is a status the player tests two ways: bit 0 is a ready flag (LDA $802B / EOR
	//#$01 / LSR / BCS <error>), and $B4C9 wants bits 7, 6 and 0 together (AND #$C1 / CMP
	//#$C1). With bit 0 alone the player reports 碟仓打开 - the tray is open - so those upper
	//two are the tray and the disc. Byte 5 bit 7 says the drive has finished. Those are
	//wanted after every command; the rest depend on what was asked, and the player front end
	//is what pins the format down:
	//
	//  $63B0: LDA #$0A / STA $00              ten tries
	//  $63B4: LDA #$A1 / JSR $672A            ask
	//  $63B9: LDA $612A / BNE                 block[10] non-zero means the answer is good
	//  $63BE: DEC $00 / BNE $63B4             otherwise ask again
	//
	//so a zero there is what makes it give up, and it then copies block[8] and block[10..14]
	//into a six byte record of its own. And the elapsed-time arithmetic at $6C72 shows what
	//the time fields are:
	//
	//  LDA $612F / SBC $612C / BCS + / SBC #$B5      frames,  256-75
	//  LDA $612E / SBC $612B / BCS + / SBC #$C4      seconds, 256-60
	//  LDA $612D / SBC $612A                         minutes
	//
	//i.e. block[10..12] and block[13..15] are two positions in plain binary M,S,F - not BCD,
	//which those two constants are what prove.
	void QueueReply()
	{
		uint8_t pos[3];
		GetPlayPosition(pos);

		//Bits 7 and 6 are the tray and the disc, bit 0 the ready flag. Nothing set at all
		//with an empty tray: bit 0 on its own reads as 碟仓打开 and then has the machine try
		//to load what it takes to be a bad disc.
		Queue(IsMounted() ? 0xC1 : 0x00);

		//$C5 is where the machine asks how far in it is, and the answer is two positions:
		//block[10..12] is where what is playing starts, block[13..15] is where the drive is
		//now, and $6C72 subtracts the one from the other. That difference is the clock on
		//the player's screen, and $6CAD also writes it into entry zero of the three tables
		//the position marker is looked up in. With both triples zero the clock sat at 00:00
		//and the marker never left the left-hand end.
		//
		//The first triple can simply be zero: only the difference is ever used, and
		//GetPlayPosition already counts from the start of the track.
		//
		//block[9] is the minutes of that same position when the question was $A0.
		Queue(_cmd == 0xA0 ? pos[0] : 0x00);
		if(_cmd == 0xC5) {
			Queue(0x00);
			Queue(0x00);
			Queue(0x00);
			Queue(pos[0]);
			Queue(pos[1]);
			Queue(pos[2]);
			return;
		}

		//block[10] is the other half of the disc-kind field WHEN THE QUESTION WAS $A1, and the
		//player will not take an answer to that with nothing in it: $63B0 asks ten times over
		//and gives up while it stays zero, which cost ten times the traffic for every reading
		//the transport took. Bit 3 is its "video CD" bit, which is what these discs are, and
		//it cannot change the label drawn - $64C7 tests block[11] bit 0 first and that one is
		//already set.
		//
		//Only for $A1. The same byte is a time field in the answer to $C5, where a stray 8
		//turns up in the player's clock - that answer is built above and never reaches here.
		//
		//$A0 carries the position too, to the second: $6D9F asks it and walks the same tables
		//to place the marker, and the two seek keys read it as the point to move away from
		//($6E44 counts it down a second at a time, $6E8A up) before handing it back as the
		//start of a fresh $D0. All three want it counted from the start of the track, which
		//is what those tables hold.
		Queue((_cmd == 0xA1 && IsMounted()) ? 0x08 : (_cmd == 0xA0 ? pos[1] : 0x00));

		//block[10] and block[11] are the kind of disc, and the player front end reads the two
		//as one field - it tests four bits in a fixed order and draws a different label for
		//each: block[11] bit 0, block[11] bit 5, block[10] bit 3, block[10] bit 0, then an
		//error. Only the first ends in a give-up, because its branch is gated on $6131 bit 7
		//(clear) where the other three are gated on $6130 bit 7 (set).
		//
		//Answering bit 5 instead makes the player accept the disc, draw the Video CD 2.0 logo
		//and run its transport - but it is NOT safe to key that off the disc: a game disc is
		//also a multi-track video CD, and reporting it as video sends the machine into the
		//player instead of the game menu (measured - the same capture launches a game with bit
		//0 and reaches the player with bit 5). The two discs are the same kind of medium, so
		//the drive cannot be what tells them apart, and what a real one reports here is not
		//known. Bit 0 keeps every disc working; do not change it on a guess.
		Queue(0x01);

		//$A1 asks what is on the disc, and its answer is drawn straight onto the player's
		//screen. The routine at $6699 runs each of these through a binary-to-decimal split
		//($6952, repeated subtract-10 - so plain numbers, not BCD) and writes the digits out
		//at row $0E: block[12] -> columns $11,$12 (the number of tracks), block[13] ->
		//$14,$15 (minutes) and block[14] -> $17,$18 (seconds), with a literal colon at $16.
		//Real hardware shows "01 05:49" for a single-track disc of 26,033 sectors, which is
		//that many plus the 150-sector lead-in as minutes and seconds.
		//
		//Only for this command: block[13] is the drive's "finished" flag for the others, and
		//$6671 spins on bit 7 of it after an $A0.
		if(_cmd == 0xA1 && IsMounted()) {
			uint32_t seconds = (GetSectorCount() + 150) / 75;
			Queue((uint8_t)_trackCount);
			Queue((uint8_t)(seconds / 60));
			Queue((uint8_t)(seconds % 60));
			Queue(0x00);
			return;
		}

		//block[12] is the track being played, which the machine takes at its word: $6C67
		//copies it straight into the parameter of the $C5 that follows, and the seek keys
		//hand it back as the track of a fresh $D0. So it has to be numbered the way the $D0
		//that started the play numbered it.
		Queue(_cmd == 0xA0 ? _playTrack : 0x00);
		//Byte 5 bit 7 says the drive has finished what it was asked to do: $6671 issues $A0
		//and spins on LDA $6792 / AND #$80 / BEQ until it is set. It means the drive has
		//taken the command, not that a video is over - see EndPlayback for that.
		Queue(0x80);
		Queue(0x00);
		Queue(0x00);
	}

	//A key on the player's own front panel. These go out as one byte with nothing read back
	//($5977 only exchanges a block for $A0 and above), so the drive is expected to act on
	//them unasked; which video it is on then comes back through $A0 block[12] like anything
	//else, and the panel draws that.
	//
	//The codes are read out of the front end rather than guessed at. $6A51 holds one
	//rectangle per widget in the order the panel draws them - five bytes each, and the x
	//co-ordinates line the groups up exactly with what is on screen: ten number keys from
	//x=66 stepping 15, three beside the display at 123, 139 and 155, nine along the bottom
	//from x=21 stepping 24 - and $6340 holds the byte each widget sends. The one key pressed
	//in a capture, with the pointer at 31,224, arrives as widget $0F, which is the bottom
	//row's first rectangle at x=21. So the order is not in doubt.
	//
	//Only the keys whose meaning follows from that order are acted on. The rest are left
	//alone: the loader sends $1C on its way into a game, so these bytes are not the panel's
	//alone, and there is nothing to check a guess against.
	void TakePanelKey(uint8_t key)
	{
		//The ten number keys, in the order the panel draws them
		static constexpr uint8_t NumberKeys[10] = { 0x16, 0x15, 0x39, 0x09, 0x0F, 0x0E, 0x0B, 0x23, 0x24, 0x49 };
		for(int i = 0; i < 10; i++) {
			if(key == NumberKeys[i]) {
				PlayVideo((uint8_t)(i + 1));
				return;
			}
		}

		//The bottom row's first two, drawn as the two skip keys
		if(key == 0x41) {
			PlayVideo((uint8_t)(_playTrack > 1 ? _playTrack - 1 : 1));
		} else if(key == 0x3A) {
			PlayVideo((uint8_t)(_playTrack + 1));
		}
	}

	//Start a video from its beginning, the way a key that names one does. The same request
	//the $D0 packet makes, so whoever plays those plays these.
	void PlayVideo(uint8_t track)
	{
		if(!IsMounted() || _trackCount < 2) {
			return;
		}

		//The sheet counts the filesystem as track 1, so the videos are 1 to one less than
		//the number of tracks. A key for one that is not there does nothing.
		if(track < 1 || track > _trackCount - 1) {
			return;
		}

		_playTrack = track;
		_panelPlay = true;
		memset(_playStart, 0, sizeof(_playStart));
		//No end, so it runs to the end of the track
		memset(_playEnd, 0, sizeof(_playEnd));
		_playPending = true;
		_playBusy = true;
		_playOffered = 0;
		_playElapsed = 0;
	}

	//$D0 asks for a stretch of one video track to be played, and the machine then waits on
	//the finished bit above until it is over. The title scripts are what name the fields:
	//they are markup, and a play reads
	//
	//  <CVCD Operate=PartPlay Area=(00,00,00,01,14,00,01)>
	//
	//with the seven numbers going into the packet as they are written: a start position, an
	//END position, and the number of the video. The last number runs 1 to 32 across the four
	//script sets on the teaching disc here, one per video file.
	//
	//The second triple looks like a length on that disc, because every play there starts at
	//0:00:00 and the two readings coincide. A game disc settles it - it plays four short
	//stretches of one video and the pairs step forward together:
	//
	//  start 0:12:30  ->  0:22:00        each about nine and a half seconds
	//  start 0:23:30  ->  0:32:00
	//  start 0:34:30  ->  0:42:00
	//  start 0:44:30  ->  0:53:00
	//
	//which is only a segment if the second number is where to stop.
	//
	//Nothing here can decode MPEG, so the drive only records the request. Whoever collects
	//it decides what to do with it, and says when it is over; until then the machine is told
	//the drive is still playing, which is what a real one would say.
	void StartPlayback()
	{
		if(_cmd != 0xD0 || _paramCount < 7) {
			return;
		}
		memcpy(_playStart, _params, 3);
		memcpy(_playEnd, _params + 3, 3);
		_playTrack = _params[6];
		_panelPlay = false;
		_playPending = true;
		_playBusy = true;
		_playOffered = 0;
		_playElapsed = 0;
	}

public:
	//Called once a picture, to time out a request nobody is going to take
	void ClockFrame()
	{
		if(_playPending) {
			if(++_playOffered > OfferTimeout) {
				//Ended the same way a real one would, so the title carries on rather than
				//sitting in its playback loop for the rest of the run
				EndPlayback();
			}
		} else if(_playBusy) {
			//Somebody took the request, so the picture is running somewhere and the drive
			//has a position to report
			_playElapsed++;
		}
	}

	//The request the machine is waiting on, if one is outstanding. Taking it makes the
	//caller responsible for calling EndPlayback - the machine waits until it does.
	bool TakePlayRequest(uint8_t& track, uint32_t& startMsf, uint32_t& endMsf)
	{
		if(!_playPending) {
			return false;
		}
		_playPending = false;
		track = _playTrack;
		startMsf = ((uint32_t)_playStart[0] << 16) | ((uint32_t)_playStart[1] << 8) | _playStart[2];
		endMsf = ((uint32_t)_playEnd[0] << 16) | ((uint32_t)_playEnd[1] << 8) | _playEnd[2];
		return true;
	}

	//The end of a video is a byte the drive sends, not a status bit. The title watches for
	//it from inside the loop it runs while the picture belongs to the decoder:
	//
	//  $9E12: JSR $8BA3                 the keyboard, so a key can cut the video short
	//         CPX #$1C / CPX #$39       ... and two of them have their own paths
	//  $9E33: LDA #$84 / JSR $5860      ask the link for a byte
	//         BCC $9E12                 nothing waiting, go round again
	//         CMP #$9F / BNE $9E12      anything else is not the end
	//
	//so $9F, once, is what lets the title out and back to drawing its own screen.
	//"completed" says the video reached its end rather than being stopped part way. The
	//transport is not a one-shot: told to play, a real one runs on through the disc, which is
	//why the player has no way at all of leaving the decoder on its own - the only exits are
	//the right mouse button ($6283, device $0D bit 0), the keyboard's $08 ($625D) and the
	//disc being taken out ($6551). So a video that ends by itself is followed by the next
	//one, and only being stopped by hand actually stops.
	//
	//Only for a play a panel key started. A $D0 names a stretch of one video, and a title
	//that asked for one is waiting for it to be over.
	void EndPlayback(bool completed = false)
	{
		if(_playBusy) {
			Queue(0x9F);
		}
		_playPending = false;
		_playBusy = false;

		if(completed && _panelPlay) {
			PlayVideo((uint8_t)(_playTrack + 1));
		}
	}

private:

	//The parameters are block[1..7]: block[4] is how many 2KB sectors follow the reply, and
	//block[1..3] address the disc. They read as minutes/seconds/frames - the first $E0 seen
	//carries 00 02 10, which is LBA 10 once the 150-sector lead-in is taken off - so that is
	//the reading used until something contradicts it.
	void StartTransfer()
	{
		if(_cmd < 0xE0 || _paramCount < 5) {
			return;
		}
		uint32_t sectors = _params[3];
		if(sectors == 0) {
			return;
		}
		uint32_t msf = ((uint32_t)_params[0] * 60 + _params[1]) * 75 + _params[2];
		uint32_t lba = msf >= 150 ? msf - 150 : 0;

		//$E0 and $E1 ask for the same thing and differ in who moves it. $E0 is read back a
		//byte at a time through the port, which is what the smaller machine does. $E1 the
		//drive carries out itself: the machine sets bit 7 of block[12] against it, which is
		//its own "no transfer to read" flag, and never comes back for the data - so leaving
		//it in the port only poisons the link, because the machine drains anything waiting
		//there before its next command and fails that command for each byte it finds.
		if(_cmd == 0xE1) {
			_placePos = lba * 0x800;
			_placeLen = sectors * 0x800;
			return;
		}

		_streamPos = lba * 0x800;
		_streamLeft = sectors * 0x800;
	}

	void Queue(uint8_t value)
	{
		if(_replyLen < sizeof(_reply)) {
			_reply[_replyLen++] = value;
		}
	}

	void Serialize(Serializer& s) override
	{
		SVArray(_open, LogSize);
		SV(_openCount);
		SVArray(_recent, LogSize);
		SV(_recentPos);
		SV(_sent);
		SVArray(_reply, 32);
		SV(_replyLen);
		SV(_paramsLeft);
		SV(_cmd);
		SVArray(_params, 8);
		SV(_paramCount);
		SV(_streamPos);
		SV(_streamLeft);
		SV(_placePos);
		SV(_placeLen);
		SV(_trackCount);
		SV(_playTrack);
		SVArray(_playStart, 3);
		SVArray(_playEnd, 3);
		SV(_playPending);
		SV(_playBusy);
		SV(_playOffered);
		SV(_playElapsed);
		SV(_lastRead);
		SV(_echoPossible);
		SV(_panelPlay);
	}
};
