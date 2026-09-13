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

//One of the disc's video tracks.
struct CdTrack
{
	//The disc's own numbering. The filesystem is track 1, so the number a title names in a
	//play is one less than this.
	uint32_t Number;
	uint32_t Lba;
	uint32_t Sectors;
};

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

	//How fast a disc runs. Everything counted in sectors turns into time through this.
	static constexpr double SectorsPerSecond = 75.0;

public:
	//How long an item is, both ways of asking, measured in one place so nothing that plays or
	//offers a video can disagree about it.
	//
	//  discSeconds   how long it plays for. A sector is a seventy-fifth of a second, the same
	//                arithmetic the drive does - but only the sectors that carry something
	//                count. See ContentSectors.
	//  streamSeconds the span between the first and last system clock the stream stamps into
	//                its pack headers. Near zero for an item that is a single still picture,
	//                which is what says there is nothing to play.
	//
	//They are not interchangeable, and the second cannot stand in for the first. On these
	//discs an item of 6375 sectors - 85 seconds, and it plays for about that - stamps clocks
	//spanning only 4.3 seconds.
	static void MeasureItem(CdImageFile& image, uint32_t lba, uint32_t sectors,
		double& discSeconds, double& streamSeconds)
	{
		discSeconds = ContentSectors(image, lba, sectors) / SectorsPerSecond;
		streamSeconds = 0;
		if(sectors == 0) {
			return;
		}
		double first = ClockNear(image, lba, false);
		double last = ClockNear(image, lba + sectors - 1, true);
		if(first >= 0 && last >= 0 && last > first) {
			streamSeconds = last - first;
		}
	}

public:
	//Where the item a track stands for really lies on the disc, which is not where the track
	//does.
	//
	//A track is not a container here. Reading one straight through gives two streams and a
	//hole between them: a stretch that carries on the clock of the item before and closes with
	//an end code, then a run of empty sectors, then a system header and a clock starting again
	//from nothing - the item this track is named for. That item runs on past the end of the
	//track, through the pause before the next one, which is full rather than empty, and some
	//way into that next track, closing there just before the empty run that separates it from
	//the item after. So the empty runs are what divide one item from the next, and the track
	//boundaries fall in the middle of them.
	//
	//This is why every item on such a disc played short by whatever lay in front of its own
	//track's gap. The decoder was right to drop that part - the library will not start a
	//stream without a system header, and there is none until the item proper begins - but the
	//end was being cut at the end of the track, which on the shortest of them left less than
	//half the item playable.
	//
	//A track with no empty run in it is its own item: that is what a disc looks like where
	//each item was laid down on its own, and what a flat image looks like whatever is on it.
	static bool FindItem(CdImageFile& image, uint32_t trackLba, uint32_t trackSectors, uint32_t& lba, uint32_t& sectors)
	{
		if(!image.IsRaw()) {
			//No subheaders to divide it by, and nothing but the track to go on
			lba = trackLba;
			sectors = trackSectors;
			return trackSectors > 0;
		}

		lba = ItemStart(image, trackLba, trackSectors);

		//From there the item runs on until something says another one has begun. An empty run
		//is not that something: on one disc here every item is broken by a hundred and
		//fifty-two empty sectors - the space between two files - and carries straight on
		//afterwards, with the pack clock stepping across the hole as if the sectors had been
		//played. Reading the far side as a different item cost every video on that disc the
		//second and a third of it that lay beyond the hole.
		//
		//So a hole is crossed unless the far side is plainly a new item: it opens with a
		//system header, or its clock stands behind the clock this side of the hole. Either
		//says the stream starts again there.
		uint32_t at = lba;
		uint32_t end = lba;
		while(true) {
			//The item's own header can stand over more than one sector, so a header only ends
			//it once an ordinary sector has been seen
			bool started = false;
			while(HasContent(image, at)) {
				if(HasStreamHeader(image, at)) {
					if(started) {
						break;
					}
				} else {
					started = true;
				}
				at++;
			}
			end = at;
			if(HasContent(image, end)) {
				//Stopped on the next item's header rather than on a hole
				break;
			}

			if(end <= lba) {
				//Nothing carried at all
				break;
			}

			double before = PackClock(image.ReadRawPayload(end - 1, PayloadSize));
			uint32_t after = end;
			while(after < end + HoleSearch && !HasContent(image, after)) {
				after++;
			}
			if(!HasContent(image, after) || HasStreamHeader(image, after)) {
				break;
			}

			double behind = PackClock(image.ReadRawPayload(after, PayloadSize));
			if(before < 0 || behind < 0 || behind < before) {
				break;
			}
			at = after;
		}

		sectors = end - lba;
		return sectors > 0;
	}

	//Where the item a track holds begins. Its system header says so outright; failing that,
	//the empty run dividing it from the item before does; and a track with neither begins
	//wherever it first carries anything, which is what a disc looks like where each item was
	//laid down on its own.
	static uint32_t ItemStart(CdImageFile& image, uint32_t trackLba, uint32_t trackSectors)
	{
		uint32_t limit = trackLba + trackSectors;
		uint32_t look = trackSectors < HeaderSearch ? trackSectors : HeaderSearch;
		for(uint32_t i = 0; i < look; i++) {
			if(HasStreamHeader(image, trackLba + i)) {
				return trackLba + i;
			}
		}

		uint32_t at = trackLba;
		//Whatever the track opens with is the tail of the item before it
		while(at < limit && HasContent(image, at)) {
			at++;
		}
		//and behind that is the empty run the next item begins after
		while(at < limit && !HasContent(image, at)) {
			at++;
		}
		if(at < limit) {
			return at;
		}

		//Nothing divided the track, so it stands for its own item
		for(uint32_t i = 0; i < trackSectors; i++) {
			if(HasContent(image, trackLba + i)) {
				return trackLba + i;
			}
		}
		return trackLba;
	}

	//Whether a sector opens a stream. A system header follows the pack header a sector begins
	//with, and the authoring on these discs writes one only where an item starts - which is
	//also the only place the decoding library will begin from.
	static bool HasStreamHeader(CdImageFile& image, uint32_t lba)
	{
		if(!HasContent(image, lba)) {
			return false;
		}

		vector<uint8_t> head = image.ReadRawPayload(lba, HeadSize);
		for(size_t i = 0; i + 4 <= head.size(); i++) {
			if(head[i] == 0 && head[i + 1] == 0 && head[i + 2] == 1 && head[i + 3] == 0xBB) {
				return true;
			}
		}
		return false;
	}


private:
	static constexpr uint32_t PayloadSize = 2324;

	//How far into a track to look for the header its item opens with. The furthest seen on
	//these discs is a little over two thousand sectors in.
	static constexpr uint32_t HeaderSearch = 8192;

	//How far past an empty run to look for the stream picking up again. The runs here are a
	//couple of hundred sectors; the pause between two tracks is a hundred and fifty.
	static constexpr uint32_t HoleSearch = 4096;

	//How much of a sector to read when all that is wanted is the pack header and whatever
	//follows it
	static constexpr uint32_t HeadSize = 32;

public:
	//How much of a stretch of disc actually carries anything.
	//
	//A sector says in its own subheader whether it holds video, audio or data, and one that
	//claims none of the three is padding: on these discs it reads as zeros throughout. Whoever
	//plays a stretch skips those, so counting them towards its length makes every video finish
	//before its own end. A track here opens with a run of them and some carry a second run
	//part way in - between 28 and 225 sectors each on the tracks measured, a third of a second
	//to three seconds.
	//
	//A flat image has no subheaders, so everything on one counts.
	static uint32_t ContentSectors(CdImageFile& image, uint32_t lba, uint32_t sectors)
	{
		uint32_t count = 0;
		for(uint32_t i = 0; i < sectors; i++) {
			if(HasContent(image, lba + i)) {
				count++;
			}
		}
		return count;
	}

	//Every sector of an item's stream, in order, handed over as the payload the sector says
	//it holds - Form 2 carries 2324 bytes, Form 1 the usual 2048 - with the sectors that carry
	//nothing left out.
	//
	//`from` and `to` are counted in these sectors and not in the disc's. The two are not the
	//same thing: an item can be broken by a run of empty sectors and carry straight on
	//afterwards (see FindItem), and the positions a title names run across such a run as
	//though it were not there. `to` of nought means to the end of the item. What comes back is
	//how many sectors were handed over.
	//
	//This is the one account of what an item's stream is made of. Decoding one and writing one
	//out for something else to play both go through here, because when they each had their
	//own account of it they each had to be corrected separately, and were.
	template<typename T>
	static uint32_t ForEachStreamSector(CdImageFile& image, uint32_t lba, uint32_t sectors, uint32_t from, uint32_t to, T handle)
	{
		uint32_t carried = 0;
		uint32_t taken = 0;
		bool started = false;
		for(uint32_t i = 0; i < sectors && (to == 0 || carried < to); i++) {
			uint8_t submode = image.SubmodeAt(lba + i);
			if((submode & 0x0E) == 0) {
				continue;
			}

			vector<uint8_t> payload = image.ReadRawPayload(lba + i, (submode & 0x20) ? PayloadSize : SectorSize);
			if(payload.empty()) {
				break;
			}

			if(!started) {
				//The stream begins at its first pack, and whatever stands in front of that is
				//of no use to anything reading it
				if(PackClock(payload) < 0) {
					continue;
				}
				started = true;
			}

			if(carried++ < from) {
				continue;
			}

			handle(payload);
			taken++;
		}
		return taken;
	}

	//Whether a sector carries any of the stream at all. Bits 1 to 3 of its subheader are video,
	//audio and data, and a sector claiming none of the three is padding - the same test a
	//decoder gathers by.
	static bool HasContent(CdImageFile& image, uint32_t lba)
	{
		return (image.SubmodeAt(lba) & 0x0E) != 0;
	}

	//How many allocations an item's stream reaches into.
	//
	//An item gets one fixed allocation, and one too long for it takes the allocations that
	//follow - along with the item numbers that belong to them, which is why a disc's numbering
	//runs ahead of the files it names. So the numbering says where an item starts and nothing
	//in it says how far the item goes; the stream says, by closing with an end code, and the
	//allocation that code falls in is the last one the item owns.
	//
	//Counted in whole allocations rather than in sectors, because an allocation is also what a
	//page is given to stand on the screen for. An item's stream stopping a third of the way
	//into its own allocation does not make the page shorter.
	//
	//Nought when no end code turns up before the item's own sectors run out: a stream not laid
	//out this way at all, where whatever the caller already had is the better answer.
	//
	//An item's sectors are one unbroken run - every item measured here carries content from
	//its first sector to its end code with no gap anywhere in it - so the first empty sector
	//after that run has started is the end of what this item can possibly hold. Reading past
	//it would find the next item's end code and hand back its allocations as though they
	//belonged to this one. One item on one disc carries no end code at all, and that is what
	//would have happened to it.
	static uint32_t StreamAllocations(CdImageFile& image, uint32_t lba, uint32_t maxAllocations)
	{
		bool started = false;
		for(uint32_t i = 0; i < maxAllocations * SegmentStride; i++) {
			uint8_t submode = image.SubmodeAt(lba + i);
			if((submode & 0x0E) == 0) {
				if(started) {
					break;
				}
				continue;
			}
			started = true;

			vector<uint8_t> payload = image.ReadRawPayload(lba + i, (submode & 0x20) ? PayloadSize : SectorSize);
			if(payload.empty()) {
				break;
			}

			for(size_t at = 3; at < payload.size(); at++) {
				if(payload[at] == 0xB9 && payload[at - 1] == 0x01 && payload[at - 2] == 0 && payload[at - 3] == 0) {
					return i / SegmentStride + 1;
				}
			}
		}
		return 0;
	}

private:

private:
	static constexpr uint32_t SectorSize = 0x800;

	//How much of an item is read to say what it carries. An item holding video holds it in
	//every sector of its allocation, so the front of one settles it: against a full read of
	//all hundred and fifty of every item on every disc here, four sectors already give the
	//same answer, and this leaves margin.
	static constexpr uint32_t ClassifyDepth = 8;

	//00 00 01 BA, then the system clock the stream is timed by in the next five bytes
	static double PackClock(const vector<uint8_t>& payload)
	{
		if(payload.size() < 9 || payload[0] || payload[1] || payload[2] != 1 || payload[3] != 0xBA) {
			return -1;
		}
		int64_t high = (payload[4] >> 1) & 0x07;
		int64_t mid = (((payload[5] << 8) | payload[6]) >> 1) & 0x7FFF;
		int64_t low = (((payload[7] << 8) | payload[8]) >> 1) & 0x7FFF;
		return (double)((high << 30) | (mid << 15) | low) / 90000.0;
	}

	//The clock nearest one end of an item. An item is padded out to a fixed allocation, so
	//its first and last sectors are often not stream at all - look a little way past them.
public:
	//The pack clock at a sector, or at the nearest one either side of it, so how long an item
	//runs can be had without walking all of it
	static double ClockNear(CdImageFile& image, uint32_t lba, bool backwards)
	{
		for(uint32_t step = 0; step < 16; step++) {
			double clock = PackClock(image.ReadRawPayload(backwards ? lba - step : lba + step, PayloadSize));
			if(clock >= 0) {
				return clock;
			}
		}
		return -1;
	}

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

	//A position on a disc counts from the lead-in, which the data does not include
	static constexpr uint32_t LeadIn = 150;

	//The pause a track is given in front of its data, which belongs to the track before it
	static constexpr uint32_t TrackPause = 150;

	//A number written the way a disc writes one: two decimal digits to the byte. A byte that
	//is not a valid pair is taken as it is, so a disc numbering plainly is still read right.
	static uint32_t TrackNumber(uint8_t value)
	{
		return (value >> 4) <= 9 && (value & 0x0F) <= 9 ? FromBcd(value) : value;
	}

	static uint32_t ReadDigits(const char*& at)
	{
		uint32_t value = 0;
		while(*at >= '0' && *at <= '9') {
			value = value * 10 + (uint32_t)(*at++ - '0');
		}
		return value;
	}

	static uint32_t FromBcd(uint8_t value) { return (uint32_t)((value >> 4) * 10 + (value & 0x0F)); }

	//The same walk as FindDirectory, for a file rather than a directory beside it
	static bool FindFile(CdImageFile& image, uint32_t dirLba, uint32_t dirLen, const char* wanted, uint32_t& outLba, uint32_t& outLen)
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
			if(nameLen >= wantedLen && (dir[pos + 25] & 0x02) == 0 &&
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

	//The tracks a sheet beside the image names. A sheet counts from the first sector of the
	//image, so its readings need no lead-in taken off them.
	static vector<CdTrack> ReadSheetTracks(string discPath)
	{
		vector<CdTrack> tracks;
		string ext = discPath.size() >= 4 ? discPath.substr(discPath.size() - 4) : string();
		std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return (char)::tolower((uint8_t)c); });
		if(ext != ".cue") {
			return tracks;
		}

		ifstream sheet(discPath, ios::in);
		string line;
		uint32_t number = 0;
		while(sheet && std::getline(sheet, line)) {
			size_t at = line.find("TRACK ");
			if(at != string::npos) {
				const char* text = line.c_str() + at + 6;
				number = ReadDigits(text);
				continue;
			}

			at = line.find("INDEX 01 ");
			if(at == string::npos || number == 0) {
				continue;
			}

			//Minutes, seconds and frames, seventy-five frames to the second
			const char* text = line.c_str() + at + 9;
			uint32_t minutes = ReadDigits(text);
			if(*text++ != ':') { continue; }
			uint32_t seconds = ReadDigits(text);
			if(*text++ != ':') { continue; }
			uint32_t frames = ReadDigits(text);

			tracks.push_back({ number, (minutes * 60 + seconds) * 75 + frames, 0 });
			number = 0;
		}
		return tracks;
	}

	//The tracks the disc's own table names.
	//
	///VCD/ENTRIES.VCD lists every track the disc plays as a number and a position, counted
	//from the very start of the disc - so its reading is a lead-in ahead of the sector the
	//data is at. Both are written as decimal digits to the byte: the discs here number their
	//tracks 02 to 09 and then $10, $11, $12 for 10, 11 and 12, so read as plain numbers
	//nothing answers to track 10 and some numbers collide with other tracks entirely.
	//
	//Entries after the first of a track are points inside it, and only the earliest of them
	//is where the track begins.
	static vector<CdTrack> ReadDiscTableTracks(CdImageFile& image)
	{
		vector<CdTrack> tracks;

		uint32_t rootLba = 0, rootLen = 0;
		uint32_t dirLba = 0, dirLen = 0;
		if(!FindRoot(image, rootLba, rootLen) || !FindDirectory(image, rootLba, rootLen, "VCD", dirLba, dirLen)) {
			return tracks;
		}

		uint32_t fileLba = 0, fileLen = 0;
		if(!FindFile(image, dirLba, dirLen, "ENTRIES.VCD", fileLba, fileLen)) {
			return tracks;
		}

		vector<uint8_t> entries = image.ReadRange((uint64_t)fileLba * SectorSize, SectorSize);
		if(entries.size() < 12 || memcmp(entries.data(), "ENTRYVCD", 8) != 0) {
			return tracks;
		}

		uint32_t count = ((uint32_t)entries[10] << 8) | entries[11];
		for(uint32_t i = 0; i < count; i++) {
			uint32_t at = 12 + i * 4;
			if(at + 4 > entries.size()) {
				break;
			}

			uint32_t number = TrackNumber(entries[at]);
			uint32_t position = (FromBcd(entries[at + 1]) * 60 + FromBcd(entries[at + 2])) * 75 + FromBcd(entries[at + 3]);
			uint32_t lba = position >= LeadIn ? position - LeadIn : 0;

			bool merged = false;
			for(CdTrack& existing : tracks) {
				if(existing.Number == number) {
					existing.Lba = std::min(existing.Lba, lba);
					merged = true;
					break;
				}
			}
			if(!merged) {
				tracks.push_back({ number, lba, 0 });
			}
		}
		return tracks;
	}

public:
	//The disc's video tracks, in the order they lie on it.
	//
	//Two things say where the tracks are and they have to be read as one, or the two ways of
	//playing a video drift apart - which is exactly what happened: one read the sheet and was
	//right, the other read the disc's table as plain numbers and was wrong for two thirds of
	//the videos on a thirty-three track disc, and nothing ever compared them.
	//
	//The sheet is preferred. It is the disc's table of contents, it states the pause in front
	//of each track outright where the table inside can only be assumed to carry the standard
	//one, and it is already what the drive counts its tracks from. The table inside is read
	//when there is no sheet or the sheet names no tracks: it travels with the data and cannot
	//be lost or renamed. Across the twelve track-bearing discs here - a hundred and thirteen
	//tracks - the two agree on every track's start, and on where the video files themselves
	//begin.
	//
	//Track 1 is the filesystem and is not returned. A track runs up to the pause in front of
	//the next one, and the last runs to the end of the image.
	static vector<CdTrack> ReadTracks(CdImageFile& image, string discPath)
	{
		vector<CdTrack> found = ReadSheetTracks(discPath);
		if(found.size() < 2) {
			found = ReadDiscTableTracks(image);
		}

		std::sort(found.begin(), found.end(), [](const CdTrack& a, const CdTrack& b) { return a.Lba < b.Lba; });

		uint32_t imageEnd = (uint32_t)(image.Size() / SectorSize);
		vector<CdTrack> tracks;
		for(size_t i = 0; i < found.size(); i++) {
			if(found[i].Number < 2) {
				continue;
			}

			uint32_t end = imageEnd;
			if(i + 1 < found.size() && found[i + 1].Lba > found[i].Lba) {
				uint32_t next = found[i + 1].Lba;
				end = next > found[i].Lba + TrackPause ? next - TrackPause : next;
			}
			if(end > found[i].Lba && found[i].Lba < imageEnd) {
				tracks.push_back({ found[i].Number, found[i].Lba, end - found[i].Lba });
			}
		}
		return tracks;
	}

	//The track a play names. Titles number their videos from one and the disc counts the
	//filesystem as track 1, so the two differ by one everywhere this is asked.
	static bool FindVideoTrack(const vector<CdTrack>& tracks, uint32_t video, CdTrack& found)
	{
		for(const CdTrack& track : tracks) {
			if(track.Number == video + 1) {
				found = track;
				return true;
			}
		}
		return false;
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
