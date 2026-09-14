#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/BaseNesPpu.h"
#include "NES/NesConsole.h"
#include "NES/NesCpu.h"
#include "NES/Input/Sb2kKeyboard.h"
#include "NES/Input/YuxingKeyboard.h"
#include "NES/Input/YuxingMouse.h"
#include "NES/Mappers/Bbk/BbkLpcAudio.h"
#include "NES/Mappers/Bbk/BbkPrinter.h"
#include "NES/Mappers/Bbk/PcFdc.h"
#include "NES/Mappers/Yuxing/YuxingVcdDrive.h"
#include "NES/NesControlManager.h"
#include "Shared/BaseControlManager.h"
#include "Shared/MessageManager.h"
#include "Shared/NotificationManager.h"
#include "Shared/SystemActionManager.h"
#include "Shared/Interfaces/INotificationListener.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/Serializer.h"

//YuXing (裕兴) learning machine / VCD player - iNES mapper 169.
//Ported from the VirtuaNES-BBK fork (NES/Mapper/Mapper169.cpp, author fanoble).
//
//One mapper covers eight machine revisions, told apart by the PRG ROM's CRC32 (the
//iNES header carries no sub-mapper). The revision picks the DRAM/VRAM sizes and which
//input devices the machine has - see YuxingType and DetectMachineType().
//
//Hardware emulated:
// - Memory controller: PRG ROM (the BIOS, up to 1MB) plus 1MB "PRAM" work DRAM and up to
//   512KB "CRAM" video RAM, banked into the CPU/PPU spaces through $4800 (ROM 32K bank),
//   $5500 (mode/mirroring/$6000 page), $5501 (CHR 8K bank + MMC3-mode enable) and a bank
//   latch written through $8000-$FFFF.
// - $5500 bit 2 flips $8000-$FFFF from "ROM window" to "DRAM window", which is how
//   software loaded off a disc runs: it is copied into PRAM and then banked in.
// - 14x8 key matrix: row mask written to $4202/$4203 (also mirrored at $4302/$4303 and
//   $5004/$5005), column read back from $4207 - see YuxingKeyboard.
//
//CPU memory map:
// - $4018-$5FFF: mapper registers (see ReadRegister/WriteRegister)
// - $6000-$7FFF: PRAM 8K page ($5500 bits 0-1) when non-zero, else PRAM 8K page $3C
// - $8000-$FFFF: ROM 32K bank ($4800 bits 0-4), or a PRAM window when $5500 bit 2 is set
class YuxingMapper : public BaseMapper
{
public:
	//The V10 and V11 are 裕兴 machines too, but they sit on a plain mapper 178 board rather
	//than this one, so nothing about them can be read off the iNES header - not the Dendy
	//timing, and not the cut-down Subor keyboard and serial mouse they carry. The reference
	//emulator picks them out by the BIOS's PRG CRC32 and so does this port.
	static bool IsV10OrV11(uint32_t prgCrc)
	{
		return prgCrc == 0xCB7AA37A || prgCrc == 0x8E53518B;
	}

	//The two earliest revisions predate the key matrix and take an ordinary keyboard on
	//$4016/$4017 instead - so the input setup has to ask the mapper which machine this is
	//before it can plug the right one in.
	//
	//These read the CRC rather than _type: NesConsole sets the input up while it is loading
	//the ROM, and does not call InitSpecificMapper() - and so DetectMachineType() - until
	//afterwards, so _type is still Unknown at that point. _romInfo is already filled in.
	bool UsesXtKeyboard() { return IsV50(_romInfo.Hash.PrgCrc32); }
	bool UsesFamilyBasicKeyboard() { return IsV40(_romInfo.Hash.PrgCrc32); }

	static bool IsV40(uint32_t prgCrc) { return prgCrc == V40PrgCrc; }
	static bool IsV50(uint32_t prgCrc) { return prgCrc == V50PrgCrc || prgCrc == V50WuBiPrgCrc; }

	//Named here rather than only in DetectMachineType()'s switch so the two cannot drift
	static constexpr uint32_t V40PrgCrc = 0xCEAC04C7;
	static constexpr uint32_t V50PrgCrc = 0x3B02AF09;
	static constexpr uint32_t V50WuBiPrgCrc = 0x871254E8;

private:
	//Handles the FDS disk shortcut keys, reused here to swap VCD discs
	class DiscSwapListener final : public INotificationListener
	{
	private:
		YuxingMapper* _mapper;

	public:
		DiscSwapListener(YuxingMapper* mapper) : _mapper(mapper) {}

		void ProcessNotification(ConsoleNotificationType type, void* parameter) override
		{
			if(type == ConsoleNotificationType::ExecuteShortcut) {
				ExecuteShortcutParams* params = (ExecuteShortcutParams*)parameter;
				switch(params->Shortcut) {
					case EmulatorShortcut::FdsEjectDisk: _mapper->EjectDisc(); break;
					case EmulatorShortcut::FdsInsertNextDisk: _mapper->InsertNextDisc(); break;
					case EmulatorShortcut::FdsInsertDiskNumber: _mapper->InsertDisc(params->Param); break;
					default: break;
				}
			}
		}
	};

	YuxingVcdDrive _vcd;
	shared_ptr<DiscSwapListener> _swapListener;
	bool _discChecked = false;

	//The 软驱一号 floppy add-on. This machine drives the PC-style controller model rather
	//than the simplified one the BBK uses - see PcFdc - so the register file is exposed
	//directly: $4200 = data rate, $4201 = digital output, $4205 = data, $4304 = main
	//status, $4305 = data.
	PcFdc _fdc;

	//Speech synthesizer command state ($4700). The chip is fed the LPC-10 bitstream one
	//byte at a time, each byte split across two $Cx writes (low nibble first) and
	//scrambled with $41.
	bool _lpcReceiving = false;
	uint8_t _lpcNibbleCount = 0;
	uint8_t _lpcByte = 0;
	unique_ptr<BbkLpcAudio> _lpcAudio;

	//The printer hangs off the controller port's expansion lines rather than a parallel
	//register file, bit-banged a byte at a time by the BIOS routine at $FA1B: $4016 bit 0
	//carries the data, a rising edge on bit 1 clocks it in (MSB first), and bit 2 - held
	//high for the whole byte - is pulsed low afterwards to latch it. Bit 1 read back is the
	//printer's ready line, which the BIOS tests before every byte and gives up on after ten
	//tries ("打印机未准备好").
	BbkPrinter _printer;
	bool _printerNamed = false;
	uint8_t _lptLast = 0;
	uint8_t _lptShift = 0;

	//A power cycle recreates the mapper, so the media in the machine is remembered here and
	//re-mounted. Scoped to the ROM path so a different machine doesn't inherit it. Discs and
	//floppies are tracked separately because the machine can hold one of each.
	inline static string _persistedDiscRom;
	inline static string _persistedDiscPath;
	//Which program on the disc was picked. Selecting one power-cycles the machine, which
	//rebuilds the mapper - so like the disc path, this has to outlive it.
	inline static int32_t _persistedProgramIndex = -1;

	//Which menu keys were down last frame - see MenuKeyPressed
	bool _menuKeyHeld[(int)YuxingKeyboard::None + 1] = {};

	//How much of the menu picture's turn on the screen is left to run, in frames
	uint32_t _menuStillFrames = 0;
	inline static string _persistedFloppyRom;
	inline static string _persistedFloppyPath;

	//Machine revisions, in the reference emulator's numbering (Mapper169::YX_type)
	enum class YuxingType : uint8_t
	{
		//Unknown BIOS - treated as the largest configuration (1MB PRAM / 256KB CRAM)
		Unknown = 0xFF,
		//V4.0 - 32KB PRAM / 32KB CRAM, Family Basic keyboard
		V40 = 7,
		//V5.0 (and the WuBi variant) - 32KB PRAM / 32KB CRAM, Subor XT keyboard
		V50 = 1,
		//V7.0 - 128KB PRAM / 32KB CRAM
		V70 = 2,
		//V8.0-C - 128KB PRAM / 32KB CRAM
		V80C = 3,
		//V8.2-D / V8.3-D - 128KB PRAM / 32KB CRAM, matrix rows 9 and 11 lose their new keys
		V8xD = 4,
		//V9.0-98 - 1MB PRAM / 128KB CRAM
		V9098 = 5,
		//V9.2-98 / V9.2-F - 1MB PRAM / 256KB CRAM, VCD player models
		V92 = 6
	};

	//Work RAM is the 1MB PRAM and nothing else - every CPU window that holds RAM, the
	//$6000-$7FFF one included, is an 8K page of it.
	static constexpr uint32_t PramSize = 0x100000;

	//The MMC3 clone's own work RAM, after the machine's own and out of reach of every bank
	//number there is - see UpdatePrgMapping
	static constexpr uint32_t Mmc3WorkRamSize = 0x2000;

	YuxingType _type = YuxingType::Unknown;

	//16K PRAM bank mask and 8K CRAM bank mask, both set from the machine revision
	uint8_t _pramMask = 0x3F;
	uint8_t _cramMask = 0x0F;

	//How many 8K CRAM banks the loader filled for the program now running
	uint8_t _cramLoaded = 0;

	//Key matrix row select - 14 bits, written as two halves through $4202/$4203
	uint16_t _keyRowMask = 0;

	//XT keyboard state (V5.0 only) - see the XT* helpers. Named after the reference
	//emulator's EXPAD_XT_Keyboard members so the two can be compared line by line.
	bool _xtOut = false;
	bool _xtEnabled = false;
	bool _xtLsb = false;
	bool _xtMsb = false;
	bool _xtB1 = false;
	bool _xtB2 = false;
	uint8_t _xtScan = 0;
	uint8_t _xtScanPrev = 0;

	//$5002: bit 1 clear = the VCD player's own screen is showing, set = normal machine
	//screen. The V9.2 models power on in VCD mode; every other revision reads it as 2.
	uint8_t _reg5002 = 2;

	uint8_t _reg4800 = 0;
	uint8_t _reg5500 = 0;
	uint8_t _reg5501 = 0;
	//Bank latch written through the $8000-$FFFF window while $5500 bit 2 is set
	uint8_t _reg8000 = 0;

	//MMC3-clone banking mode ($5501 bit 7). Cartridge-style software banks the whole
	//$8000-$FFFF window and both pattern tables through a private MMC3 register file, out
	//of PRAM/CRAM rather than ROM, with a per-scanline (not A12-based) counter IRQ.
	//Ported from the reference emulator's MMC3Base.
	bool _mmc3Mode = false;
	uint8_t _mmc3Cmd = 0;
	uint8_t _mmc3Prg0 = 0, _mmc3Prg1 = 1;
	uint8_t _mmc3Chr01 = 0, _mmc3Chr23 = 2, _mmc3Chr4 = 4, _mmc3Chr5 = 5, _mmc3Chr6 = 6, _mmc3Chr7 = 7;
	uint8_t _mmc3IrqLatch = 0xFF, _mmc3IrqCounter = 0, _mmc3IrqPreset = 0, _mmc3IrqPresetVbl = 0;
	bool _mmc3IrqEnable = false;
	int32_t _lastPpuScanline = -2;
	int32_t _lastBandScanline = -2;

	//Band currently mapped at $1000-$1FFF in the 4-band split; $FF = needs rebanking
	uint8_t _lastSplitBand = 0xFF;

	//The V9.2 models power on as a VCD player. Ejecting the disc leaves that mode and
	//soft-resets into the learning machine - the reference emulator does the same thing
	//from a host key combination, since the machine's own "computer" key is not on the
	//scanned matrix.
	bool _vcdMode = false;
	//Set while the BIOS has the serial link switched to the keyboard rather than the
	//drive; the controller port has to stay quiet for the duration
	bool _vcdKeyboardSelected = false;

	//The reference emulator zero-fills PRAM/CRAM at power-on
	uint8_t* Pram() { return _workRam; }

	//8K PRAM window. The reference wraps the bank at 128 pages (1MB) rather than masking
	//it to the fitted size, so an out-of-range bank folds back instead of reading open bus.
	void MapPramPage(uint8_t cpuPage, int32_t bank)
	{
		uint16_t start = (uint16_t)(cpuPage * 0x2000);
		SetCpuMemoryMapping(start, (uint16_t)(start + 0x1FFF), PrgMemoryType::WorkRam, ((uint32_t)bank % 0x80) * 0x2000, MemoryAccessType::ReadWrite);
	}

	void MapPram16k(uint8_t cpuPage, int32_t bank)
	{
		MapPramPage(cpuPage, bank * 2);
		MapPramPage(cpuPage + 1, bank * 2 + 1);
	}

	void MapPram32k(int32_t bank)
	{
		for(uint8_t i = 0; i < 4; i++) {
			MapPramPage(4 + i, bank * 4 + i);
		}
	}

	void MapRom16k(uint8_t cpuPage, int32_t bank)
	{
		SelectPrgPage(cpuPage - 4, (uint16_t)(bank * 2));
		SelectPrgPage(cpuPage - 3, (uint16_t)(bank * 2 + 1));
	}

	void MapRom32k(int32_t bank)
	{
		for(uint16_t i = 0; i < 4; i++) {
			SelectPrgPage(i, (uint16_t)(bank * 4 + i));
		}
	}

	//8K CRAM window over the whole pattern table
	void MapCram8k(int32_t bank)
	{
		for(uint16_t i = 0; i < 8; i++) {
			SelectChrPage(i, (uint16_t)(bank * 8 + i));
		}
	}

	//$4800 bit 7 turns on the video chip's split-screen mode. With $5500 bit 7 clear it is
	//the 2-screen split: the screen's top and bottom halves take their glyphs from
	//different halves of CRAM, the background becomes 1bpp, and the tile address' bit 3
	//comes from the screen column - which packs four 8x8 monochrome glyphs into the space
	//of one 2bpp tile, enough character shapes for Chinese text.
	//
	//With $5500 bit 7 set the chip instead splits the screen into four bands of eight tile
	//rows, each taking the $1000-$1FFF half of its pattern data from its own 4KB CRAM bank.
	//The learning-machine side of the V9.2 models draws its desktop this way; without it
	//every band renders from the same bank and the screen shows the same strip four times.
	bool IsSplit2Screen() { return (_reg4800 & 0x80) && !(_reg5500 & 0x80); }
	bool IsSplit4Screen() { return (_reg4800 & 0x80) && (_reg5500 & 0x80); }

	void UpdateSplitMode()
	{
		//Force the next fetch to rebank - the band bank depends on $5501 as well as the mode
		_lastSplitBand = 0xFF;
		_console->GetPpu()->SetSplitBgFetch(IsSplit2Screen() ? 1 : 0);
	}

	//The mouse reports on a different bit of the controller port while the VCD side is
	//running, so it has to be told which mode the machine is in.
	//
	//This has to be re-applied periodically, not just when the mode changes: the control
	//manager rebuilds its devices whenever the input configuration is touched, and a fresh
	//YuxingMouse defaults to the computer-side bit. A mouse left reporting on bit 0 drives
	//its serial stream straight into the joypad's data line, which the guest reads as
	//random button presses - menus wander up and down on their own.
	void UpdateMouseMode()
	{
		shared_ptr<YuxingMouse> mouse = _console->GetControlManager()->GetControlDevice<YuxingMouse>();
		if(mouse) {
			mouse->SetVcdMode(_vcdMode);
		}
	}

	//1K CRAM window
	void MapCram1k(uint16_t page, int32_t bank)
	{
		SelectChrPage(page, (uint16_t)(bank & 0x1FF));
	}

	//4K CRAM window (used for the 4-band split's $1000-$1FFF half)
	void MapCram4k(uint16_t page, int32_t bank)
	{
		for(uint16_t i = 0; i < 4; i++) {
			MapCram1k(page + i, bank * 4 + i);
		}
	}

	//===== MMC3-clone mode ($5501 bit 7) =====
	uint8_t Mmc3SetPrg(uint8_t value) { return value & 0x3F; }

	//The 8KB CRAM bank the clone selects has its low two bit-pairs swapped. The loader
	//writes a bank through $5501 at the same swapped position, so this undoes itself.
	static uint8_t Swap5(uint8_t bank)
	{
		return (uint8_t)((bank & 0x10) | ((bank << 2) & 0x0C) | ((bank >> 2) & 0x03));
	}

	//How far a CHR bank number reaches: only as far as the loader filled, rounded up to the
	//next power of two, and never past the video RAM the machine has. A cartridge MMC3 folds
	//a bank number to the CHR on its board and titles written for one rely on it - a program
	//holding sixteen banks asks for bank $11 and means bank $01. Nothing here knows how much
	//a program brought except the loader, which paged every bank of it through $5501 on the
	//way in. A program that brought none (it builds its own tiles) leaves this alone: all it
	//saw was the start-up write selecting bank 0, and no program carries a single 8K bank.
	uint8_t CramFoldMask()
	{
		if(_cramLoaded < 2) {
			return _cramMask;
		}
		uint8_t mask = 0;
		while(mask < _cramLoaded - 1) {
			mask = (uint8_t)((mask << 1) | 1);
		}
		return mask < _cramMask ? mask : _cramMask;
	}

	//The clone's CHR bank number is not laid out the way the MMC3 expects: the 8KB bank it
	//selects has its low two bit-pairs swapped before the 1K page index is put back on.
	uint16_t Mmc3SetChr(uint8_t value)
	{
		uint8_t bank = (uint8_t)((value >> 3) & CramFoldMask());
		return (uint16_t)((Swap5(bank) << 3) | (value & 0x07));
	}

	//Map the four 8K PRG banks + eight 1K CHR banks per the current MMC3 register state.
	//Unlike the BBK's clone this one owns the whole $8000-$FFFF window.
	void Mmc3Sync()
	{
		bool pwrap = (_mmc3Cmd & 0x40) != 0;
		bool cwrap = (_mmc3Cmd & 0x80) != 0;

		MapPramPage(pwrap ? 6 : 4, Mmc3SetPrg(_mmc3Prg0));
		MapPramPage(5, Mmc3SetPrg(_mmc3Prg1));
		MapPramPage(pwrap ? 4 : 6, Mmc3SetPrg(0xFE));
		MapPramPage(7, Mmc3SetPrg(0xFF));

		uint16_t c[8] = {
			Mmc3SetChr((uint8_t)(_mmc3Chr01 + 0)), Mmc3SetChr((uint8_t)(_mmc3Chr01 + 1)),
			Mmc3SetChr((uint8_t)(_mmc3Chr23 + 0)), Mmc3SetChr((uint8_t)(_mmc3Chr23 + 1)),
			Mmc3SetChr(_mmc3Chr4), Mmc3SetChr(_mmc3Chr5), Mmc3SetChr(_mmc3Chr6), Mmc3SetChr(_mmc3Chr7)
		};
		for(uint16_t i = 0; i < 8; i++) {
			MapCram1k(cwrap ? (uint16_t)((i + 4) & 7) : i, c[i]);
		}
	}

	void Mmc3Write(uint16_t addr, uint8_t value)
	{
		switch(addr & 0xE001) {
			case 0x8000: _mmc3Cmd = value; Mmc3Sync(); break;

			case 0x8001:
				switch(_mmc3Cmd & 0x07) {
					case 0: _mmc3Chr01 = value & 0xFE; break;
					case 1: _mmc3Chr23 = value & 0xFE; break;
					case 2: _mmc3Chr4 = value; break;
					case 3: _mmc3Chr5 = value; break;
					case 4: _mmc3Chr6 = value; break;
					case 5: _mmc3Chr7 = value; break;
					case 6: _mmc3Prg0 = value; break;
					case 7: _mmc3Prg1 = value; break;
				}
				Mmc3Sync();
				break;

			case 0xA000:
				SetMirroringType((value & 0x01) ? MirroringType::Horizontal : MirroringType::Vertical);
				break;

			case 0xC000:
				_mmc3IrqLatch = value;
				break;

			case 0xC001:
				_mmc3IrqCounter |= 0x80;
				if(_console->GetPpu()->GetCurrentScanline() < 240) {
					_mmc3IrqPreset = 0xFF;
				} else {
					_mmc3IrqPresetVbl = 0xFF;
					_mmc3IrqPreset = 0;
				}
				break;

			case 0xE000:
				_mmc3IrqEnable = false;
				_console->GetCpu()->ClearIrqSource(IRQSource::External);
				break;

			case 0xE001:
				_mmc3IrqEnable = true;
				break;
		}
	}

	//Per-scanline IRQ counter, evaluated once per visible line while the display is on
	void Mmc3IrqSync(int32_t scanline)
	{
		if(scanline < 0 || scanline > 239 || !_console->GetPpu()->IsDisplayOn()) {
			return;
		}

		if(_mmc3IrqPresetVbl) { _mmc3IrqCounter = _mmc3IrqLatch; _mmc3IrqPresetVbl = 0; }
		if(_mmc3IrqPreset) {
			_mmc3IrqCounter = _mmc3IrqLatch;
			_mmc3IrqPreset = 0;
		} else if(_mmc3IrqCounter > 0) {
			_mmc3IrqCounter--;
		}

		if(_mmc3IrqCounter == 0) {
			if(_mmc3IrqEnable) {
				_console->GetCpu()->SetIrqSource(IRQSource::External);
			}
			_mmc3IrqPreset = 0xFF;
		}
	}

	void Mmc3Reset()
	{
		_mmc3Cmd = 0;
		_mmc3Prg0 = 0;
		_mmc3Prg1 = 1;
		_mmc3Chr01 = 0; _mmc3Chr23 = 2; _mmc3Chr4 = 4; _mmc3Chr5 = 5; _mmc3Chr6 = 6; _mmc3Chr7 = 7;
		_mmc3IrqEnable = false;
		_mmc3IrqCounter = 0;
		_mmc3IrqLatch = 0xFF;
		_mmc3IrqPreset = 0;
		_mmc3IrqPresetVbl = 0;
	}

	void DetectMachineType()
	{
		uint32_t crc = _romInfo.Hash.PrgCrc32;

		//Defaults: the largest configuration, used for any BIOS not in the table below
		_type = YuxingType::Unknown;
		_pramMask = 0x3F; //1MB PRAM
		_cramMask = 0x0F; //128KB CRAM
		_reg5002 = 2;

		switch(crc) {
			case V40PrgCrc: //V4.0
				_type = YuxingType::V40;
				_pramMask = 0x01; //32KB
				_cramMask = 0x03; //32KB
				break;

			case V50PrgCrc: //V5.0
			case V50WuBiPrgCrc: //V5.0 + WuBi
				_type = YuxingType::V50;
				_pramMask = 0x01;
				_cramMask = 0x03;
				break;

			case 0x3DF65263: //V7.0
				_type = YuxingType::V70;
				_pramMask = 0x07; //128KB
				_cramMask = 0x03;
				break;

			case 0x6BA6BD80: //V8.0-C
				_type = YuxingType::V80C;
				_pramMask = 0x07;
				_cramMask = 0x03;
				break;

			case 0x2B1B969E: //V8.2-D
			case 0xA70FD0F3: //V8.3-D
				_type = YuxingType::V8xD;
				_pramMask = 0x07;
				_cramMask = 0x03;
				break;

			case 0x6085FEE8: //V9.0-98
				_type = YuxingType::V9098;
				_pramMask = 0x3F;
				_cramMask = 0x0F;
				break;

			case 0x2A1E4D89: //V9.2-98
			case 0x5C6CE13E: //V9.2-F (YuxingVCD662A)
			case 0x011D8EBC: //V9.2-F (YuxingVCD315)
				_type = YuxingType::V92;
				_pramMask = 0x3F;
				_cramMask = 0x1F; //256KB
				//The VCD models come up showing the player's own screen
				_reg5002 = 0;
				break;
		}
	}

	//Rebuilds the $6000-$FFFF mapping from the current register state.
	//Mirrors Mapper169::SetBank_CPU().
	void UpdatePrgMapping()
	{
		if((_reg5500 & 0x04) && _mmc3Mode) {
			//The MMC3 clone owns $8000-$FFFF in this combination, and $6000-$7FFF is a page of
			//its own rather than the one $5500 picks below.
			//
			//Which page that is only matters in that it is not the other one. A program handing
			//the machine to another parks its whole zero page and stack there, along with the
			//address to come back to, and the program it hands over to loads its own code over
			//those same addresses - so on a single shared page the second overwrote the first's
			//way home and the machine never came back from a lesson. Neither one ever reads what
			//the other wrote, and what tells them apart is which mode the machine is in: the
			//parking, the saved address and the unparking all happen with the clone switched on,
			//and the loading and the running of the loaded code all happen with it switched off.
			//The handover code says as much itself - it opens the clone's RAM window with $A001
			//in the instruction before it reads the address back out of here.
			//
			//So it is the clone's own eight kilobytes, the way a cartridge's is, and not a page
			//of the machine's RAM at all. It cannot be one: every page is reachable - the clone's
			//PRG registers reach the first sixty-four (see Mmc3SetPrg) and the plain window reaches
			//all of them - so whichever page were picked, some program would be running out of it.
			//One did: a disc game asked the clone for the very page picked here, so its own writes
			//to this window came down on its own code, and five frames later it branched into what
			//it had written and stopped dead with the screen blanked.
			SetCpuMemoryMapping(0x6000, 0x7FFF, PrgMemoryType::WorkRam, PramSize, MemoryAccessType::ReadWrite);
			return;
		}

		if(_reg5500 & 0x04) {
			if(_type == YuxingType::V50) {
				//V5.0 runs loaded software out of ROM, not DRAM
				MapRom16k(4, 0x20 | _reg8000);
				MapRom16k(6, 0x27);
			} else if(_reg5500 & 0x40) {
				MapPram32k(_reg8000 >> 1);
			} else {
				MapPram16k(4, _reg8000);
				MapPram16k(6, _pramMask);
			}
		} else {
			MapRom32k(_reg4800 & 0x1F);
		}

		SetMirroringType((_reg5500 & 0x08) ? MirroringType::Horizontal : MirroringType::Vertical);

		//$6000-$7FFF: an 8K PRAM page picked by $5500 bits 0-1, with selector 0 the odd one
		//out at page $3C. The reference masks that page with the 16K bank mask even though
		//it is an 8K page number - kept as-is, since the small machines rely on the value it
		//folds down to (32KB PRAM is four pages, so selectors 0-3 cover all of it).
		//
		//This window is NOT a private scratch RAM: the BIOS' own system check fills PRAM
		//with a known pattern through the $8000 window and then reads it back here with
		//selectors 1-3, so a separate buffer reads back as zeroes and fails the check.
		if(_reg5500 & 0x03) {
			MapPramPage(3, _reg5500 & 0x03);
		} else if(_type != YuxingType::V50) {
			MapPramPage(3, 0x3C & _pramMask);
		}
	}

protected:
	//The 2C02 suppresses the vblank flag and the NMI when $2002 is read on the dot before
	//vblank. The BBK, 语音二号 and Dr. PC Jr. clone PPUs were all found to lack that race,
	//and so does this one: the V5.0's BIOS polls $2002 in a 7-cycle loop while its own NMI
	//handler reads the same register, and on Dendy's constant-length frame the poll lands on
	//that exact dot and kills the frame's flag - leaving the machine waiting forever.
	bool EnablePpuNmiSuppressRace() override { return false; }

	uint16_t GetPrgPageSize() override { return 0x2000; }
	uint16_t GetChrPageSize() override { return 0x400; }
	uint16_t GetChrRamPageSize() override { return 0x400; }
	uint32_t GetChrRamSize() override { return 0x80000; } //512KB CRAM
	uint32_t GetWorkRamSize() override { return PramSize + Mmc3WorkRamSize; }
	uint32_t GetWorkRamPageSize() override { return 0x2000; }
	bool ForceWorkRamSize() override { return true; }
	uint32_t GetSaveRamSize() override { return 0; }

	uint16_t RegisterStartAddress() override { return 0x4018; }
	uint16_t RegisterEndAddress() override { return 0x5FFF; }
	bool AllowRegisterRead() override { return true; }
	bool EnableCpuClockHook() override { return true; }
	bool EnableVramAddressHook() override { return true; }

	void ProcessCpuClock() override
	{
		BaseProcessCpuClock();
		_fdc.Clock();
		_lpcAudio->Clock();
		_printer.Clock();

		//The reference emulator renders scanline N and then runs its per-scanline logic, so
		//fire it during that line's hblank (after the sprite fetches, PPU cycle >= 321) -
		//the same placement the BBK port uses.
		int32_t scanline = _console->GetPpu()->GetCurrentScanline();
		uint32_t cycle = _console->GetPpu()->GetCurrentCycle();

		if(scanline != _lastBandScanline && cycle >= 257) {
			_lastBandScanline = scanline;
			LatchSplitBand();
		}

		if(scanline != _lastPpuScanline && cycle >= 321) {
			_lastPpuScanline = scanline;
			if(_mmc3Mode) {
				Mmc3IrqSync(scanline);
			}
			if(scanline == 0) {
				//Once a frame, cheap enough, and survives the control manager rebuilding
				//its devices behind the mapper's back
				UpdateMouseMode();
				if(UsesXtKeyboard()) {
					XtSync();
				}
			}
		}
	}

	//The VCD drive shares the controller port with the pad and the mouse, so the mapper
	//has to take over $4016/$4017 and merge the drive's contribution with whatever the
	//control manager returns - the reference emulator ORs the two the same way. Only the
	//$4016 write is claimed; $4017 writes belong to the APU frame counter.
	void GetMemoryRanges(MemoryRanges& ranges) override
	{
		BaseMapper::GetMemoryRanges(ranges);
		ranges.AddHandler(MemoryOperation::Read, 0x4016, 0x4017);
		ranges.AddHandler(MemoryOperation::Write, 0x4016);
		ranges.SetAllowOverride();
	}

	void InitMapper(RomData& romData) override
	{
		//Dendy-timed famiclone hardware
		romData.Info.System = GameSystem::Dendy;
	}

	void InitMapper() override
	{
		_romInfo.System = GameSystem::Dendy;

		DetectMachineType();

		//The bank latch lives in the $8000-$FFFF window, so those writes have to reach
		//the mapper even when the window is mapped to readable DRAM
		AddRegisterRange(0x8000, 0xFFFF, MemoryOperation::Write);

		//GetMemoryRanges routes the controller ports here; these make the mapper actually
		//decode them rather than fall through to the PRG mapping
		AddRegisterRange(0x4016, 0x4017, MemoryOperation::Read);
		AddRegisterRange(0x4016, 0x4016, MemoryOperation::Write);

		_keyRowMask = 0;
		_xtOut = false;
		_xtEnabled = false;
		_xtLsb = false;
		_xtMsb = false;
		_xtB1 = false;
		_xtB2 = false;
		_xtScan = 0;
		_xtScanPrev = 0;
		_reg4800 = 0;
		_reg5500 = 0;
		_reg5501 = 0;
		_reg8000 = 0;
		_mmc3Mode = false;
		_vcdMode = (_type == YuxingType::V92);
		_vcdKeyboardSelected = false;
		_lpcReceiving = false;
		_lpcNibbleCount = 0;
		_lpcByte = 0;
		_lpcAudio.reset(new BbkLpcAudio(_console, BbkLpcAudio::LpcVariant::Yuxing));
		_lpcAudio->Reset();
		_printerNamed = false;
		_printer.Reset();
		_lptLast = 0;
		_lptShift = 0;
		_lastPpuScanline = -2;
		_lastBandScanline = -2;
		Mmc3Reset();
		_cramLoaded = 0;

		if(!_swapListener) {
			_swapListener.reset(new DiscSwapListener(this));
			_emu->GetNotificationManager()->RegisterNotificationListener(_swapListener);
		}
		_discChecked = false;

		//The reference emulator zero-fills all three RAMs at power-on
		memset(_workRam, 0, _workRamSize);
		memset(_chrRam, 0, _chrRamSize);

		//Reset state: ROM bank 0 over $8000-$FFFF, CRAM bank 0 in the pattern tables, and
		//the $6000 window where UpdatePrgMapping() would put it with all registers clear
		UpdateSplitMode();
		UpdatePrgMapping();
		MapCram8k(0);
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		if(!_discChecked) {
			CheckForDisc();
			UpdateMouseMode();
		}

		switch(addr) {
			case 0x4016:
			case 0x4017: {
				uint8_t value = NesControls()->ReadRam(addr);
				if(_vcdKeyboardSelected && addr == 0x4016) {
					//While the serial keyboard is selected the pad's shift register and the
					//microphone are held low - but NOT the mouse, which shares this port and
					//is clocked by the very $FF/$FE writes that select the keyboard. Masking
					//the whole port here instead is what made the mouse look dead.
					value &= (uint8_t)~0x07;
				}
				uint8_t vcdValue = 0;
				if(IsVcdActive()) {
					_vcd.Read(addr, vcdValue);
				}
				if(addr == 0x4016 && !_vcdMode && IsPrinterSelected()) {
					value |= 0x02;
				}
				if(UsesXtKeyboard()) {
					value |= (addr == 0x4016) ? XtRead4016() : XtRead4017();
				}
				return value | vcdValue;
			}

			case 0x4207: {
				//A pending sector byte takes priority over the key matrix
				if(IsVcdActive() && !_vcd.IsReadComplete()) {
					uint8_t value = 0;
					if(_vcd.Read(addr, value)) {
						return value;
					}
				}
				return ReadKeyMatrix();
			}

			//$4304 = main status register, $4305 = data port
			case 0x4304:
			case 0x4305:
				_fdc.MarkActivity();
				return _fdc.Read((uint8_t)(addr & 0x07));

			//Speech status: bit 7 set while the synthesizer is still busy
			case 0x4701: return _lpcAudio->IsReady() ? 0x00 : 0x80;

			case 0x5002: return _reg5002;
		}

		//Anything unclaimed in $4018-$5FFF reads back the address' high byte, which is
		//what the reference emulator's default handler returns
		return (uint8_t)(addr >> 8);
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr >= 0x8000) {
			WriteBankLatch(addr, value);
			return;
		}

		switch(addr) {
			case 0x4016:
				//Only the computer side has a printer. In VCD mode this port is the drive's
				//serial link, and its clock line idles at exactly $06 - the value the printer
				//select test looks for - so letting the printer see it would force the ready
				//bit into every status byte the BIOS shifts back from the drive.
				//The XT keyboard's command words collide with the printer's clock and select
				//lines ($06 is both "clock high, selected" and an XT command), so the two
				//cannot share the port - and only the later, computer-shaped models have a
				//printer anyway.
				if(!_vcdMode && !UsesXtKeyboard()) {
					WriteLpt(value);
				}
				if(UsesXtKeyboard()) {
					XtWrite4016(value);
				}
				//$FF/$FE switches the serial link to the keyboard
				if(IsVcdActive()) {
					_vcdKeyboardSelected = (value == 0xFF || value == 0xFE);
					LatchKeyForVcd();
					_vcd.Write(addr, value);
				}
				NesControls()->WriteRam(addr, value);
				break;

			//Key matrix row select, low 8 rows. $4302/$5004 are the same register on the
			//later models (the decode ignores those address bits).
			case 0x4202:
			case 0x4302:
			case 0x5004:
				if(IsVcdActive() && _vcd.Write(addr, value)) {
					break;
				}
				_keyRowMask = (_keyRowMask & 0xFF00) | value;
				break;

			//Key matrix row select, high 6 rows
			case 0x4203:
			case 0x4303:
			case 0x5005:
				if(IsVcdActive() && _vcd.Write(addr, value)) {
					break;
				}
				_keyRowMask = (_keyRowMask & 0x00FF) | ((value & 0x3F) << 8);
				break;

			//Floppy controller: data-rate select, digital output register, data port
			case 0x4200: _fdc.MarkActivity(); _fdc.Write(7, value); break;
			case 0x4201: _fdc.MarkActivity(); _fdc.Write(2, value); break;
			case 0x4205: _fdc.MarkActivity(); _fdc.Write(5, value); break;
			//$4201 read is the DRQ line and $4200 read is disk-change; the reference
			//emulator's YuXing path never decodes them, so neither does this one.

			case 0x4700:
				WriteSpeech(value);
				break;

			case 0x4800:
				_reg4800 = value;
				UpdateSplitMode();
				UpdatePrgMapping();
				break;

			case 0x5500:
				_reg5500 = value;
				UpdateSplitMode();
				UpdatePrgMapping();
				break;

			case 0x5501:
				_reg5501 = value;
				MapCram8k(_reg5501 & _cramMask);
				//How much character data this program brought - see CramFoldMask. Bit 7 is the
				//mode flag rather than part of the bank, so a write that only hands the window
				//to the clone says nothing about the size.
				if((value & 0x80) == 0) {
					uint8_t loaded = (uint8_t)(Swap5((uint8_t)(value & _cramMask)) + 1);
					if(loaded > _cramLoaded) {
						_cramLoaded = loaded;
					}
				}
				_lastSplitBand = 0xFF;
				//Bit 7 hands $8000-$FFFF over to the MMC3 clone
				_mmc3Mode = (value & 0x80) != 0;
				//Both ways round, because $6000-$7FFF changes with the mode and Mmc3Sync()
				//only owns $8000-$FFFF. Leaving that window to the previous mode is what made
				//the two programs either side of a handover share it.
				UpdatePrgMapping();
				if(_mmc3Mode) {
					Mmc3Sync();
				}
				break;
		}
	}

	//$8000-$FFFF write. While the DRAM window is open this is either the bank latch
	//($4800 bit 5 set) or a plain store into the mapped DRAM; with the window closed the
	//write is swallowed by the ROM.
	void WriteBankLatch(uint16_t addr, uint8_t value)
	{
		if(_mmc3Mode) {
			Mmc3Write(addr, value);
			return;
		}

		if(_reg5500 & 0x04) {
			if(_type == YuxingType::V50) {
				_reg8000 = value & 0x07;
			} else if(_reg4800 & 0x20) {
				_reg8000 = value & _pramMask;
			} else {
				//Plain store into the DRAM mapped here. The register range intercepts the
				//write before it reaches the mapping, so repeat the lookup by hand.
				WriteMappedDram(addr, value);
			}
		}

		UpdatePrgMapping();
	}

	//Resolves the DRAM byte behind a $8000-$FFFF address using the same bank arithmetic
	//UpdatePrgMapping() used to map it, and stores through it.
	void WriteMappedDram(uint16_t addr, uint8_t value)
	{
		uint8_t cpuPage = (uint8_t)(addr >> 13);
		int32_t bank;
		if(_reg5500 & 0x40) {
			bank = (_reg8000 >> 1) * 4 + (cpuPage - 4);
		} else if(cpuPage < 6) {
			bank = _reg8000 * 2 + (cpuPage - 4);
		} else {
			bank = _pramMask * 2 + (cpuPage - 6);
		}
		_workRam[((uint32_t)bank % 0x80) * 0x2000 + (addr & 0x1FFF)] = value;
	}

	//Both split modes key off the nametable row's top two bits - the reference emulator
	//latches them once per scanline from the same place in the scroll address.
	//
	//2-screen: bit 12 of the tile address comes from that row rather than $2000 bit 4 (so
	//the screen's halves use different glyph banks) and bit 3 from the parity of the screen
	//column being fetched - the leftmost column is even, and in Mesen's pipeline that column
	//is fetched at cycle 321 of the previous scanline.
	//
	//4-band: the fetch is an ordinary 2bpp one, but $1000-$1FFF is rebanked per band.
	void ApplySplitBgFetch(uint8_t tileIndex, uint16_t videoRamAddr, uint16_t cycle, uint16_t& tileAddr) override
	{
		uint8_t band = (uint8_t)((videoRamAddr >> 8) & 0x03);
		uint16_t halfSelect = (band & 0x02) ? 0x1000 : 0;
		uint16_t column = (uint16_t)((((cycle - 1) >> 3) & 1) << 3);
		tileAddr = halfSelect | ((uint16_t)tileIndex << 4) | column | (videoRamAddr >> 12);
	}

	//4-band split: the video chip latches the row once per scanline and banks $1000-$1FFF
	//from it for that whole line, sprites included. Doing it per background fetch instead
	//looked equivalent - the row bits do not change within a line - but it is not: a $5501
	//write part way down the screen then took effect at the next tile rather than the next
	//line, which made a loaded game's text and score flicker between right and wrong.
	//
	//Latched at cycle 257, after the scroll address has stepped to the next row and before
	//that row's sprite patterns (257-320) and background prefetch (321-336) are fetched.
	void LatchSplitBand()
	{
		if(!IsSplit4Screen() || !_console->GetPpu()->IsDisplayOn()) {
			return;
		}
		uint8_t band = (uint8_t)((_console->GetPpu()->GetVideoRamAddr() >> 8) & 0x03);
		if(band != _lastSplitBand) {
			_lastSplitBand = band;
			MapCram4k(4, (band << 3) | ((_reg5501 & 0x03) << 1) | 1);
		}
	}

	//A pattern-table address on the PPU bus while the display is off is software pointing
	//$2006 at video RAM to upload through $2007. The reference emulator puts the full 8KB
	//window back for that; otherwise the upload would land in whichever band bank the last
	//rendered scanline happened to leave mapped.
	void NotifyVramAddressChange(uint16_t addr) override
	{
		if(addr >= 0x2000 || !IsSplit4Screen()) {
			return;
		}

		//Tell a CPU-driven access apart from a rendering fetch by *when* it happens, not by
		//the $2001 mask: software leaves rendering enabled across vblank, so testing the mask
		//meant this never fired for the usual vblank upload and the data landed in whichever
		//band bank the last rendered scanline left mapped.
		int32_t scanline = _console->GetPpu()->GetCurrentScanline();
		bool rendering = _console->GetPpu()->IsDisplayOn() && scanline >= 0 && scanline < 240;
		if(!rendering) {
			MapCram8k(_reg5501 & _cramMask);
			_lastSplitBand = 0xFF;
		}
	}

	//$4700 command port. $00 resets the decoder, $FF opens a data stream, and each $Cx
	//afterwards carries one nibble of a stream byte - low nibble first, the assembled byte
	//scrambled with $41.
	//
	//The assembled bytes are handed to BbkLpcAudio in its "PE" mode - the same decoder the
	//BBK and SB-2000 use, with this machine's coefficient set and no stream header.
	void WriteSpeech(uint8_t value)
	{
		if(value == 0x00) {
			_lpcReceiving = false;
			_lpcNibbleCount = 0;
			_lpcByte = 0;
			//A reset between phrases, like the BBK's $FF10 rising edge
			_lpcAudio->WriteControl(0);
			_lpcAudio->WriteControl(1);
		} else if(value == 0xFF) {
			_lpcReceiving = true;
		} else if((value & 0xF0) == 0xC0 && _lpcReceiving) {
			if(_lpcNibbleCount == 0) {
				_lpcByte = value & 0x0F;
				_lpcNibbleCount = 1;
			} else {
				_lpcByte |= (uint8_t)(value << 4);
				_lpcNibbleCount = 0;
				_lpcAudio->WriteData(_lpcByte ^ 0x41);
			}
		}
	}

	//The page is named after the ROM, and the name is only available once the ROM is loaded
	BbkPrinter& Printer()
	{
		if(!_printerNamed) {
			_printerNamed = true;
			_printer.SetRomName(FolderUtilities::GetFilename(_emu->GetRomInfo().RomFile.GetFilePath(), false));
		}
		return _printer;
	}

	//Shift one bit per rising clock edge, emit the byte when the select line is pulsed low
	void WriteLpt(uint8_t value)
	{
		uint8_t changed = _lptLast ^ value;

		if((changed & 0x02) && (value & 0x02)) {
			_lptShift = (uint8_t)((_lptShift << 1) | (value & 0x01));
		}

		if((changed & 0x04) && !(value & 0x04)) {
			Printer().WriteData(_lptShift);
		}

		_lptLast = value;
	}

	//$4016 bit 1 reads back as the printer's ready line, but only while the BIOS is talking
	//to the printer: this port also carries the pad, the mouse and the serial keyboard, and
	//the ready bit would collide with them.
	//
	//The gate is the exact idle word the BIOS writes before each ready test. A looser test
	//on bit 2 alone is NOT safe - $FF and $FE, which select the serial keyboard, both have
	//bit 2 set, and $FE also matches on bits 1 and 0.
	bool IsPrinterSelected() { return !UsesXtKeyboard() && _lptLast == 0x06; }

	bool IsVcdActive() { return _vcdMode && _vcd.IsDiscInserted(); }

	//$4016/$4017 are handled by the mapper here, so the controller port's own value has to
	//be fetched from the control manager and merged in by hand
	NesControlManager* NesControls() { return (NesControlManager*)_console->GetControlManager(); }

	//The serial keyboard reports one key's matrix position, sampled at the moment the
	//BIOS selects it
	void LatchKeyForVcd()
	{
		shared_ptr<YuxingKeyboard> keyboard = _console->GetControlManager()->GetControlDevice<YuxingKeyboard>();
		if(keyboard) {
			_vcd.PendingKeyCell = keyboard->GetLastPressedCell();
			_vcd.PendingModifiers = keyboard->GetModifiers();
		} else {
			_vcd.PendingKeyCell = -1;
			_vcd.PendingModifiers = 0;
		}
	}

	//===== XT keyboard (V5.0) =====
	//
	//The V5.0 predates the key matrix: its BIOS talks to a plain PC/XT keyboard over
	//$4016/$4017 instead, which is why the matrix registers ($4202/$4203/$4207) are never
	//touched by this machine. Ported from the reference emulator's EXPAD_XT_Keyboard,
	//whose author notes the protocol was reconstructed from the BIOS rather than from the
	//hardware - so it is transcribed here rather than tidied.
	//
	//The BIOS writes a command to $4016 to say which half of the scan code it wants
	//($05 = high nibble, $07 = low nibble, $06 = idle/clear), reads $4016 bit 1 back as
	//the handshake ("both halves have been asked for"), and takes the nibble itself off
	//$4017 in an inverted, shifted form.
	//
	//The device lives in the mapper rather than in a control device of its own because the
	//mapper already owns these two addresses for every YuXing machine; the key states come
	//from Sb2kKeyboard, which reports the same scan-code set 1 that an XT keyboard sends.

	//Sample the keyboard once a frame, exactly as the reference emulator's Sync() does.
	void XtSync()
	{
		shared_ptr<Sb2kKeyboard> keyboard = _console->GetControlManager()->GetControlDevice<Sb2kKeyboard>();

		_xtEnabled = false;
		_xtScanPrev = _xtScan;

		if(keyboard) {
			//The reference sweeps DirectInput's key array in order and stops at the first key
			//it finds held. DirectInput's indices are the scan codes themselves, so walking
			//the codes in ascending order visits the keys in the same order it does - which
			//matters because only one key is ever reported, and this decides which one wins.
			int32_t bestCode = -1;
			for(uint8_t i = 0; i < Sb2kKeyboard::KeyCount; i++) {
				if(keyboard->IsPressed(i)) {
					uint8_t code = Sb2kKeyboard::GetScanCode(i);
					if(bestCode < 0 || code < bestCode) {
						bestCode = code;
					}
				}
			}

			if(bestCode >= 0) {
				//Bit 7 is the break flag on this bus, so an extended key reports as the
				//unextended one it shares a code with - the arrow keys arrive as the numpad
				//keys, which is what an XT keyboard has instead of them.
				_xtScan = (uint8_t)(bestCode & 0x7F);
				_xtEnabled = true;
			}
		}

		if(!_xtEnabled) {
			//Nothing held: send the break code for whatever was last down
			_xtScan = (uint8_t)(_xtScanPrev | 0x80);
			_xtEnabled = true;
		}

		if(_xtScanPrev & 0x80) {
			//The break code was already sent last frame - send nothing until something
			//changes, so a release is reported once rather than held forever
			_xtEnabled = false;
		}
	}

	//$4016 bit 1: set once the BIOS has asked for both halves of the scan code
	uint8_t XtRead4016()
	{
		return (_xtB1 && _xtB2) ? 0x02 : 0x00;
	}

	//$4017: the requested nibble, inverted and shifted, with bit 4 carrying the nibble's
	//top bit
	uint8_t XtRead4017()
	{
		if(!_xtEnabled) {
			return 0;
		}

		if(_xtMsb) {
			uint8_t high = _xtScan & 0xF0;
			return (uint8_t)(((high ^ 0xFF) >> 3) & (high > 0x70 ? 0x0F : 0x1F));
		}

		if(_xtLsb) {
			uint8_t low = _xtScan & 0x0F;
			return (uint8_t)((((low ^ 0xFF) << 1) & 0x0F) | (low > 7 ? 0x00 : 0x10));
		}

		return 0;
	}

	//$4016 commands. Only $05/$06/$07 mean anything; the BIOS writes the others while
	//strobing the joypad and the reference ignores them there too.
	void XtWrite4016(uint8_t value)
	{
		switch(value) {
			case 0x05:
				_xtOut = !_xtOut;
				_xtMsb = true;
				_xtLsb = false;
				_xtB1 = true;
				break;

			case 0x06:
				_xtOut = false;
				_xtMsb = false;
				_xtLsb = false;
				_xtB1 = false;
				_xtB2 = false;
				break;

			case 0x07:
				_xtOut = !_xtOut;
				_xtMsb = false;
				_xtLsb = true;
				_xtB2 = true;
				break;
		}
	}

	uint8_t ReadKeyMatrix()
	{
		shared_ptr<YuxingKeyboard> keyboard = _console->GetControlManager()->GetControlDevice<YuxingKeyboard>();
		if(!keyboard) {
			return 0;
		}
		//The V8.x-D machines' matrix predates three keys and their BIOS never scans those cells
		return keyboard->GetColumns(_keyRowMask, _type == YuxingType::V8xD);
	}

	string GetConfiguredDiscFolder()
	{
		const char* folder = _console->GetNesConfig().BbkDiskFolder;
		return folder[0] ? string(folder) : string();
	}

	//Mounts the disc the user last chose for this ROM, or "<rom name>.bin" next to it.
	//Deferred to the first bus access so the emulator's rom info is fully set up.
	void CheckForDisc()
	{
		if(_discChecked) {
			return;
		}
		_discChecked = true;

		MountPairedMedia();

		//The V9.2 models power on showing the VCD player's screen, which asks for a key the
		//emulated keyboard cannot reach - it is normally left by ejecting the disc. An empty
		//drive puts nothing on that screen and offers no way off it, so drop straight to the
		//computer side: it is the useful state, and the only one a recording can start from.
		//
		//An empty drive and nothing else. Whatever is in the drive belongs on the player's
		//side, in every state it can be in: a disc carrying one program has had it picked
		//already, a disc carrying a library draws its own menu, and one whose program has not
		//been chosen yet is chosen from the disk list, which starts the machine again on it.
		//Skipping for any of those is what made a disc reachable only through that list.
		if(_vcdMode && !_vcd.HasDisc() && _console->GetNesConfig().YuxingSkipVcdScreen) {
			_vcdMode = false;
			_reg5002 = 2;
			UpdateMouseMode();
			MessageManager::Log("[YuXing] No disc - starting on the computer side");
		}
	}

	//A freshly loaded disc starts on its first program; if one was picked before the power
	//cycle that brought us back here, go to that one instead.
	void RestoreSelectedProgram()
	{
		if(_persistedProgramIndex >= 0 && _vcd.GetProgramCount() > 0) {
			_vcd.SelectProgram((uint32_t)_persistedProgramIndex);
			return;
		}

		//A disc carrying a single program has already made the choice, and that program is
		//what the machine would find and start by itself. Leaving it unpicked is what sent
		//these discs to the computer side at power-on: an unpicked disc answers exactly like
		//an empty drive, so the player's own screen had nothing on it and was skipped, and
		//the only way back was to pick the program out of the disk list by hand.
		//
		//A disc that carries a library of them is a different thing - there is no one program
		//to start, the machine's way of choosing is a menu it reads off the disc itself, and
		//guessing at the first one would start something arbitrary. Those are left alone.
		if(_vcd.GetProgramCount() == 1) {
			_vcd.SelectProgram(0);
		}
	}

	//Mounts the disc or floppy paired with the ROM. Returns true when a VCD disc was found.
	bool MountPairedMedia()
	{
		string romPath = _emu->GetRomInfo().RomFile.GetFilePath();

		if(_persistedDiscRom == romPath && !_persistedDiscPath.empty()) {
			if(_vcd.LoadDisc(_persistedDiscPath)) {
				RestoreSelectedProgram();
				MessageManager::Log("[YuXing] Re-inserted disc: " + _persistedDiscPath);
				return _vcd.IsDiscInserted();
			}
		}

		if(_persistedFloppyRom == romPath && !_persistedFloppyPath.empty()) {
			if(_fdc.LoadDiskImage(_persistedFloppyPath)) {
				MessageManager::Log("[YuXing] Re-inserted disk: " + _persistedFloppyPath);
			}
		}

		string baseName = FolderUtilities::GetFilename(romPath, false);
		vector<string> folders = { FolderUtilities::GetFolderName(romPath) };
		string configured = GetConfiguredDiscFolder();
		if(!configured.empty() && configured != folders[0]) {
			folders.push_back(configured);
		}

		for(string& folder : folders) {
			//A floppy for the 软驱一号 drive, paired with the ROM by name
			for(string ext : { ".img", ".IMG", ".ima", ".IMA" }) {
				if(_fdc.IsDiskInserted()) {
					break;
				}
				string diskPath = FolderUtilities::CombinePath(folder, baseName + ext);
				if(_fdc.LoadDiskImage(diskPath)) {
					MessageManager::Log("[YuXing] Mounted disk image: " + diskPath);
					break;
				}
			}

			//A .cue is preferred over the .bin it names, so a whole-disc image wins over a
			//single extracted program of the same name
			for(string ext : { ".cue", ".CUE", ".bin", ".BIN", ".iso", ".ISO" }) {
				string discPath = FolderUtilities::CombinePath(folder, baseName + ext);
				if(_vcd.LoadDisc(discPath)) {
					RestoreSelectedProgram();
					MessageManager::Log("[YuXing] Inserted disc: " + discPath);
					if(_vcd.GetProgramCount() > 0 && !_vcd.IsDiscInserted()) {
						MessageManager::DisplayMessage("YuXing", std::to_string(_vcd.GetProgramCount()) + " programs on disc - pick one from the disk list");
					}
					return _vcd.IsDiscInserted();
				}
			}
		}
		return false;
	}

public:
	//Media swapping - driven by the FDS disk shortcut keys via DiscSwapListener.
	//The machine takes two kinds of media and the list holds both: a .bin is a VCD disc for
	//the player side, a .img/.ima is a floppy for the 软驱一号 drive. Which device an entry
	//goes into is decided by its extension.
	static bool IsFloppyImage(const string& path)
	{
		string ext = path.size() >= 4 ? path.substr(path.size() - 4) : string();
		std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return (char)::tolower((uint8_t)c); });
		return ext == ".img" || ext == ".ima";
	}

	vector<string> GetDiskFileList()
	{
		//A whole-disc image carries its own list: the programs on the disc are what the swap
		//shortcuts step through, since swapping the image itself is not what a user wants
		//while one of its programs is running.
		if(_vcd.GetProgramCount() > 0) {
			vector<string> programs;
			for(uint32_t i = 0; i < _vcd.GetProgramCount(); i++) {
				programs.push_back(_vcd.GetProgramName(i));
			}
			return programs;
		}

		string folder = GetConfiguredDiscFolder();
		if(folder.empty()) {
			folder = FolderUtilities::GetFolderName(_emu->GetRomInfo().RomFile.GetFilePath());
		}
		vector<string> files = FolderUtilities::GetFilesInFolder(folder, { ".cue", ".bin", ".iso", ".img", ".ima" }, false);
		std::sort(files.begin(), files.end());

		//A .cue and the image it names are one disc; drop the image so the list has one entry
		//per medium rather than two. Matched on the filename alone - the resolved path comes
		//back with a separator the folder listing does not use, so comparing paths misses.
		auto leaf = [](const string& path) {
			//Not FolderUtilities::GetFilename: this also has to take the leaf of a name that
			//came out of a .cue rather than off the filesystem, and std::filesystem throws on
			//bytes that are not valid UTF-8 - which reached the UI as "external component has
			//thrown an exception" the moment the media folder was pointed at such a disc.
			size_t sep = path.find_last_of("/\\");
			string name = sep == string::npos ? path : path.substr(sep + 1);
			std::transform(name.begin(), name.end(), name.begin(), [](char c) { return (char)::tolower((uint8_t)c); });
			return name;
		};

		vector<string> named;
		for(string& file : files) {
			if(leaf(file).size() >= 4 && leaf(file).compare(leaf(file).size() - 4, 4, ".cue") == 0) {
				named.push_back(leaf(YuxingVcdDrive::ResolveCueSheet(file)));
			}
		}
		files.erase(std::remove_if(files.begin(), files.end(), [&named, &leaf](const string& file) {
			return std::find(named.begin(), named.end(), leaf(file)) != named.end();
		}), files.end());
		return files;
	}

	uint32_t GetDiskCount() { return (uint32_t)GetDiskFileList().size(); }

	//Whichever medium is loaded; the floppy wins when both are, since it is the one that
	//can be swapped while the machine runs
	//The image in the disc drive, for a front end that wants to look inside it - see
	//NesConsole::GetVideoDiscPath.
	string GetVcdDiscPath() { return _vcd.GetDiscFilename(); }

	//Where the video on the mounted disc is - see CdSegmentIndex
	vector<CdVideoReel> GetVideoReels() { return _vcd.GetVideoReels(); }
	const vector<CdTrack>& GetDiscTracks() { return _vcd.GetTracks(); }
	CdImageFile& GetDiscImage() { return _vcd.GetImage(); }

	//Walking the disc's own menu, once a frame. Nothing in the machine does this - on the
	//hardware it is the drive's player that reads the descriptor, shows each still and takes
	//the keys - so it is driven from outside the guest, like the picture itself.
	void ClockDiscMenu()
	{
		//Only while the machine is on the player's side. The disc paired with the ROM is
		//found again every time the machine is reset - which is what leaving the player does -
		//so without this the menu would come back up over the computer's own screen, having
		//been ejected a moment earlier.
		if(!_discChecked || !_vcdMode) {
			return;
		}

		//Time passing for whatever the drive was given, whether or not this disc has a menu -
		//the program asks the drive how much of it is left. What one frame is worth comes from
		//the console rather than from a constant, since these machines run at two rates.
		double fps = _console->GetFps();
		_vcd.TickPlayback(1.0 / (fps > 1 ? fps : 50.0));

		if(!_vcd.HasMenu()) {
			return;
		}

		YuxingVcdMenu& menu = _vcd.GetMenu();
		ReadMenuKeys(menu);

		//An opening screen has had its turn once its picture has been up for as long as the
		//disc gives it, which is when the disc means the machine to move on to what it is
		//really offering.
		//
		//How long that is comes from the item itself - its allocation, at the disc's 75
		//sectors a second - rather than from whoever happens to be showing it. Asking the
		//decoder in here would have answered exactly, and asking a player outside this
		//process cannot answer at all; taking it from the disc answers the same for both, and
		//is the same measurement the decoder would be timing against anyway.
		//A picture the menu has just asked for starts its turn; one that is part way through
		//has its turn counted down; one whose turn is over lets the list move on if it is a
		//screen that moves on by itself. Asking for the picture first is what gives an opening
		//screen its turn at all - counted the other way round, its time was up before it had
		//been asked for, and it never appeared.
		uint32_t sectors = _vcd.ShowMenuStill();
		if(sectors > 0) {
			_menuStillFrames = (uint32_t)(sectors / CdSegmentIndex::SectorsPerSecond * (fps > 1 ? fps : 50.0));
		} else if(_menuStillFrames > 0) {
			_menuStillFrames--;
		} else {
			menu.TimeOut();
		}
	}

	//The keys the menu answers to. The numbers are the ones printed beside each entry on the
	//still, which is how these menus are meant to be used and the only part of it that can be
	//seen: there is no pointer on the screen yet, so moving a hidden selection about with the
	//arrows shows nothing until one is drawn. A list too long for one screen draws a button
	//for the page after it, which is the list's own way out rather than an entry on it, so it
	//is taken by a page key instead of by a number.
	void ReadMenuKeys(YuxingVcdMenu& menu)
	{
		shared_ptr<YuxingKeyboard> keyboard = _console->GetControlManager()->GetControlDevice<YuxingKeyboard>();
		if(!keyboard) {
			return;
		}

		static constexpr YuxingKeyboard::Buttons digits[9] = {
			YuxingKeyboard::Num1, YuxingKeyboard::Num2, YuxingKeyboard::Num3,
			YuxingKeyboard::Num4, YuxingKeyboard::Num5, YuxingKeyboard::Num6,
			YuxingKeyboard::Num7, YuxingKeyboard::Num8, YuxingKeyboard::Num9
		};
		static constexpr YuxingKeyboard::Buttons pad[9] = {
			YuxingKeyboard::Numpad1, YuxingKeyboard::Numpad2, YuxingKeyboard::Numpad3,
			YuxingKeyboard::Numpad4, YuxingKeyboard::Numpad5, YuxingKeyboard::Numpad6,
			YuxingKeyboard::Numpad7, YuxingKeyboard::Numpad8, YuxingKeyboard::Numpad9
		};

		bool chosen = false;
		for(uint32_t i = 0; i < 9; i++) {
			if(MenuKeyPressed(keyboard, digits[i]) || MenuKeyPressed(keyboard, pad[i])) {
				chosen = menu.Number(i + 1);
				break;
			}
		}

		if(MenuKeyPressed(keyboard, YuxingKeyboard::Up) || MenuKeyPressed(keyboard, YuxingKeyboard::Left)) {
			menu.Move(-1);
		}
		if(MenuKeyPressed(keyboard, YuxingKeyboard::Down) || MenuKeyPressed(keyboard, YuxingKeyboard::Right)) {
			menu.Move(1);
		}
		if(MenuKeyPressed(keyboard, YuxingKeyboard::PageDown)) {
			menu.NextPage();
		}
		if(MenuKeyPressed(keyboard, YuxingKeyboard::PageUp)) {
			menu.PrevPage();
		}
		if(MenuKeyPressed(keyboard, YuxingKeyboard::Esc) || MenuKeyPressed(keyboard, YuxingKeyboard::Backspace)) {
			menu.Leave();
		}

		if(chosen || MenuKeyPressed(keyboard, YuxingKeyboard::Enter) ||
			MenuKeyPressed(keyboard, YuxingKeyboard::NumpadEnter) ||
			MenuKeyPressed(keyboard, YuxingKeyboard::Space)) {
			uint32_t program = menu.Enter();
			if(program != YuxingVcdMenu::Nowhere) {
				StartMenuProgram(program);
			}
		}
	}

	//Held keys are not repeats: a menu key counts once, where it goes down. Without this a
	//key held for the tenth of a second a person holds one for walks the whole way down a
	//menu tree.
	bool MenuKeyPressed(shared_ptr<YuxingKeyboard>& keyboard, YuxingKeyboard::Buttons key)
	{
		bool down = keyboard->IsPressed((uint8_t)key);
		bool was = _menuKeyHeld[key];
		_menuKeyHeld[key] = down;
		return down && !was;
	}

	//What the menu was for: the program behind the entry that was chosen. The machine is
	//started again on it, which is what picking one out of the disk list does - so the two
	//ways in end up in the same place.
	void StartMenuProgram(uint32_t index)
	{
		if(!_vcd.SelectProgram(index)) {
			return;
		}
		_discChecked = true;
		_persistedProgramIndex = (int32_t)index;
		_persistedDiscRom = _emu->GetRomInfo().RomFile.GetFilePath();
		_persistedDiscPath = _vcd.GetDiscFilename();
		MessageManager::Log("[YuXing] Menu chose entry " + std::to_string(index + 1) + ": " +
			_vcd.GetProgramName(index));
		_emu->GetSystemActionManager()->PowerCycle();
	}

	//Whether the drive is still the thing the screen belongs to. A picture off a disc stays
	//up until something takes it down, and a still stays up indefinitely, so leaving the
	//player - by ejecting, or by any other way out of that mode - has to be able to say so.
	bool IsPlayerShowing() { return _vcdMode && _vcd.HasDisc() && _vcd.IsPictureShown(); }

	//Where the disc's program has asked for its pointer - see YuxingVcdDrive::GetPointer
	bool GetDiscPointer(double& x, double& y) { return _vcd.GetPointer(x, y); }

	//Which audio channels the disc's program asked to hear - see YuxingVcdDrive
	uint8_t GetDiscAudioChannels() { return _vcd.GetAudioChannels(); }

	//Which cursor the disc's program is asking for - see CdVideoPlayer::DrawPointer
	uint8_t GetDiscPointerShape() { return _vcd.GetPointerShape(); }

	//A video the disc's own program has asked to show. The drive decides what is worth
	//handing over; this is only the way out to the front end - see NesConsole.
	bool TakeVideoPlayRequest(uint8_t& track, uint32_t& lba, uint32_t& sectors)
	{
		return _vcd.TakePlayRequest(track, lba, sectors);
	}

	void EndVideoPlayback(bool completed) { _vcd.EndPlayback(completed); }

	string GetCurrentDiskFilename()
	{
		if(!_fdc.IsDiskInserted() && _vcd.GetProgramIndex() >= 0) {
			return _vcd.GetProgramName((uint32_t)_vcd.GetProgramIndex());
		}
		return _fdc.IsDiskInserted() ? _fdc.GetDiskFilename() : _vcd.GetDiscFilename();
	}

	//Ejecting also leaves VCD mode, which is how the machine gets to its learning-machine
	//side: the BIOS's own "computer key" prompt is answered over a link the emulated
	//keyboard cannot reach, so the reference emulator escapes with a host key combination.
	void EjectDisc()
	{
		auto lock = _emu->AcquireLock();
		if(_fdc.IsDiskInserted()) {
			MessageManager::DisplayMessage("YuXing", "Disk ejected: " + FolderUtilities::GetFilename(_fdc.GetDiskFilename(), true));
			_fdc.EjectDisk(); //Saves pending changes first
		}
		//A disc whose programs are catalogued but none started is still a disc in the drive,
		//and it is the state the machine boots into - so eject has to test for that, not for
		//a running program
		if(_vcd.HasDisc()) {
			MessageManager::DisplayMessage("YuXing", "Disc ejected: " + FolderUtilities::GetFilename(_vcd.GetDiscFilename(), true));
			_vcd.EjectDisc();
		}
		_persistedDiscRom.clear();
		_persistedDiscPath.clear();
		_persistedProgramIndex = -1;
		_persistedFloppyRom.clear();
		_persistedFloppyPath.clear();

		if(_vcdMode) {
			//$5002 bit 1 tells the BIOS to come up as a learning machine instead. A soft
			//reset re-runs its boot code with the new value; the mapper keeps its state
			//across that, so the mode stays off.
			_vcdMode = false;
			_reg5002 = 2;
			UpdateMouseMode();
			MapRom32k(0);
			_emu->GetSystemActionManager()->Reset();
			MessageManager::DisplayMessage("YuXing", "Switched to computer mode");
		}
	}

	void InsertDisc(uint32_t index)
	{
		auto lock = _emu->AcquireLock();
		//Picking one of the programs on the mounted disc, rather than a different medium
		if(_vcd.GetProgramCount() > 0) {
			if(_vcd.SelectProgram(index)) {
				_discChecked = true;
				_persistedProgramIndex = (int32_t)index;
				_persistedDiscRom = _emu->GetRomInfo().RomFile.GetFilePath();
				_persistedDiscPath = _vcd.GetDiscFilename();
				MessageManager::DisplayMessage("YuXing", "Program: " + _vcd.GetProgramName(index));
				_emu->GetSystemActionManager()->PowerCycle();
			}
			return;
		}

		vector<string> media = GetDiskFileList();
		if(index >= media.size()) {
			return;
		}
		string path = media[index];

		if(IsFloppyImage(path)) {
			//A floppy can be changed while the machine runs - the controller raises its
			//disk-change line and the BIOS picks the new disk up on its next access
			_fdc.SaveDiskImage();
			if(_fdc.LoadDiskImage(path)) {
				_discChecked = true;
				_persistedFloppyRom = _emu->GetRomInfo().RomFile.GetFilePath();
				_persistedFloppyPath = path;
				MessageManager::DisplayMessage("YuXing", "Disk inserted: " + FolderUtilities::GetFilename(path, true));
			}
			return;
		}

		if(_vcd.LoadDisc(path)) {
			_discChecked = true;
			//A different disc has its own programs - carrying the last disc's choice over
			//would land on an unrelated one
			_persistedProgramIndex = -1;
			_persistedDiscRom = _emu->GetRomInfo().RomFile.GetFilePath();
			_persistedDiscPath = path;
			MessageManager::DisplayMessage("YuXing", "Disc inserted: " + FolderUtilities::GetFilename(path, true));
			//Unlike a floppy, a VCD disc is only read while the player side is running, and
			//the BIOS decides which side that is at boot. The reference emulator resets the
			//machine when a disc is dropped on it, so do the same.
			_emu->GetSystemActionManager()->PowerCycle();
		}
	}

	void InsertNextDisc()
	{
		vector<string> media = GetDiskFileList();
		if(media.empty()) {
			return;
		}

		string current = GetCurrentDiskFilename();
		int32_t index = -1;
		for(size_t i = 0; i < media.size(); i++) {
			if(media[i] == current) {
				index = (int32_t)i;
				break;
			}
		}
		InsertDisc((index + 1) % media.size());
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SV(_keyRowMask); SV(_reg5002); SV(_reg4800); SV(_reg5500); SV(_reg5501);
		SV(_reg8000); SV(_mmc3Mode); SV(_cramLoaded); SV(_vcdMode); SV(_vcdKeyboardSelected);
		SV(_mmc3Cmd); SV(_mmc3Prg0); SV(_mmc3Prg1);
		SV(_mmc3Chr01); SV(_mmc3Chr23); SV(_mmc3Chr4); SV(_mmc3Chr5); SV(_mmc3Chr6); SV(_mmc3Chr7);
		SV(_mmc3IrqLatch); SV(_mmc3IrqCounter); SV(_mmc3IrqPreset); SV(_mmc3IrqPresetVbl);
		SV(_mmc3IrqEnable); SV(_lastPpuScanline); SV(_lastBandScanline); SV(_lastSplitBand);
		SV(_xtOut); SV(_xtEnabled); SV(_xtLsb); SV(_xtMsb); SV(_xtB1); SV(_xtB2);
		SV(_xtScan); SV(_xtScanPrev);
		SV(_lpcReceiving); SV(_lpcNibbleCount); SV(_lpcByte);
		SV(_lpcAudio);
		SV(_printer); SV(_lptLast); SV(_lptShift);
		SV(_fdc);
		_vcd.Serialize(s);

		if(!s.IsSaving()) {
			//The PPU and the mouse both cache mapper state - put it back after a state load
			UpdateSplitMode();
			UpdateMouseMode();
		}
	}
};
