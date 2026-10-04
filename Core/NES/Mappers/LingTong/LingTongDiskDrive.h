#pragma once
#include "pch.h"
#include "Utilities/ISerializable.h"
#include "Utilities/Serializer.h"
#include "Utilities/CRC32.h"
#include "Shared/MessageManager.h"

//The LingTong SMART-128's floppy drive: a 3.5" drive behind an Apple II style Disk II controller.
//The controller is sixteen soft switches at $5600-$560F and a read latch, and the software does
//everything else - finds the address marks, decodes the GCR, steps the head - so what this has to
//be is the disk going round under the head, one bit cell at a time.
//
//The switches, from the BIOS's own seek and boot code:
//  $5600/$5601  head 1 / head 0
//  $5602/$5603  step line off / on - a step per pulse, in the direction set below
//  $5604/$5605  direction: in (towards the hub) / out (towards cylinder 0)
//  $5606/$5607  a mode the boot sequence flips between retries; $5607 is the one these disks need
//  $5608/$5609  motor off / on
//  $560C/$560D  Q6 low / high - reading $560C gives the latch
//  $560E/$560F  Q7 low (read) / high (write); with Q6 high, $560E reads the write protect in bit 7
//There is no track 0 sensor: the BIOS takes the head to be at cylinder 85 and steps out until it
//believes it is at 0, and the drive's end stop does the rest.
//
//The disks are bit streams, not sectors: the system tracks carry 36 6-and-2 sectors a side with
//the BIOS's own translate table, and the data tracks 26 4-and-4 sectors with their own check, so
//no sector image can hold them. A WOZ 2 image does - one revolution of bits per track, TMAP
//index = cylinder * 2 + head as for double-sided 3.5" disks, the bit cell in INFO (1.75us here).
class LingTongDiskDrive : public ISerializable
{
private:
	static constexpr int TrackSlots = 160;
	static constexpr uint32_t MaxCylinder = 81;

	struct Track
	{
		uint32_t Offset = 0;   //where its bits start in the file
		uint32_t BitCount = 0;
	};

	vector<uint8_t> _file;
	Track _tracks[TrackSlots] = {};
	uint8_t _tmap[TrackSlots] = {};
	bool _fiveInch = false;
	bool _writeProtected = false;
	bool _dirty = false;
	string _filename;
	double _cyclesPerBit = 3.1;
	double _clockRate = 1773447.0;
	double _bitCell = 1.75e-6;

	//The mechanism
	uint8_t _cylinder = 0;
	uint8_t _head = 0;
	bool _dirOut = false;
	bool _stepLine = false;
	bool _motor = false;
	bool _altMode = false;

	//The controller
	bool _q6 = false;
	bool _q7 = false;
	uint8_t _latch = 0;     //the byte the CPU sees, once complete
	bool _latchFull = false;
	bool _latchRead = false;
	uint8_t _shift = 0;     //the byte coming in behind it
	uint8_t _writeReg = 0;

	//Where the disk is under the head
	uint64_t _lastCycle = 0;
	double _bitPhase = 0;
	uint32_t _bitPos = 0;

	int32_t TrackIndex()
	{
		uint32_t slot = _fiveInch ? _cylinder * 4 : _cylinder * 2 + _head;
		if(slot >= TrackSlots || _tmap[slot] == 0xFF) {
			return -1;
		}
		return _tmap[slot];
	}

	bool GetBit(Track& t, uint32_t pos)
	{
		return (_file[t.Offset + (pos >> 3)] >> (7 - (pos & 7))) & 1;
	}

	void SetBit(Track& t, uint32_t pos, bool bit)
	{
		uint8_t& b = _file[t.Offset + (pos >> 3)];
		uint8_t mask = (uint8_t)(0x80 >> (pos & 7));
		uint8_t before = b;
		b = bit ? (b | mask) : (b & ~mask);
		if(b != before) {
			_dirty = true;
		}
	}

	//One bit cell passes under the head
	void ClockBit(Track* t, uint32_t count)
	{
		bool bit = t && t->BitCount ? GetBit(*t, _bitPos) : false;
		if(_q7) {
			//Writing: the register leaves MSB first, a bit a cell, zeros once it runs out -
			//which is how the 10-cell sync bytes come out of a CPU that waits longer to reload
			if(t && t->BitCount && !_writeProtected) {
				SetBit(*t, _bitPos, (_writeReg & 0x80) != 0);
			}
			_writeReg <<= 1;
		} else {
			//A byte the CPU has read goes at the next cell; one it has not stays until the next
			//one is complete. Leading zeros are dropped, which is what lets the self-sync bytes
			//bring a reader that started mid-byte into step.
			if(_latchFull && _latchRead) {
				_latchFull = false;
			}
			if(_shift != 0 || bit) {
				_shift = (uint8_t)((_shift << 1) | (bit ? 1 : 0));
				if(_shift & 0x80) {
					_latch = _shift;
					_latchFull = true;
					_latchRead = false;
					_shift = 0;
				}
			}
		}
		_bitPos = count ? (_bitPos + 1) % count : 0;
	}

	void Advance(uint64_t cycle)
	{
		uint64_t elapsed = cycle - _lastCycle;
		_lastCycle = cycle;
		if(!_motor || _file.empty()) {
			return;
		}

		double cells = elapsed / _cyclesPerBit + _bitPhase;
		uint64_t n = (uint64_t)cells;
		_bitPhase = cells - (double)n;

		int32_t index = TrackIndex();
		Track* t = index >= 0 ? &_tracks[index] : nullptr;
		uint32_t count = t ? t->BitCount : 0;

		//Long idle stretches: only the last few bytes' worth decide what the latch holds
		if(n > 256 && count) {
			uint64_t skip = n - 256;
			if(_q7) {
				for(uint64_t i = 0; i < skip && i < count; i++) {
					ClockBit(t, count);
				}
			} else {
				_bitPos = (uint32_t)((_bitPos + skip) % count);
			}
			n = 256;
		}
		for(uint64_t i = 0; i < n; i++) {
			ClockBit(t, count);
		}
	}

	//Keeps the angle when the head moves to a track of a different length
	void ChangeTrack(int32_t before)
	{
		int32_t after = TrackIndex();
		if(before == after) {
			return;
		}
		uint32_t oldCount = before >= 0 ? _tracks[before].BitCount : 0;
		uint32_t newCount = after >= 0 ? _tracks[after].BitCount : 0;
		if(oldCount && newCount) {
			_bitPos = (uint32_t)((uint64_t)_bitPos * newCount / oldCount);
		} else {
			_bitPos = 0;
		}
		_shift = 0;
	}

	void Switch(uint8_t sw)
	{
		int32_t before = TrackIndex();
		switch(sw) {
			case 0x00: _head = 1; break;
			case 0x01: _head = 0; break;
			case 0x02: _stepLine = false; break;
			case 0x03:
				if(!_stepLine) {
					if(_dirOut) {
						if(_cylinder > 0) {
							_cylinder--;
						}
					} else if(_cylinder < MaxCylinder) {
						_cylinder++;
					}
				}
				_stepLine = true;
				break;
			case 0x04: _dirOut = false; break;
			case 0x05: _dirOut = true; break;
			case 0x06: _altMode = true; break;
			case 0x07: _altMode = false; break;
			case 0x08: _motor = false; break;
			case 0x09: _motor = true; break;
			case 0x0C: _q6 = false; break;
			case 0x0D: _q6 = true; break;
			case 0x0E: _q7 = false; break;
			case 0x0F: _q7 = true; break;
		}
		ChangeTrack(before);
	}

public:
	~LingTongDiskDrive()
	{
		if(_dirty) {
			Save();
		}
	}

	bool IsDiskInserted() { return !_file.empty(); }
	bool IsDirty() { return _dirty; }
	string GetFilename() { return _filename; }

	void SetClockRate(double rate)
	{
		_clockRate = rate;
		_cyclesPerBit = _bitCell * _clockRate;
	}

	bool Load(string filename)
	{
		ifstream f(filename, ios::in | ios::binary);
		if(!f) {
			return false;
		}
		vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if(data.size() < 12 + 8 || memcmp(data.data(), "WOZ2\xFF\x0A\x0D\x0A", 8) != 0) {
			MessageManager::Log("[LingTong] Not a WOZ 2 disk image: " + filename);
			return false;
		}

		memset(_tmap, 0xFF, sizeof(_tmap));
		memset(_tracks, 0, sizeof(_tracks));
		double bitCell = 1.75e-6;
		bool fiveInch = false;
		bool writeProtected = false;
		bool haveTracks = false;
		size_t pos = 12;
		while(pos + 8 <= data.size()) {
			uint32_t id = data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | ((uint32_t)data[pos + 3] << 24);
			uint32_t size = data[pos + 4] | (data[pos + 5] << 8) | (data[pos + 6] << 16) | ((uint32_t)data[pos + 7] << 24);
			size_t body = pos + 8;
			if(body + size > data.size()) {
				break;
			}
			if(id == 0x4F464E49 && size >= 40) { //INFO
				fiveInch = data[body + 1] == 1;
				writeProtected = data[body + 2] != 0;
				if(data[body] >= 2 && data[body + 39] != 0) {
					bitCell = data[body + 39] * 125e-9;
				} else {
					bitCell = fiveInch ? 4e-6 : 2e-6;
				}
			} else if(id == 0x50414D54 && size >= TrackSlots) { //TMAP
				memcpy(_tmap, &data[body], TrackSlots);
			} else if(id == 0x534B5254 && size >= TrackSlots * 8) { //TRKS
				for(int i = 0; i < TrackSlots; i++) {
					const uint8_t* e = &data[body + i * 8];
					uint32_t block = e[0] | (e[1] << 8);
					uint32_t blocks = e[2] | (e[3] << 8);
					uint32_t bits = e[4] | (e[5] << 8) | (e[6] << 16) | ((uint32_t)e[7] << 24);
					if(block && bits && (size_t)block * 512 + (bits + 7) / 8 <= data.size() && (bits + 7) / 8 <= blocks * 512) {
						_tracks[i].Offset = block * 512;
						_tracks[i].BitCount = bits;
						haveTracks = true;
					}
				}
			}
			pos = body + size;
		}
		if(!haveTracks) {
			MessageManager::Log("[LingTong] No tracks in disk image: " + filename);
			return false;
		}

		_file = std::move(data);
		_filename = filename;
		_fiveInch = fiveInch;
		//A file that cannot be written is a disk with its tab open
		ofstream test(filename, ios::in | ios::out | ios::binary);
		_writeProtected = writeProtected || !test;
		_bitCell = bitCell;
		_cyclesPerBit = _bitCell * _clockRate;
		_dirty = false;
		_bitPos = 0;
		_shift = 0;
		_latchFull = false;
		return true;
	}

	bool Save()
	{
		if(_file.empty() || _filename.empty()) {
			return false;
		}
		uint32_t crc = CRC32::GetCRC(&_file[12], _file.size() - 12);
		_file[8] = crc & 0xFF;
		_file[9] = (crc >> 8) & 0xFF;
		_file[10] = (crc >> 16) & 0xFF;
		_file[11] = (crc >> 24) & 0xFF;
		ofstream f(_filename, ios::out | ios::binary);
		if(!f) {
			return false;
		}
		f.write((char*)_file.data(), _file.size());
		_dirty = false;
		return true;
	}

	void Eject()
	{
		if(_dirty) {
			Save();
		}
		_file.clear();
		_filename.clear();
		_dirty = false;
	}

	//Any access to $5600-$560F, read or write. Returns what a read sees.
	uint8_t Access(uint8_t sw, uint64_t cycle, bool isWrite, uint8_t value)
	{
		Advance(cycle);
		Switch(sw & 0x0F);
		if(isWrite) {
			//A store with both Q6 and Q7 high loads the write register
			if(_q6 && _q7) {
				_writeReg = value;
			}
			return 0;
		}

		if(_q6 && !_q7) {
			//Sensing the write protect
			return _writeProtected || _file.empty() ? 0x80 : 0x00;
		}
		if(_file.empty() || !_motor) {
			return _latch & 0x7F;
		}
		if(_latchFull) {
			_latchRead = true;
			return _latch;
		}
		return _shift;
	}

	void Serialize(Serializer& s) override
	{
		SV(_cylinder);
		SV(_head);
		SV(_dirOut);
		SV(_stepLine);
		SV(_motor);
		SV(_altMode);
		SV(_q6);
		SV(_q7);
		SV(_latch);
		SV(_latchFull);
		SV(_latchRead);
		SV(_shift);
		SV(_writeReg);
		SV(_lastCycle);
		SV(_bitPhase);
		SV(_bitPos);
	}
};
