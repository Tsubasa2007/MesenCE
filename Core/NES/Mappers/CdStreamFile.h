#pragma once
#include "pch.h"
#include "NES/Mappers/CdImageFile.h"
#include "NES/Mappers/CdSegmentIndex.h"

//Writing one of a disc's items out as an ordinary MPEG-1 file, for something other than this
//to play.
//
//Which sectors that is, and in what order, is not decided here - CdSegmentIndex::ForEachStreamSector
//says, and the decoder inside this emulator is fed by the same walk. The two used to have an
//account each, and a correction to one of them was a correction to only one of them.
//
//Nothing here reads the mounted image. It opens the file for itself, so it can be called from
//whatever thread is doing the writing.
class CdStreamFile
{
public:
	//`from` and `to` bound the part of the item wanted, counted in the sectors that carry the
	//stream, `to` of nought meaning the rest of it. `retime` is for a reel - several of the
	//disc's items written out one after another - and is described at Retime below.
	static uint32_t Write(string binPath, uint32_t lba, uint32_t sectors, uint32_t from, uint32_t to, bool retime, string outPath)
	{
		CdImageFile image;
		if(!image.Open(binPath)) {
			return 0;
		}

		//The project's own stream, not std::ofstream: these files are named after the disc
		//they came off, and a plain narrow stream cannot create what that spells
		ofstream out(outPath, std::ios::out | std::ios::binary);
		if(!out) {
			return 0;
		}

		int64_t read = 0;
		int64_t written = -1;
		uint32_t taken = CdSegmentIndex::ForEachStreamSector(image, lba, sectors, from, to,
			[&](vector<uint8_t>& payload) {
				if(retime) {
					Retime(payload, read, written);
				}
				out.write((const char*)payload.data(), (std::streamsize)payload.size());
			});

		out.close();
		return taken;
	}

private:
	//33 bits of clock at 90kHz, spread across five bytes around two marker bits
	static constexpr int64_t ClockRate = 90000;
	static constexpr int64_t ClockStep = ClockRate / 75;
	static constexpr int64_t MaxClockStep = ClockRate;
	static constexpr int64_t ClockMask = 0x1FFFFFFFFLL;

	//Move one pack's clocks onto the end of the pack before it.
	//
	//A reel is several of the disc's items end to end, and on some discs each of those is a
	//stream of its own that starts its clocks again from nothing. A player reading that finds
	//the clock at the back of the file lower than the one at the front and has nothing to size
	//its progress bar with.
	//
	//So the step between one pack and the next is what is kept, not the readings themselves. A
	//step the disc could have meant is used as it stands - that leaves a reel whose items
	//already run on written exactly as it was found. Anything else is a join, and one CD frame
	//is left across it. The last packs of an item on these discs read three hours and more, so
	//a join is not only a step backwards.
	//
	//Both clocks have to move together. The pack header carries the one the drive is fed by,
	//and the packets inside it carry the ones the picture and sound are shown on - and a player
	//asked how long a file is may answer from either. Moving only the first left one player
	//still calling a twelve minute reel three seconds long.
	static void Retime(vector<uint8_t>& payload, int64_t& read, int64_t& written)
	{
		//The pack header is 00 00 01 BA and then the clock
		if(payload.size() < 12 || payload[0] || payload[1] || payload[2] != 1 || payload[3] != 0xBA) {
			return;
		}

		int64_t scr = ReadClock(payload, 4);
		int64_t step = scr - read;
		int64_t now = written < 0 ? scr : written + (step > 0 && step <= MaxClockStep ? step : ClockStep);
		read = scr;
		written = now & ClockMask;
		WriteClock(payload, 4, written);

		//What the pack header moved by, which is what everything inside it moves by too
		int64_t shift = written - scr;
		if(shift == 0) {
			return;
		}

		size_t limit = payload.size();
		size_t at = 12;
		while(at + 6 <= limit && payload[at] == 0x00 && payload[at + 1] == 0x00 && payload[at + 2] == 0x01) {
			uint8_t id = payload[at + 3];
			if(id == 0xB9) {
				break;
			}

			size_t length = ((size_t)payload[at + 4] << 8) | payload[at + 5];
			if(id == 0xE0 || id == 0xE1 || id == 0xC0) {
				size_t end = at + 6 + length;
				ShiftPacketClocks(payload, at + 6, end < limit ? end : limit, shift, written);
			}
			at += 6 + length;
		}
	}

	//The head of one packet: any number of stuffing bytes, then the buffer size if it is given,
	//then either nothing, a presentation time, or a presentation and a decode time.
	static void ShiftPacketClocks(vector<uint8_t>& payload, size_t from, size_t limit, int64_t shift, int64_t floor)
	{
		size_t at = from;
		while(at < limit && payload[at] == 0xFF) {
			at++;
		}
		if(at + 1 < limit && (payload[at] & 0xC0) == 0x40) {
			at += 2;
		}
		if(at >= limit) {
			return;
		}

		//0x20 is a presentation time on its own, 0x30 that and a decode time behind it
		uint8_t marker = payload[at] & 0xF0;
		int count = marker == 0x20 ? 1 : marker == 0x30 ? 2 : 0;
		for(int i = 0; i < count; i++) {
			size_t on = at + (size_t)i * 5;
			if(on + 5 > limit) {
				return;
			}
			//A pack whose own reading was thrown away leaves a shift these cannot follow, so
			//they are put where the pack now is rather than somewhere before the start
			int64_t moved = ReadClock(payload, on) + shift;
			WriteClock(payload, on, (moved < 0 ? floor : moved) & ClockMask);
		}
	}

	//The clock in the pack header and in a packet's times alike - the four bits above it differ
	//and are left alone
	static int64_t ReadClock(const vector<uint8_t>& payload, size_t at)
	{
		int64_t high = (payload[at] >> 1) & 0x07;
		int64_t mid = (((payload[at + 1] << 8) | payload[at + 2]) >> 1) & 0x7FFF;
		int64_t low = (((payload[at + 3] << 8) | payload[at + 4]) >> 1) & 0x7FFF;
		return (high << 30) | (mid << 15) | low;
	}

	static void WriteClock(vector<uint8_t>& payload, size_t at, int64_t value)
	{
		int64_t high = (value >> 30) & 0x07;
		int64_t mid = (value >> 15) & 0x7FFF;
		int64_t low = value & 0x7FFF;
		payload[at] = (uint8_t)((payload[at] & 0xF0) | (high << 1) | 1);
		payload[at + 1] = (uint8_t)(mid >> 7);
		payload[at + 2] = (uint8_t)(((mid & 0x7F) << 1) | 1);
		payload[at + 3] = (uint8_t)(low >> 7);
		payload[at + 4] = (uint8_t)(((low & 0x7F) << 1) | 1);
	}
};
