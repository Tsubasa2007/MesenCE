#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/NesConsole.h"
#include "NES/NesMemoryManager.h"
#include "NES/Mappers/Bbk/BbkLpcAudio.h"
#include "NES/Mappers/Bbk/BbkPrinter.h"
#include "NES/NesControlManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/Serializer.h"

//BBK (步步高) 语音二号 learning machine - mapper 171, variant 2 in the header byte 9
//convention this branch uses to tell the learning machines apart (see MapperFactory).
//
//Nothing to do with the Inno/Holtek chipset the plain BBK uses - this board is a much
//simpler design that was recovered from a bank-wise dump of the ROM rather than ported
//from an existing emulator, so only what the BIOS demonstrably needs is implemented here.
//
//Memory controller: four 8KB windows, one 8-bit bank register each, giving a 2MB address
//space. The board itself carries 1MB (banks $00-$7F); banks $80-$FF are the add-on card
//slot, and read as an open bus when nothing is plugged in.
//
// - $5000 -> $8000-$9FFF
// - $5001 -> $A000-$BFFF
// - $5002 -> $C000-$DFFF
// - $5003 -> $E000-$FFFF
//
//The BIOS's own setters are four "STA $50xx / RTS" pairs at $D040/$D044/$D048/$D04C in
//bank $7E, reached through the system call table at $C0C6/$C0CC.
//
//$E000 holds bank $7F at power-on; that bank's reset vector points at a stub which maps
//the kernel bank ($7E) into $C000 and then jumps into it. Every executable bank repeats
//the same trampoline at $FFE0, so an inter-bank call can always get home.
//
//No CHR ROM: the machine has 8KB of CHR-RAM and blits glyphs into it out of PRG (the font
//lives in banks $42-$61 as ordinary 2bpp tile data). The blit routines are in bank $1C.
//
//2KB of nametable RAM, and the mirroring is switchable - bit 1 of $5027 picks it, clear for
//vertical and set for horizontal. Vertical is what the machine powers up in and all of its own
//software runs that way: the editor writes its menu and message overlays to $2400 and then
//displays them by pointing $2000's nametable bits at $2C00, which only works if the two alias.
//Four screens is what horizontal was first mistaken for: the typing game builds a picture in
//$2000 and then writes a full 1KB page to $2400, which lands back on top of the picture under
//horizontal but is a different page under vertical.
//
//The sub-card is what needs the other setting. The card's games are ordinary famiclone titles
//lifted off cartridges that were wired either way, so the card's launcher writes the mirroring
//it wants into $5027 in the same breath as the four bank registers, immediately before jumping
//into the game. Across seven recordings the machine's own software only ever writes $01/$00 to
//$5027 (the speech reset pulse, bit 1 clear), while a card game launch writes $02 exactly once.
//Both poles are needed to see this: a card game that writes $02 builds its screen in $2800 and
//then clears $2000, so under vertical the clear lands on top of the screen it just drew and the
//display goes black, while a card game that does not write $02 is blank under horizontal.
//
//Speech is the same LPC-10 synthesizer the BBK learning machine carries, wired to three
//ports instead of two: $5027 is the control port (bit 0 rising edge resets the decoder, and
//bit 1 is the mirroring select described above - one register, two unrelated jobs),
//$5007 is the data port, and reading $5007 gives the busy status. The BIOS pulses $5027
//bit 0 high-low-high at $FFCA before every phrase and then feeds the stream a byte at a
//time, waiting for bit 7 of $5007 and separately treating $FF as "not now". Every one of
//the 912 phrases in the ROM opens with the $D6 header this decoder syncs on.
//
//A parallel printer hangs off two lines that have nothing to do with the mapper's own window:
//a whole byte is written to $480F, and bit 0 of $4016 is the printer's ready line, which the
//send routine (bank $42, $9124) polls before each byte and gives up on after eight rounds -
//landing on a DOS-style Abort/Retry/Fail prompt. The stream is ESC/P with GB2312 text.
//Bit 0 of $4016 is also the port 1 controller's serial output. Nothing in the machine's own ROM
//reads it that way (its two other readers of the port take bits 2-3), but sub-card software is
//ordinary famiclone code and polls it as the pad - so the two cannot share the wire, and which
//one is attached is a setting rather than something the mapper can decide.
//
//$5010 is written by the BIOS with values whose meaning is still unidentified.
class Yuyin2Mapper : public BaseMapper
{
private:
	uint8_t _banks[4] = {};
	unique_ptr<BbkLpcAudio> _lpcAudio;
	BbkPrinter _printer;
	bool _printerNamed = false;

	//Font banks. The half-width face is 16 straight bytes per glyph indexed by character
	//code; the hanzi are 32 bytes each, but two of them share a 64-byte block by alternating
	//8-byte chunks, and the four chunks of one glyph are its quadrants in TL, TR, BL, BR
	//order. 256 hanzi per bank, indexed in GB2312 order from 啊.
	static constexpr uint32_t AsciiFontBank = 0x42;
	static constexpr uint32_t HanziFontBank = 0x44;

	NesControlManager* NesControls() { return (NesControlManager*)_console->GetControlManager(); }

	//Named lazily: the rom path is not available yet when the mapper is constructed
	BbkPrinter& Printer()
	{
		if(!_printerNamed) {
			_printerNamed = true;
			_printer.SetRomName(FolderUtilities::GetFilename(_emu->GetRomInfo().RomFile.GetFilePath(), false));
		}
		return _printer;
	}

	//The printer had no font of its own, so the machine's is lent to it - the glyphs then
	//match what the same text looks like on screen.
	bool LookupGlyph(uint16_t code, uint8_t* bitmap, int& width, int& height)
	{
		if(code >= 0x20 && code < 0x7F) {
			uint32_t offset = AsciiFontBank * 0x2000 + code * 16;
			if(offset + 16 > _prgSize) {
				return false;
			}
			memcpy(bitmap, _prgRom + offset, 16);
			width = 8;
			height = 16;
			return true;
		}

		uint8_t high = code >> 8;
		uint8_t low = code & 0xFF;
		if(high < 0xB0 || high > 0xF7 || low < 0xA1 || low > 0xFE) {
			return false;
		}

		uint32_t index = (high - 0xB0) * 94 + (low - 0xA1);
		uint32_t offset = (HanziFontBank + index / 256) * 0x2000 + (index % 256 / 2) * 64;
		uint32_t parity = index & 1;
		if(offset + 64 > _prgSize) {
			return false;
		}

		for(uint32_t quadrant = 0; quadrant < 4; quadrant++) {
			const uint8_t* src = _prgRom + offset + (quadrant * 2 + parity) * 8;
			for(uint32_t line = 0; line < 8; line++) {
				//Two bytes per row: left half then right half
				bitmap[((quadrant >= 2 ? 8 : 0) + line) * 2 + (quadrant & 1)] = src[line];
			}
		}
		width = 16;
		height = 16;
		return true;
	}

	void UpdatePrgMapping()
	{
		for(int i = 0; i < 4; i++) {
			SelectPrgPage(i, _banks[i]);
		}
	}

protected:
	uint16_t GetPrgPageSize() override { return 0x2000; }
	uint16_t GetChrPageSize() override { return 0x2000; }
	uint16_t GetChrRamPageSize() override { return 0x2000; }
	uint32_t GetChrRamSize() override { return 0x2000; }
	uint32_t GetWorkRamSize() override { return 0x2000; }
	uint32_t GetSaveRamSize() override { return 0; }

	uint16_t RegisterStartAddress() override { return 0x5000; }
	uint16_t RegisterEndAddress() override { return 0x5FFF; }
	bool AllowRegisterRead() override { return true; }
	bool EnableCpuClockHook() override { return true; }

	//This clone PPU does not have the 2C02's "a $2002 read one dot before vblank suppresses the
	//NMI" race. Sub-card software polls $2002 in a tight loop across the vblank edge and drives
	//its screen building from the NMI, so on a 2C02 it loses an NMI every time the poll lands on
	//that dot and stops drawing altogether - the nametable is left blank while CHR-RAM keeps
	//filling. The machine's own software never trips it, which is why this surfaced only once a
	//card was plugged in.
	//
	//Only this one. The other three departures BbkMapper makes (palette background hack, VRAM
	//write glitch, shared $2005/$2006 toggle) were each tried alone and together against the same
	//recording: with this hook alone the frame is pixel-identical to all four applied, and with
	//any of the other three alone the screen stays blank. That board is a different design - see
	//the note at the top of this file - so its PPU findings are not assumed to carry over here
	//without a case that shows them.
	bool EnablePpuNmiSuppressRace() override { return false; }

	void ProcessCpuClock() override
	{
		BaseProcessCpuClock();
		_lpcAudio->Clock();
		_printer.Clock();
	}

	//The base mapper only claims $4020 and up, so $4016 has to be asked for explicitly or
	//AddRegisterRange below never sees a read - the control manager keeps the address
	void GetMemoryRanges(MemoryRanges& ranges) override
	{
		BaseMapper::GetMemoryRanges(ranges);
		ranges.AddHandler(MemoryOperation::Read, 0x4016);
		ranges.SetAllowOverride();
	}

	void InitMapper(RomData& romData) override
	{
		//Chinese famiclone hardware, timed like its siblings
		romData.Info.System = GameSystem::Dendy;
	}

	void InitMapper() override
	{
		_romInfo.System = GameSystem::Dendy;

		memset(_banks, 0, sizeof(_banks));
		//The window the CPU fetches its reset vector from has to hold the boot bank before
		//any code runs; the other three are set by that bank's own startup path
		_banks[3] = 0x7F;
		UpdatePrgMapping();

		//Power-on state; $5027 bit 1 switches it, and the card's launcher uses that
		SetMirroringType(MirroringType::Vertical);

		//Neither printer line falls inside the mapper's own $5000-$5FFF window
		AddRegisterRange(0x480F, 0x480F, MemoryOperation::Write);
		AddRegisterRange(0x4016, 0x4016, MemoryOperation::Read);

		_lpcAudio.reset(new BbkLpcAudio(_console));
		_lpcAudio->Reset();
		_printerNamed = false;
		_printer.Reset();
		_printer.SetGlyphSource([this](uint16_t code, uint8_t* bitmap, int& width, int& height) {
			return LookupGlyph(code, bitmap, width, height);
		});

		memset(_chrRam, 0, _chrRamSize);
		memset(_workRam, 0, _workRamSize);
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		if(addr == 0x4016) {
			//Bit 0 is the port 1 controller's serial output and it is also the printer's ready
			//line - one wire, so only one of the two can be attached. With the printer on it the
			//line sits high and the send routine proceeds; with a controller on it the line
			//carries pad data, which reads as "not ready" and drops the machine into its own
			//Abort/Retry/Fail prompt, exactly as real hardware does with nothing plugged in.
			uint8_t value = NesControls()->ReadRam(addr);
			return _console->GetNesConfig().Yuyin2Printer ? ((value & 0xFE) | 0x01) : value;
		}

		if((addr & 0x0FFF) == 0x0007) {
			//Speech busy status - $8F when the FIFO can take more, $00 when it cannot,
			//which is what the BIOS's "bit 7 set and not $FF" test is looking for
			return _lpcAudio->ReadStatus();
		}
		return _console->GetMemoryManager()->GetOpenBus();
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr == 0x480F) {
			Printer().WriteData(value);
			return;
		}

		switch(addr & 0x0FFF) {
			case 0x0000:
			case 0x0001:
			case 0x0002:
			case 0x0003:
				_banks[addr & 0x03] = value;
				SelectPrgPage(addr & 0x03, value);
				break;

			case 0x0007: _lpcAudio->WriteData(value); break;

			case 0x0027:
				//Bit 1 is the nametable mirroring select - see the note above SetMirroringType
				SetMirroringType((value & 0x02) ? MirroringType::Horizontal : MirroringType::Vertical);
				_lpcAudio->WriteControl(value);
				break;
		}
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SVArray(_banks, 4);
		SV(_lpcAudio);
		SV(_printer);

		if(!s.IsSaving()) {
			UpdatePrgMapping();
		}
	}
};
