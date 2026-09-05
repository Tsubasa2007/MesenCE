#pragma once
#include "pch.h"
#include <fstream>

//A disc image, read where it lies rather than held in memory.
//
//The discs these machines take run to 700MB, and a drive only ever serves one program - a
//few hundred KB - out of one at a time. Reading the whole file in cost more than the machine
//being emulated ever had, and flattening it into a second buffer cost that again: 723MB read
//and 629MB kept, peaking near 1.3GB, for one mounted disc.
//
//Both layouts look the same from the outside. An address is an offset into 2048-byte user
//data, which is how the drives address a disc; the translation from a raw 2352-byte sector -
//12 sync bytes, a 4-byte header, an 8-byte subheader on Mode 2, then the payload - happens
//here. Nothing above this needs to know which kind of file it was handed.
class CdImageFile
{
private:
	static constexpr uint32_t UserSectorSize = 0x800;
	static constexpr uint32_t RawSectorSize = 2352;

	ifstream _file;
	uint64_t _sectorCount = 0;
	bool _raw = false;

	//One sector's worth, so a run of reads inside the same sector - which is what walking a
	//directory record by record does - does not seek for every one of them.
	uint8_t _cache[RawSectorSize] = {};
	uint64_t _cachedSector = UINT64_MAX;

	//Where the payload begins in the cached sector. Mode 2 carries a subheader ahead of it
	//and Mode 1 does not; byte 15 of the header says which this is.
	uint32_t PayloadOffset()
	{
		return _raw ? (_cache[15] == 2 ? 24 : 16) : 0;
	}

	bool LoadSector(uint64_t sector)
	{
		if(sector == _cachedSector) {
			return true;
		}
		if(sector >= _sectorCount) {
			return false;
		}

		uint32_t size = _raw ? RawSectorSize : UserSectorSize;
		_file.clear();
		_file.seekg((std::streamoff)(sector * size), ios::beg);
		if(!_file.read((char*)_cache, size)) {
			_cachedSector = UINT64_MAX;
			return false;
		}

		_cachedSector = sector;
		return true;
	}

public:
	//A disc already in the drive is left alone when this fails. The mount paths try a list
	//of candidate names across more than one folder and keep trying after the real disc is
	//in, so a failed open here must not be what takes it out again.
	//
	//The stream is the project's own - utf8::ifstream, through pch.h - and not std::ifstream.
	//The discs here name their image in a sheet written in another code page, and a plain
	//narrow stream cannot open what comes out of that.
	bool Open(string path)
	{
		ifstream probe(path, ios::in | ios::binary);
		if(!probe) {
			return false;
		}

		probe.seekg(0, ios::end);
		uint64_t size = (uint64_t)probe.tellg();
		probe.seekg(0, ios::beg);

		//A whole disc is ~700MB at most; a single program is a few hundred KB
		if(size == 0 || size > 800ull * 1024 * 1024) {
			return false;
		}

		//A raw dump opens with the sync pattern every sector carries
		static const uint8_t Sync[12] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
		uint8_t head[12] = {};
		probe.read((char*)head, sizeof(head));
		bool raw = size >= RawSectorSize && size % RawSectorSize == 0 && memcmp(head, Sync, sizeof(Sync)) == 0;

		uint64_t sectors = size / (raw ? RawSectorSize : UserSectorSize);
		if(sectors == 0) {
			return false;
		}

		probe.close();
		Close();
		_file.open(path, ios::in | ios::binary);
		if(!_file) {
			Close();
			return false;
		}

		_raw = raw;
		_sectorCount = sectors;
		_cachedSector = UINT64_MAX;
		return true;
	}

	void Close()
	{
		if(_file.is_open()) {
			_file.close();
		}
		_file.clear();
		_sectorCount = 0;
		_raw = false;
		_cachedSector = UINT64_MAX;
	}

	bool IsOpen() { return _sectorCount > 0; }
	uint64_t SectorCount() { return _sectorCount; }

	//The size of the disc as the drives address it, which is user data only
	uint64_t Size() { return _sectorCount * UserSectorSize; }

	//Read from an offset in that same user-data space, across as many sectors as it takes.
	//Anything past the end of the disc is left untouched and false comes back.
	bool Read(uint64_t offset, uint64_t length, uint8_t* out)
	{
		if(offset + length > Size()) {
			return false;
		}

		while(length > 0) {
			uint64_t sector = offset / UserSectorSize;
			uint32_t within = (uint32_t)(offset % UserSectorSize);
			uint32_t take = (uint32_t)std::min<uint64_t>(length, UserSectorSize - within);
			if(!LoadSector(sector)) {
				return false;
			}

			memcpy(out, _cache + PayloadOffset() + within, take);
			out += take;
			offset += take;
			length -= take;
		}
		return true;
	}

	//The subheader's submode byte, which says what a sector carries: bits 1 to 3 are video,
	//audio and data, and a sector with none of them is the padding an item's allocation is
	//made up to. A flat image has no subheader, so everything on one counts as data.
	uint8_t SubmodeAt(uint64_t sector)
	{
		if(!_raw) {
			return sector < _sectorCount ? 0x08 : 0x00;
		}
		return LoadSector(sector) ? _cache[18] : 0x00;
	}

	//The same, as a buffer. Empty when the range does not fit on the disc.
	vector<uint8_t> ReadRange(uint64_t offset, uint64_t length)
	{
		vector<uint8_t> data;
		if(length == 0 || offset + length > Size()) {
			return data;
		}

		data.resize((size_t)length);
		if(!Read(offset, length, data.data())) {
			data.clear();
		}
		return data;
	}
};
