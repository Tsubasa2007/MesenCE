#pragma once
#include "pch.h"
#include "Utilities/Serializer.h"

//The two cartridge controllers a converted game can ask this machine to be.
//
//$4181 bits 4-5 name the personality the window takes once a game has been handed the
//machine: 0 is a plain converted board that banks itself (see DrPcJrMapper's game modes),
//1 is the machine's own system-bank window, and 2 and 3 are these - the conversion left
//the game's own mapper writes alone and the machine imitates the chip instead. On the
//three game discs here that is 83 games on the first and 38 on the second, so leaving
//them out left a third of every disc unable to start at all.
//
//These classes hold nothing but the registers and the arithmetic. What a "bank" addresses
//is the machine's business, not theirs: PRG comes out as four 8KB banks of its PRG-RAM and
//CHR as eight 1KB pages of its CHR-RAM, and the mapper masks both to what the game was
//given ($4183) before laying the window out. The behaviour is the two chips' own, as
//Mesen implements them elsewhere; the wiring follows the reference emulator, which holds a
//Mapper001 and a Mapper004 inside MapperDrPCJr in exactly this shape.

//MMC1. Five writes of one bit each fill a shift register; the fifth commits it to whichever
//of the four registers the address picked.
class DrPcJrMmc1 final : public ISerializable
{
private:
	uint8_t _shift = 0;
	uint8_t _count = 0;
	//$8000: bits 0-1 mirroring, bit 2 which half of the window moves, bit 3 16KB or 32KB
	//PRG, bit 4 one 8KB CHR bank or two 4KB ones
	uint8_t _ctrl = 0x0C;
	uint8_t _chr0 = 0;
	uint8_t _chr1 = 0;
	uint8_t _prg = 0;
	bool _wramDisable = false;
	//Which of the two CHR registers was written last, for the 512KB PRG select below
	bool _chr1Last = false;
	uint64_t _lastWriteClock = 0;

	//A 512KB game has more PRG than the four-bit PRG register can reach, so bit 4 of the
	//CHR register that is in charge picks the half. Nothing smaller uses it.
	uint8_t ExtraReg() const { return (_chr1Last && (_ctrl & 0x10)) ? _chr1 : _chr0; }

public:
	void Reset()
	{
		_shift = 0;
		_count = 0;
		_ctrl = 0x0C;
		_chr0 = 0;
		_chr1 = 0;
		_prg = 0;
		_wramDisable = false;
		_chr1Last = false;
		_lastWriteClock = 0;
	}

	//True when the write changed something and the window has to be laid out again
	bool Write(uint16_t addr, uint8_t value, uint64_t masterClock)
	{
		//Two writes on consecutive cycles: only the first counts. A read-modify-write
		//instruction puts the unmodified byte out first, and taking both shifts the
		//register one bit too far.
		bool tooSoon = masterClock - _lastWriteClock < 2;
		_lastWriteClock = masterClock;
		if(tooSoon) {
			return false;
		}

		if(value & 0x80) {
			//Bit 7 empties the shift register and puts the PRG window back into the
			//16KB form with the high half fixed; nothing else changes.
			_shift = 0;
			_count = 0;
			_ctrl |= 0x0C;
			return true;
		}

		_shift = (uint8_t)((_shift >> 1) | ((value & 0x01) << 4));
		if(++_count < 5) {
			return false;
		}

		uint8_t val = _shift;
		_shift = 0;
		_count = 0;
		switch(addr & 0xE000) {
			case 0x8000: _ctrl = val; break;
			case 0xA000: _chr0 = (uint8_t)(val & 0x1F); _chr1Last = false; break;
			case 0xC000: _chr1 = (uint8_t)(val & 0x1F); _chr1Last = true; break;
			default: _prg = (uint8_t)(val & 0x0F); _wramDisable = (val & 0x10) != 0; break;
		}
		return true;
	}

	//Bits 0-1 carry the same four values the machine's own mirroring selection uses
	uint8_t Mirroring() const { return (uint8_t)(_ctrl & 0x03); }

	//prgBanks is how many 8KB banks of PRG-RAM the game was given
	void GetPrgBanks(uint8_t prgBanks, uint8_t out[4]) const
	{
		uint8_t half = (prgBanks == 0x40) ? (uint8_t)(ExtraReg() & 0x10) : (uint8_t)0;
		uint8_t low, high;
		if(_ctrl & 0x08) {
			if(_ctrl & 0x04) {
				low = (uint8_t)(_prg | half);
				high = (uint8_t)(0x0F | half);
			} else {
				low = half;
				high = (uint8_t)(_prg | half);
			}
		} else {
			low = (uint8_t)((_prg & 0x0E) | half);
			high = (uint8_t)(low + 1);
		}
		out[0] = (uint8_t)(low * 2);
		out[1] = (uint8_t)(low * 2 + 1);
		out[2] = (uint8_t)(high * 2);
		out[3] = (uint8_t)(high * 2 + 1);
	}

	void GetChrPages(uint16_t out[8]) const
	{
		if(_ctrl & 0x10) {
			for(int i = 0; i < 4; i++) {
				out[i] = (uint16_t)(_chr0 * 4 + i);
				out[4 + i] = (uint16_t)(_chr1 * 4 + i);
			}
		} else {
			uint8_t bank = (uint8_t)(_chr0 & 0x1E);
			for(int i = 0; i < 8; i++) {
				out[i] = (uint16_t)(bank * 4 + i);
			}
		}
	}

	void Serialize(Serializer& s) override
	{
		SV(_shift); SV(_count); SV(_ctrl); SV(_chr0); SV(_chr1); SV(_prg);
		SV(_wramDisable); SV(_chr1Last); SV(_lastWriteClock);
	}
};

//MMC3. Eight bank registers behind a two-write protocol, plus a counter clocked off the
//pattern-table address line, which is what the games split the screen with.
class DrPcJrMmc3 final : public ISerializable
{
private:
	uint8_t _bankSelect = 0;
	uint8_t _mirror = 0;
	uint8_t _registers[8] = { 0, 2, 4, 5, 6, 7, 0, 1 };

	uint8_t _irqLatch = 0;
	uint8_t _irqCounter = 0;
	bool _irqReload = false;
	bool _irqEnabled = false;
	//Held rather than pulsed: this mapper rewrites the interrupt line every CPU clock, so
	//a one-cycle assertion would be gone before the core sampled it - the same reason the
	//machine's own line counter latches its request.
	bool _irqPending = false;

	//The counter is clocked by $1000 in the PPU address going high after it has been low
	//long enough, which is what makes it count picture lines rather than fetches
	uint64_t _a12LowClock = 0;

public:
	void Reset()
	{
		_bankSelect = 0;
		_mirror = 0;
		uint8_t init[8] = { 0, 2, 4, 5, 6, 7, 0, 1 };
		memcpy(_registers, init, sizeof(_registers));
		_irqLatch = 0;
		_irqCounter = 0;
		_irqReload = false;
		_irqEnabled = false;
		_irqPending = false;
		_a12LowClock = 0;
	}

	//True when the write changed a bank or the mirroring
	bool Write(uint16_t addr, uint8_t value)
	{
		switch(addr & 0xE001) {
			case 0x8000:
				_bankSelect = value;
				return true;

			case 0x8001:
				//The two 2KB CHR registers cannot name an odd bank
				if((_bankSelect & 0x07) <= 1) {
					value &= (uint8_t)0xFE;
				}
				_registers[_bankSelect & 0x07] = value;
				return true;

			case 0xA000:
				_mirror = value;
				return true;

			case 0xA001:
				//Work RAM enable and write protect. This machine hands $6000-$7FFF to the
				//game whatever the game says, so there is nothing here to act on.
				return false;

			case 0xC000:
				_irqLatch = value;
				return false;

			case 0xC001:
				_irqCounter = 0;
				_irqReload = true;
				return false;

			case 0xE000:
				_irqEnabled = false;
				_irqPending = false;
				return false;

			default:
				_irqEnabled = true;
				return false;
		}
	}

	uint8_t Mirroring() const { return (uint8_t)((_mirror & 0x01) ? 3 : 2); }
	bool IrqPending() const { return _irqPending; }

	void GetPrgBanks(uint8_t prgBanks, uint8_t out[4]) const
	{
		uint8_t last = (uint8_t)(prgBanks - 1);
		if(_bankSelect & 0x40) {
			out[0] = (uint8_t)(last - 1);
			out[1] = _registers[7];
			out[2] = _registers[6];
			out[3] = last;
		} else {
			out[0] = _registers[6];
			out[1] = _registers[7];
			out[2] = (uint8_t)(last - 1);
			out[3] = last;
		}
	}

	void GetChrPages(uint16_t out[8]) const
	{
		const uint8_t* r = _registers;
		uint16_t low[4] = { (uint16_t)(r[0] & 0xFE), (uint16_t)(r[0] | 0x01),
			(uint16_t)(r[1] & 0xFE), (uint16_t)(r[1] | 0x01) };
		uint16_t high[4] = { r[2], r[3], r[4], r[5] };
		//Bit 7 swaps the 2KB half of the window with the 1KB half
		const uint16_t* first = (_bankSelect & 0x80) ? high : low;
		const uint16_t* second = (_bankSelect & 0x80) ? low : high;
		for(int i = 0; i < 4; i++) {
			out[i] = first[i];
			out[4 + i] = second[i];
		}
	}

	//Called for every PPU address the mapper is shown
	void ClockA12(uint16_t addr, uint64_t masterClock)
	{
		if(!(addr & 0x1000)) {
			if(_a12LowClock == 0) {
				_a12LowClock = masterClock;
			}
			return;
		}

		bool rising = _a12LowClock > 0 && (masterClock - _a12LowClock) >= 3;
		_a12LowClock = 0;
		if(!rising) {
			return;
		}

		uint8_t before = _irqCounter;
		if(_irqCounter == 0 || _irqReload) {
			_irqCounter = _irqLatch;
		} else {
			_irqCounter--;
		}
		if((before > 0 || _irqReload) && _irqCounter == 0 && _irqEnabled) {
			_irqPending = true;
		}
		_irqReload = false;
	}

	void Serialize(Serializer& s) override
	{
		SV(_bankSelect); SV(_mirror); SVArray(_registers, 8);
		SV(_irqLatch); SV(_irqCounter); SV(_irqReload); SV(_irqEnabled); SV(_irqPending);
		SV(_a12LowClock);
	}
};
