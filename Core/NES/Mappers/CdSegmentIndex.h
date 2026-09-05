#pragma once
#include "pch.h"
#include "NES/Mappers/CdImageFile.h"

//Where the video on one of these discs actually is.
//
//A VCD 2.0 disc keeps its video in two places. Tracks of their own hold whole programmes,
//and /SEGMENT holds "segment play items" - a fixed 150 sectors each, about two seconds -
//which is where these machines keep nearly everything: on one disc here, 288MB of video in
//1001 items against a single 62MB track.
//
//The directory is not what says where an item is. Three quarters of the records on some of
//these discs are placeholders naming sector zero, so an item is addressed the way the
//machine addresses it - the Nth begins at origin + (N-1) * 150 - and the directory is read
//only to work out where the first one is.
//
//Listing items one at a time gives a menu of a thousand entries, so what comes out of here
//is the stretches instead: consecutive items carrying video, joined into one. The discs
//here come to between three and twenty-nine of those, of one to fifteen minutes.

struct CdVideoReel
{
	uint32_t Lba;
	uint32_t Sectors;
	//How much of that is really stream. An item is given its 150 sectors whatever it holds,
	//and on some discs two fifths of that is blank padding, so a reel plays for well under
	//the stretch of disc it came off.
	uint32_t ContentSectors;
	//The disc's own numbering, which is also what the machine names in a play command
	uint32_t FirstItem;
	uint32_t ItemCount;
};

class CdSegmentIndex
{
public:
	//The fixed allocation one item gets, whatever it holds
	static constexpr uint32_t SegmentStride = 150;

private:
	static constexpr uint32_t SectorSize = 0x800;

	//How much of an item is read to say what it carries. An item holding video holds it in
	//every sector of its allocation, so the front of one settles it: against a full read of
	//all hundred and fifty of every item on every disc here, four sectors already give the
	//same answer, and this leaves margin.
	static constexpr uint32_t ClassifyDepth = 8;

	enum class ItemContent
	{
		Empty,
		Still,
		Audio,
		Video
	};

	//One directory of an ISO9660 filesystem: a record is its length at 0, the extent LBA at
	//2, the data length at 10, flags at 25 (bit 1 = directory), the name length at 32 and
	//the name at 33. Records never straddle a sector; a zero length is the padding to the
	//end of one.
	static bool FindDirectory(CdImageFile& image, uint32_t dirLba, uint32_t dirLen, const char* wanted, uint32_t& outLba, uint32_t& outLen)
	{
		vector<uint8_t> record = image.ReadRange((uint64_t)dirLba * SectorSize, dirLen);
		if(record.empty()) {
			return false;
		}

		size_t wantedLen = strlen(wanted);
		const uint8_t* dir = record.data();
		for(uint32_t pos = 0; pos < dirLen; ) {
			uint8_t len = dir[pos];
			if(len == 0) {
				pos = (pos / SectorSize + 1) * SectorSize;
				continue;
			}
			if(pos + len > dirLen || len < 34) {
				break;
			}

			uint8_t nameLen = dir[pos + 32];
			if(nameLen >= wantedLen && (dir[pos + 25] & 0x02) != 0 &&
				memcmp(dir + pos + 33, wanted, wantedLen) == 0) {
				memcpy(&outLba, dir + pos + 2, 4);
				memcpy(&outLen, dir + pos + 10, 4);
				return true;
			}
			pos += len;
		}
		return false;
	}

	static bool FindRoot(CdImageFile& image, uint32_t& rootLba, uint32_t& rootLen)
	{
		//The primary volume descriptor at sector 16, with the root directory record at +156
		vector<uint8_t> pvd = image.ReadRange(16 * SectorSize, SectorSize);
		if(pvd.empty() || memcmp(pvd.data() + 1, "CD001", 5) != 0) {
			return false;
		}

		memcpy(&rootLba, pvd.data() + 156 + 2, 4);
		memcpy(&rootLen, pvd.data() + 156 + 10, 4);
		return true;
	}

	//What an item carries, from the stream ids in the front of it. 0xE0 is the video a lesson
	//is made of, 0xE1 the single-frame pages its menus are drawn from, and 0xC0 the narration
	//that plays over one of those.
	static ItemContent Classify(CdImageFile& image, uint32_t lba)
	{
		bool still = false;
		bool audio = false;
		for(uint32_t i = 0; i < ClassifyDepth; i++) {
			vector<uint8_t> sector = image.ReadRange((uint64_t)(lba + i) * SectorSize, SectorSize);
			//A pack header, or nothing worth reading
			if(sector.size() < 16 || sector[0] != 0x00 || sector[1] != 0x00 || sector[2] != 0x01 || sector[3] != 0xBA) {
				continue;
			}

			//Past the twelve-byte pack header, the packets themselves
			uint32_t pos = 12;
			while(pos + 6 <= SectorSize && sector[pos] == 0x00 && sector[pos + 1] == 0x00 && sector[pos + 2] == 0x01) {
				uint8_t id = sector[pos + 3];
				if(id == 0xB9) {
					break;
				}
				if(id == 0xE0) {
					return ItemContent::Video;
				}
				still |= id == 0xE1;
				audio |= id == 0xC0;
				pos += 6 + (((uint32_t)sector[pos + 4] << 8) | sector[pos + 5]);
			}
		}
		return still ? ItemContent::Still : audio ? ItemContent::Audio : ItemContent::Empty;
	}

	//How many of an item's sectors carry anything. The used ones sit at the front of the
	//allocation and the padding behind them - checked across nine hundred items on these
	//discs without exception - so the boundary is found by halving rather than by reading all
	//of it. A sector whose subheader claims neither video, audio nor data is the padding.
	static uint32_t ItemFill(CdImageFile& image, uint32_t lba)
	{
		uint32_t low = 0;
		uint32_t high = SegmentStride;
		while(low < high) {
			uint32_t middle = (low + high) / 2;
			if(image.SubmodeAt(lba + middle) & 0x0E) {
				low = middle + 1;
			} else {
				high = middle;
			}
		}
		return low;
	}

public:
	//Where item 1 begins. Every record naming a real sector has to agree on it: the layout is
	//arithmetic, so one record that does not fit it means this is not that layout, and a
	//guess would read a stream out of the middle of some other item.
	static uint32_t FindOrigin(CdImageFile& image, uint32_t& confirming)
	{
		confirming = 0;

		uint32_t rootLba = 0, rootLen = 0;
		uint32_t segLba = 0, segLen = 0;
		if(!FindRoot(image, rootLba, rootLen) || !FindDirectory(image, rootLba, rootLen, "SEGMENT", segLba, segLen)) {
			return 0;
		}

		vector<uint8_t> record = image.ReadRange((uint64_t)segLba * SectorSize, segLen);
		if(record.empty()) {
			return 0;
		}

		uint32_t imageSectors = (uint32_t)(image.Size() / SectorSize);
		uint32_t origin = 0;
		uint32_t agreed = 0, seen = 0;

		const uint8_t* dir = record.data();
		for(uint32_t pos = 0; pos < segLen; ) {
			uint8_t len = dir[pos];
			if(len == 0) {
				pos = (pos / SectorSize + 1) * SectorSize;
				continue;
			}
			if(pos + len > segLen || len < 34) {
				break;
			}

			uint8_t nameLen = dir[pos + 32];
			const char* name = (const char*)(dir + pos + 33);
			//ITEMnnnn.DAT - the number is what the machine asks for
			if(nameLen >= 12 && memcmp(name, "ITEM", 4) == 0) {
				uint32_t item = 0;
				bool digits = true;
				for(int i = 4; i < 8; i++) {
					if(name[i] < '0' || name[i] > '9') { digits = false; break; }
					item = item * 10 + (uint32_t)(name[i] - '0');
				}

				uint32_t lba = 0;
				memcpy(&lba, dir + pos + 2, 4);
				uint32_t before = digits && item > 0 ? (item - 1) * SegmentStride : 0;
				if(digits && item > 0 && lba > before && lba < imageSectors) {
					uint32_t candidate = lba - before;
					seen++;
					if(origin == 0) {
						origin = candidate;
						agreed = 1;
					} else if(candidate == origin) {
						agreed++;
					}
				}
			}
			pos += len;
		}

		if(seen == 0 || agreed != seen) {
			return 0;
		}

		confirming = seen;
		return origin;
	}

	//The items run up to the first video track, or to the end of the image on a disc that has
	//none - most of these carry their video as items and no tracks at all.
	static uint32_t FindEnd(CdImageFile& image, uint32_t origin)
	{
		uint32_t end = (uint32_t)(image.Size() / SectorSize);

		uint32_t rootLba = 0, rootLen = 0;
		uint32_t avLba = 0, avLen = 0;
		if(!FindRoot(image, rootLba, rootLen) || !FindDirectory(image, rootLba, rootLen, "MPEGAV", avLba, avLen)) {
			return end;
		}

		vector<uint8_t> record = image.ReadRange((uint64_t)avLba * SectorSize, avLen);
		if(record.empty()) {
			return end;
		}

		const uint8_t* dir = record.data();
		for(uint32_t pos = 0; pos < avLen; ) {
			uint8_t len = dir[pos];
			if(len == 0) {
				pos = (pos / SectorSize + 1) * SectorSize;
				continue;
			}
			if(pos + len > avLen || len < 34) {
				break;
			}

			uint8_t nameLen = dir[pos + 32];
			if(nameLen >= 5 && memcmp(dir + pos + 33, "AVSEQ", 5) == 0) {
				uint32_t lba = 0;
				memcpy(&lba, dir + pos + 2, 4);
				if(lba > origin && lba < end) {
					end = lba;
				}
			}
			pos += len;
		}
		return end;
	}

	//Consecutive items carrying video, joined into one entry each
	static vector<CdVideoReel> FindVideoReels(CdImageFile& image)
	{
		vector<CdVideoReel> reels;
		if(!image.IsOpen()) {
			return reels;
		}

		uint32_t confirming = 0;
		uint32_t origin = FindOrigin(image, confirming);
		if(origin == 0) {
			return reels;
		}

		uint32_t end = FindEnd(image, origin);
		uint32_t slots = end > origin ? (end - origin) / SegmentStride : 0;
		uint32_t runStart = 0;
		uint32_t runLength = 0;
		uint32_t runContent = 0;

		//One past the last, so a run reaching the end of the area is closed too
		for(uint32_t i = 0; i <= slots; i++) {
			if(i < slots && Classify(image, origin + i * SegmentStride) == ItemContent::Video) {
				if(runLength == 0) {
					runStart = i;
				}
				runLength++;
				runContent += ItemFill(image, origin + i * SegmentStride);
				continue;
			}

			if(runLength > 0) {
				reels.push_back({
					origin + runStart * SegmentStride,
					runLength * SegmentStride,
					runContent,
					runStart + 1,
					runLength
				});
				runLength = 0;
				runContent = 0;
			}
		}
		return reels;
	}
};
