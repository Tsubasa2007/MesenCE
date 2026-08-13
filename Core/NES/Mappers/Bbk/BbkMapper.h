#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/BaseNesPpu.h"
#include "NES/NesConsole.h"
#include "NES/NesCpu.h"
#include "NES/Mappers/Bbk/BbkFdc.h"
#include "NES/Mappers/Bbk/BbkLpcAudio.h"
#include "NES/Mappers/Bbk/BbkPrinter.h"
#include "Shared/MessageManager.h"
#include "Shared/NotificationManager.h"
#include "Shared/Interfaces/INotificationListener.h"
#include "Shared/Video/DebugHud.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/Serializer.h"

//BBK (步步高) learning machine - iNES mapper 171 in the VirtuaNES-BBK convention.
//Ported from the VirtuaNES-BBK fork (NES/Mapper/MapperBBK.cpp, author fanoble).
//
//Hardware emulated:
// - Inno ASIC (memory controller): 128KB BIOS ROM + 512KB DRAM banked into the CPU space
//   through registers $FF04/$FF14/$FF1C/$FF24/$FF2C; $FF01 bit 3 flips $C000-$FFFF
//   between BIOS and DRAM ("map RAM" mode); $FF01 bit 4 + PPU shadow mirrors
//   $2000-$3FFF writes into DRAM.
// - Holtek ASIC (video controller): 32KB CHR-RAM banked in 1K/2K pages
//   ($FF03/$FF0B/$FF13/$FF1B, $FF23-$FF5B), scanline split engine with queued
//   scanline counts + CHR banks ($FF0A/$FF1A/$FF12/$FF22), line counter IRQ.
// - LPC-10 speech synthesizer ($FF10/$FF18) - see BbkLpcAudio.
// - uPD765 floppy controller at $FF80-$FFB8 - see BbkFdc. A disk image named
//   "<rom name>.img" next to the ROM is mounted automatically.
// - Parallel port printer ($FF40/$FF48/$FF50) - see BbkPrinter. Pages come out as PNGs
//   in the screenshot folder.
//
//Address decode rules (from the fork's MapAddr, OPT_ADDR_MAP path):
// - $2000-$3FFF write: PPU registers; also shadowed to DRAM $7A000+ when mapRam && !ff01D4 && !(addr & 2)
// - $4000-$7FFF: DRAM $78000 + (addr & $3FFF), read/write
// - $8000-$9FFF: FF14 bit6 ? DRAM page (FF14 & $3F) : ROM page (FF14 & $0F), read-only
// - $A000-$BFFF: FF14 bit6 ? DRAM page (FF1C & $3F) : ROM page (FF1C & $0F), read-only
// - $C000-$DFFF: mapRam ? DRAM page (FF24 & $3F) : ROM $1C000
// - $E000-$FEFF: mapRam ? DRAM page (FF2C & $3F) : ROM $1E000
// - $FF00-$FFFF write: always IO registers
// - $FF00-$FFFF read: mapRam ? DRAM : ((addr & 7) == 0 ? IO : ROM)
//
//BBK-98 chipset revision (detected by PRG size > 128K; the BIOS is a 2MB flash chip
//holding a FAT12 "electronic disk" plus the system code in its top banks):
// - $FF04: D7 selects DRAM/ROM at $8000-$BFFF (was D5), D6-D0 = 16K page over the
//   full 2MB ROM; the fixed $C000-$FFFF region maps the last 16K of ROM
// - 1MB DRAM with the fixed $4000-$7FFF window at 16K bank $3E, and 256K CHR-RAM
//   (the CHR page registers are 8 bits wide here, not 5)
// - $FF09 bit 6 is the top DRAM address line, above the page registers' 7 bits. The
//   BIOS sizes memory by running its probe with that line clear and again with it set,
//   then ADDING the two results ($94E1), so the half that is not fitted has to read as
//   absent - folding it back onto the populated half reports twice the real size.
// - $FF11 bit 7 maps DRAM over $C000-$FFFF here, taking that job over from $FF01 bit 3
//   (a BIOS-call nesting counter, cleared by writing $FF19, so a call always runs with
//   the ROM swapped back in); $FF09 bit 1 exposes the IO registers to reads while
//   that overlay is active
// - Interrupt controller: $FF08 bit 5 + $FF01 bit 2 arm a vblank-start IRQ (the BIOS
//   drains its VRAM upload queue there since the Dendy NMI fires too late at line 291);
//   $FFB0 read = pending source id (6) and acknowledges, $FFB0 write = mask,
//   $FF98 write = EOI (restores $FF09)
class BbkMapper : public BaseMapper
{
private:
	//Handles the FDS disk shortcut keys (eject / insert next / insert number) for disk swapping
	class DiskSwapListener final : public INotificationListener
	{
	private:
		BbkMapper* _mapper;

	public:
		DiskSwapListener(BbkMapper* mapper) : _mapper(mapper) {}

		void ProcessNotification(ConsoleNotificationType type, void* parameter) override
		{
			if(type == ConsoleNotificationType::ExecuteShortcut) {
				ExecuteShortcutParams* params = (ExecuteShortcutParams*)parameter;
				switch(params->Shortcut) {
					case EmulatorShortcut::FdsEjectDisk: _mapper->EjectDisk(); break;
					case EmulatorShortcut::FdsInsertNextDisk: _mapper->InsertNextDisk(); break;
					case EmulatorShortcut::FdsInsertDiskNumber: _mapper->InsertDisk(params->Param); break;
					default: break;
				}
			}
		}
	};

	BbkFdc _fdc;
	unique_ptr<BbkLpcAudio> _lpcAudio;
	BbkPrinter _printer;
	bool _printerNamed = false;
	shared_ptr<DiskSwapListener> _swapListener;

	//Inno ASIC registers
	uint8_t _regFF14 = 0;
	uint8_t _regFF1C = 0;
	uint8_t _regFF24 = 0;
	uint8_t _regFF2C = 0;
	bool _mapRam = false;
	bool _ff01D4 = false;

	//BBK-98 "electronic disk" model: the ROM is a 2MB flash chip (BIOS in the last 16K+banks,
	//the rest is a FAT12 filesystem). Its Inno revision widens $FF04: bit 7 selects DRAM
	//(instead of bit 5) and bits 0-6 are a 16K ROM bank covering the full 2MB. Detected by
	//ROM size; the classic machines only ever shipped 128K BIOSes.
	bool _bbk98 = false;
	uint8_t _romBank16k = 0;
	bool _dram8000 = false;
	//BBK-98 $C000-$FFFF DRAM overlay: enabled by $FF11 bit 7 (a BIOS-call nesting counter),
	//cleared by any $FF19 write. While the overlay is on, $FFxx reads hit DRAM instead of the
	//IO registers unless $FF09 bit 1 is set - the BIOS sets it at boot and leaves it set, so
	//software can read $FF18/$FFB0 while running with the overlay active.
	uint8_t _regFF09 = 0;
	uint8_t _regFF11 = 0;
	//A raster IRQ exposes the IO registers to the handler for the duration of the interrupt
	//even if $FF09 bit 1 is clear; the $FF98 EOI ends that. Tracked separately instead of
	//forcing the bit into $FF09 and restoring the old value at the EOI: the BIOS writes $FF98
	//at the end of every interrupt, including ones this chip did not raise, and restoring a
	//stale value there wiped the bit the BIOS had set at boot - after which the speech-status
	//poll ($FF18, read with only $FF01 D3 cleared) read DRAM instead of the LPC and hung.
	bool _ff09IrqIoOverride = false;

	//BBK-98 interrupt controller. On Dendy timing the NMI is delayed to scanline 291,
	//so this chipset revision adds a vblank-start IRQ the BIOS uses for its VRAM upload
	//queue: armed via $FF08 bit 5 + $FF01 bit 2, source id read from $FFB0 (bit 7 =
	//nothing pending, low bits = source, 6 = this raster IRQ), acknowledged via $FF98.
	uint8_t _regFF08 = 0;
	uint8_t _regFFB0 = 0;
	bool _lineIrqPending = false;

	//Holtek ASIC state
	bool _splitMode = false;
	bool _enableIrq = false;
	uint16_t _lineCount = 0;
	uint8_t _nrOfSR = 0;
	uint8_t _nrOfVR = 0;
	uint8_t _queueSR[64] = {};
	uint8_t _queueVR[64] = {};
	uint8_t _queueIndex = 0;

	//The BBK-98 revision double-buffers the split queue: $FF0A/$FF1A push entries into a
	//staging queue that $FF22 commits, so the ~16 bands the BIOS rewrites every frame can
	//never corrupt the bands currently on screen. $FF12 clears the live queue, $FF2A
	//restarts it from the top. Its band counter is a plain 8-bit counter reloaded from the
	//queue whenever it wraps.
	uint8_t _stageSR[64] = {};
	uint8_t _stageVR[64] = {};
	uint8_t _stageCount = 0;
	uint8_t _stageCountVR = 0;
	uint8_t _splitLine = 0;
	bool _renderEnabled = false;

	//BBK-98 CHR bank registers, latched until the split engine is off (see UpdateChrBanks98)
	uint8_t _chrReg[8] = {};

	int32_t _lastPpuScanline = -2;
	bool _irqPending = false;
	bool _irqApplied = true;
	bool _diskChecked = false;

	//MMC3-clone banking mode ($FF01 bit 5). Cartridge-style games bank $8000-$BFFF through
	//a private MMC3 register file mapped into DRAM (not the plain $FF04/$FF14 windows), with
	//an MMC3 scanline-counter IRQ. Ported from the reference emulator's MapperBBK2 MMC3 path.
	bool _mmc3Mode = false;
	uint8_t _mmc3Cmd = 0;
	uint8_t _mmc3Prg0 = 0;
	uint8_t _mmc3Prg1 = 0;
	uint8_t _mmc3Chr01 = 0, _mmc3Chr23 = 2, _mmc3Chr4 = 4, _mmc3Chr5 = 5, _mmc3Chr6 = 6, _mmc3Chr7 = 7;
	uint8_t _mmc3IrqLatch = 0xFF, _mmc3IrqCounter = 0, _mmc3IrqPreset = 0, _mmc3IrqPresetVbl = 0;
	bool _mmc3IrqEnable = false;
	uint8_t _regFF3C = 0;
	//PRG/CHR page masks: PRAM from DRAM size (0x7F for the 1MB BBK-98), YPRAM/YCRAM from $FF3C
	uint8_t _pramMask = 0x7F, _ypramMask = 0x3F, _ycramMask = 0x7F;

	//A power cycle recreates the mapper, so the inserted disk is remembered here (like a floppy
	//physically staying in the drive) and re-mounted instead of the paired boot disk. Scoped to
	//the ROM path so a different game doesn't inherit a stale selection.
	inline static string _persistedDiskRom;
	inline static string _persistedDiskPath;

	//8K DRAM page mask: 6 bits (512K) on the classic models, 7 bits on the BBK-98 - whose
	//page registers only carry the low 7 bits, with $FF09 bit 6 supplying the top DRAM
	//address line (the BIOS sizes memory by probing with that line both ways, so anything
	//past the fitted 1MB has to fold back onto it)
	uint8_t DramBankMask() { return _bbk98 ? 0x7F : 0x3F; }

	uint8_t DramPage(uint8_t reg)
	{
		//$FF09 bit 6 sits above the register's 7 bits
		return _bbk98 ? (uint8_t)((reg & 0x7F) | ((_regFF09 & 0x40) << 1)) : (uint8_t)(reg & 0x3F);
	}

	//True when the selected page is past the DRAM actually fitted. The BIOS sizes memory by
	//probing with $FF09's top address line both ways and ADDING the two results, so the
	//unpopulated half has to read as absent rather than fold back onto the populated one.
	bool IsDramPageAbsent(uint8_t page) { return page * 0x2000u >= _workRamSize; }

	//Maps an 8K DRAM window, leaving it unmapped when that page is not populated
	void MapDramPage(uint16_t start, uint16_t end, uint8_t page)
	{
		if(IsDramPageAbsent(page)) {
			RemoveCpuMemoryMapping(start, end);
		} else {
			SetCpuMemoryMapping(start, end, page, PrgMemoryType::WorkRam, MemoryAccessType::ReadWrite);
		}
	}

	//Offset of the fixed $4000-$7FFF DRAM window: 16K bank $3E on the 98, $1E otherwise
	uint32_t FixedDramWindow() { return (_bbk98 ? 0x3E : 0x1E) * 0x4000; }

	//DRAM vs ROM at $8000-$BFFF: the classic models encode the selector in _regFF14 bit 6
	//(original $FF04 D5, shifted left); the 98 moved it to $FF04 D7, tracked separately
	//because the shift pushes it out of the 8-bit bank value
	bool DramAt8000() { return _bbk98 ? _dram8000 : (_regFF14 & 0x40) != 0; }

	void UpdatePrgBank8000()
	{
		//In MMC3-clone mode the whole $8000-$FFFF map is owned by Mmc3Sync
		if(_mmc3Mode) { return; }
		if(DramAt8000()) {
			MapDramPage(0x8000, 0x9FFF, DramPage(_regFF14));
		} else if(_bbk98) {
			SelectPrgPage(0, _romBank16k * 2);
		} else {
			SelectPrgPage(0, _regFF14 & 0x0F);
		}
	}

	void UpdatePrgBankA000()
	{
		if(_mmc3Mode) { return; }
		//The DRAM/ROM selector for $A000-$BFFF is $FF14's bit 6 (not $FF1C's)
		if(DramAt8000()) {
			MapDramPage(0xA000, 0xBFFF, DramPage(_regFF1C));
		} else if(_bbk98) {
			SelectPrgPage(1, _romBank16k * 2 + 1);
		} else {
			SelectPrgPage(1, _regFF1C & 0x0F);
		}
	}

	//DRAM at $C000-$FFFF: classic models use $FF01 bit 3; the 98 replaced it with $FF11 bit 7.
	//The distinction is load-bearing: the BIOS call gate posts the function number to $FF19,
	//which clears $FF11 and so swaps the ROM back in for the duration of the call - the
	//dispatcher the gate jumps to only exists in ROM. Letting $FF01 bit 3 hold the overlay up
	//as well leaves every call after software sets that bit landing on whatever the RAM copy
	//happens to hold at the same address.
	bool DramAtC000() { return _bbk98 ? (_regFF11 & 0x80) != 0 : _mapRam; }

	void UpdatePrgBankC000()
	{
		//$C000-$FFFF stays overlay/BIOS-controlled even in MMC3 mode
		if(DramAtC000()) {
			MapDramPage(0xC000, 0xDFFF, DramPage(_regFF24));
		} else {
			//Last 16K of ROM (== the fork's fixed $1C000 for a 128K BIOS)
			SelectPrgPage(2, GetPrgPageCount() - 2);
		}
	}

	void UpdatePrgBankE000()
	{
		//$FF00-$FFFF is additionally covered by the register handlers
		if(DramAtC000()) {
			MapDramPage(0xE000, 0xFFFF, DramPage(_regFF2C));
		} else {
			SelectPrgPage(3, GetPrgPageCount() - 1);
		}
	}

	void SelectChrPage2k(uint8_t slot2k, uint8_t page2k)
	{
		uint16_t mask = _bbk98 ? 0xFF : 0x1F;
		SelectChrPage(slot2k * 2, (page2k * 2) & mask);
		SelectChrPage(slot2k * 2 + 1, (page2k * 2 + 1) & mask);
	}

	//The BBK-98's CHR bank registers are latched rather than applied on the spot: the video
	//controller only copies them into the 1K slots while the split engine is off. In split mode
	//the queue owns $0000-$0FFF and $1000-$1FFF keeps whatever was selected before it was turned
	//on - applying the registers there instead would fight the queue and scramble the banded
	//screens the disk software draws (the BIOS turns split off around its CHR-RAM uploads).
	void UpdateChrBanks98()
	{
		if(_splitMode) {
			return;
		}

		for(int i = 0; i < 8; i++) {
			SelectChrPage(i, _chrReg[i]);
		}
	}

	//$FF03/$FF0B/$FF13/$FF1B: 2K page for a pair of 1K slots
	void WriteChrReg2k(uint8_t slot2k, uint8_t value)
	{
		if(!_bbk98) {
			SelectChrPage2k(slot2k, value);
			return;
		}

		_chrReg[slot2k * 2] = (uint8_t)(value * 2);
		_chrReg[slot2k * 2 + 1] = (uint8_t)(value * 2 + 1);
		UpdateChrBanks98();
	}

	//$FF23-$FF5B: 1K page (8-bit registers on the BBK-98, 5-bit on the classic models)
	void WriteChrReg1k(uint8_t slot1k, uint8_t value)
	{
		if(!_bbk98) {
			SelectChrPage(slot1k, value & 0x1F);
			return;
		}

		_chrReg[slot1k] = value;
		UpdateChrBanks98();
	}

	//Applies the split queue entry at _queueIndex (scanline count + two 2K CHR banks for $0000-$0FFF)
	void ApplySplitEntry()
	{
		_lineCount = _queueSR[_queueIndex];

		uint8_t page = _queueVR[_queueIndex] & 0x0F;
		SelectChrPage(0, page * 2);
		SelectChrPage(1, page * 2 + 1);

		page = (_queueVR[_queueIndex] >> 4) & 0x0F;
		SelectChrPage(2, page * 2);
		SelectChrPage(3, page * 2 + 1);

		_queueIndex++;
	}

	bool EvaluateIrq()
	{
		if(_lineCount == 254 && _splitMode) {
			return _queueIndex == _nrOfSR && _enableIrq;
		}
		if(_lineCount == 254 && !_splitMode) {
			return _enableIrq;
		}
		return false;
	}

	bool CheckIrq()
	{
		if(EvaluateIrq() || (_bbk98 && _lineIrqPending)) {
			_console->GetCpu()->SetIrqSource(IRQSource::External);
			return true;
		}
		_console->GetCpu()->ClearIrqSource(IRQSource::External);
		return false;
	}

	//Called once per scanline (at display row N's hblank with argument N);
	//line numbering follows the reference emulator: 0 = dummy line, 1-239 = visible, 240+ = vblank.
	//The IRQ line state is evaluated here (before the counter increments, like the
	//original) but only applied later in the line - see ProcessCpuClock.
	//Applies the live queue entry at _queueIndex on the BBK-98 (two 2K CHR banks packed in
	//one byte, plus the reload value for the band counter)
	void ApplySplitEntry98()
	{
		uint8_t idx = _queueIndex & 0x3F;
		uint8_t banks = _queueVR[idx];
		SelectChrPage2k(0, banks & 0x0F);
		SelectChrPage2k(1, (banks >> 4) & 0x0F);
		_splitLine = _queueSR[idx];
		_queueIndex++;
	}

	void RaiseLineIrq()
	{
		_lineIrqPending = true;
		//Expose the IO registers to the handler even if the $FF11 DRAM overlay is active
		//(ended by the $FF98 EOI)
		_ff09IrqIoOverride = true;
		_console->GetCpu()->SetIrqSource(IRQSource::External);
	}

	//The 98 revision's raster engine. In split mode the band counter is reloaded from the
	//queue each time it wraps and the frame IRQ fires at the end of the visible area (the
	//BIOS drains its VRAM upload queue there, since the Dendy NMI arrives too late at
	//scanline 291); otherwise the counter free-runs over the rendered lines.
	void HSync98(int32_t scanline)
	{
		if(_splitMode) {
			if(scanline >= 240) {
				//Vblank restarts the sequence for the next frame
				_queueIndex = 0;
				_splitLine = 0;
			} else {
				_splitLine++;
			}

			if(_splitLine == 0 && _queueIndex < _nrOfSR && _queueIndex < _nrOfVR) {
				ApplySplitEntry98();
			}

			if(scanline == 239 && _enableIrq && !_lineIrqPending) {
				RaiseLineIrq();
			}
		} else if(scanline < 240 && _renderEnabled) {
			_splitLine++;
			if(_splitLine == 0 && _enableIrq && !_lineIrqPending) {
				RaiseLineIrq();
			}
		}
	}

	//===== MMC3-clone mode ($FF01 bit 5) =====
	//The switchable PRG banks pass through FixPrg (small game bank numbers 0x30-0x3F relocate
	//into the DRAM handler region at +0x70=112); every bank is then masked by PRAM_mask.
	uint8_t Mmc3SetPrg(uint8_t v) { return v & _pramMask; }
	uint8_t Mmc3SetChr(uint8_t v) { return v & _ycramMask; }
	uint8_t Mmc3FixPrg(uint8_t v)
	{
		if((v & _ypramMask) == (uint8_t)(_ypramMask - 3)) return 0;
		if((v & _ypramMask) == (uint8_t)(_ypramMask - 2)) return 1;
		return (uint8_t)((v & _ypramMask) + (_pramMask - _ypramMask));
	}

	//Map the four 8K PRG banks + eight 1K CHR banks per the current MMC3 register state
	void Mmc3Sync()
	{
		bool pwrap = (_mmc3Cmd & 0x40) != 0;
		bool cwrap = (_mmc3Cmd & 0x80) != 0;
		//MMC3 only banks $8000-$BFFF. $C000-$FFFF stays under the BBK BIOS/overlay control
		//(the games run with the $C000-$FFFF DRAM overlay active there); remapping it here
		//would yank the ground from under the BIOS game-start code that is still executing at
		//$E000-$FFFF when it flips into MMC3 mode.
		MapDramPage(0x8000, 0x9FFF, Mmc3SetPrg(pwrap ? 0xFE : _mmc3Prg0));
		MapDramPage(0xA000, 0xBFFF, Mmc3SetPrg(_mmc3Prg1));

		uint8_t c[8] = {
			Mmc3SetChr((uint8_t)(_mmc3Chr01 + 0)), Mmc3SetChr((uint8_t)(_mmc3Chr01 + 1)),
			Mmc3SetChr((uint8_t)(_mmc3Chr23 + 0)), Mmc3SetChr((uint8_t)(_mmc3Chr23 + 1)),
			Mmc3SetChr(_mmc3Chr4), Mmc3SetChr(_mmc3Chr5), Mmc3SetChr(_mmc3Chr6), Mmc3SetChr(_mmc3Chr7)
		};
		for(int i = 0; i < 8; i++) {
			SelectChrPage((uint16_t)(cwrap ? ((i + 4) & 7) : i), c[i]);
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
					case 6: _mmc3Prg0 = Mmc3FixPrg(value); break;
					case 7: _mmc3Prg1 = Mmc3FixPrg(value); break;
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

	//Per-scanline IRQ counter (not A12-based), evaluated once per visible line while rendering
	void Mmc3IrqSync(int32_t scanline)
	{
		if(scanline >= 0 && scanline <= 239 && _renderEnabled) {
			if(_mmc3IrqPresetVbl) { _mmc3IrqCounter = _mmc3IrqLatch; _mmc3IrqPresetVbl = 0; }
			if(_mmc3IrqPreset) { _mmc3IrqCounter = _mmc3IrqLatch; _mmc3IrqPreset = 0; }
			else if(_mmc3IrqCounter > 0) { _mmc3IrqCounter--; }
			if(_mmc3IrqCounter == 0) {
				if(_mmc3IrqEnable) { _console->GetCpu()->SetIrqSource(IRQSource::External); }
				_mmc3IrqPreset = 0xFF;
			}
		}
	}

	//Reset the MMC3 register file to its power-on defaults (FixPrg(0)/FixPrg(1) already point
	//$8000/$A000 at the DRAM handler region, e.g. page 0x70=112 on the BBK-98)
	void Mmc3Reset()
	{
		_mmc3Cmd = 0;
		_mmc3Prg0 = Mmc3FixPrg(0);
		_mmc3Prg1 = Mmc3FixPrg(1);
		_mmc3Chr01 = 0; _mmc3Chr23 = 2; _mmc3Chr4 = 4; _mmc3Chr5 = 5; _mmc3Chr6 = 6; _mmc3Chr7 = 7;
		_mmc3IrqEnable = false;
		_mmc3IrqCounter = 0;
		_mmc3IrqLatch = 0xFF;
		_mmc3IrqPreset = 0;
		_mmc3IrqPresetVbl = 0;
	}

	void HSync(int32_t scanline)
	{
		if(_mmc3Mode) {
			Mmc3IrqSync(scanline);
			return;
		}

		if(_bbk98) {
			HSync98(scanline);
			return;
		}

		//Restart the split sequence at the top of each frame while IRQ handling is pending
		if(scanline == 0 && _splitMode) {
			//Continue using the settings of the last frame, but only while the queue actually
			//holds entries - $FF12 empties it, and re-applying a stale entry would re-point the
			//CHR banks at the start of every frame, overriding the ones the bank registers
			//select and leaving the screen showing whichever pages the previous program used.
			_queueIndex = 0;
			if(_nrOfSR > 0) {
				ApplySplitEntry();
			}
		}

		if(scanline >= 240) {
			return;
		}

		//The counter itself free-runs while the display is off (below), but the line IRQ must
		//not be raised then: software blanks the screen for its CHR/nametable uploads and an
		//IRQ taken in the middle of one pre-empts it at a scanline it never expects.
		_irqPending = EvaluateIrq() && _renderEnabled;
		_irqApplied = false;

		if(_lineCount == 255) {
			//Only the split engine repoints the CHR banks. With split mode off the counter is
			//just a raster IRQ timer, and a queue left behind by an earlier program must not be
			//walked: its entries would land on top of the bank registers the current program
			//selected, one entry every time the IRQ counter wraps, until the queue runs out.
			if(_splitMode && _queueIndex != _nrOfSR) {
				ApplySplitEntry();
			}
		} else if(_splitMode || _enableIrq) {
			//The classic Holtek counter free-runs over the visible lines whether or not the
			//display is on (MapperBBK::HSync in the reference has no IsDispON gate - only the
			//98's MapperBBK2 does): software blanks the screen for its uploads and still expects
			//the line IRQ it armed to arrive on schedule.
			_lineCount++;
		}
	}

	//User-configured folder to scan for floppy images, or "" to use the game's own folder
	string GetConfiguredDiskFolder()
	{
		const char* folder = _console->GetNesConfig().BbkDiskFolder;
		return folder[0] ? string(folder) : string();
	}

	//Mounts "<rom name>.img" found next to the ROM (or in the configured disk folder), if
	//present. Deferred until first FDC access so the emulator's rom info is fully set up.
	void CheckForDiskImage()
	{
		if(_diskChecked) {
			return;
		}
		_diskChecked = true;

		string romPath = _emu->GetRomInfo().RomFile.GetFilePath();
		string baseName = FolderUtilities::GetFilename(romPath, false);

		//A .img opened directly from the UI boots the BIOS and mounts that specific image,
		//taking precedence over the paired/persisted disk (taken, so it only applies to this boot).
		string pending = BbkFdc::TakePendingBootDisk();
		if(!pending.empty()) {
			ifstream test(pending, ios::in | ios::binary);
			if(test) {
				test.close();
				_fdc.LoadDiskImage(pending);
				_persistedDiskRom = romPath;
				_persistedDiskPath = pending; //keep it across a power cycle, like a manually inserted disk
				MessageManager::Log("[BBK] Mounted disk image: " + pending);
				return;
			}
		}

		//A power cycle recreates the mapper: re-mount the disk the user last selected for this ROM
		//(the "floppy stays in the drive"), so power cycle reboots from it like a soft reset does.
		if(_persistedDiskRom == romPath && !_persistedDiskPath.empty()) {
			ifstream test(_persistedDiskPath, ios::in | ios::binary);
			if(test) {
				test.close();
				_fdc.LoadDiskImage(_persistedDiskPath);
				MessageManager::Log("[BBK] Re-mounted disk image: " + _persistedDiskPath);
				return;
			}
		}

		//Look next to the ROM first (paired-disk convention), then in the configured folder
		vector<string> folders = { FolderUtilities::GetFolderName(romPath) };
		string configured = GetConfiguredDiskFolder();
		if(!configured.empty() && configured != folders[0]) {
			folders.push_back(configured);
		}

		for(string& folder : folders) {
			for(string ext : { ".img", ".IMG", ".ima", ".IMA" }) {
				string diskPath = FolderUtilities::CombinePath(folder, baseName + ext);
				ifstream test(diskPath, ios::in | ios::binary);
				if(test) {
					test.close();
					_fdc.LoadDiskImage(diskPath);
					MessageManager::Log("[BBK] Mounted disk image: " + diskPath);
					return;
				}
			}
		}
	}

protected:
	uint16_t GetPrgPageSize() override { return 0x2000; }
	uint16_t GetChrPageSize() override { return 0x400; }
	//256K on the BBK-98 (its CHR bank registers are 8 bits wide), 32K on the classic models
	uint32_t GetChrRamSize() override { return _prgSize > 0x20000 ? 0x40000 : 0x8000; }
	uint16_t GetChrRamPageSize() override { return 0x400; }
	//1MB of DRAM on the BBK-98 (the BIOS finds the size by probing for the point where the
	//banks start repeating, then declares it through $5FF9), 512K on the classic models.
	//_prgSize is not set yet when this is called, so the header tells them apart.
	uint32_t GetWorkRamSize() override { return _romInfo.Header.GetPrgSize() > 0x20000 ? 0x100000 : 0x80000; }
	uint32_t GetWorkRamPageSize() override { return 0x2000; }
	bool ForceWorkRamSize() override { return true; }
	uint32_t GetSaveRamSize() override { return 0; }

	uint16_t RegisterStartAddress() override { return 0xFF00; }
	uint16_t RegisterEndAddress() override { return 0xFFFF; }
	bool AllowRegisterRead() override { return true; }
	bool EnableCpuClockHook() override { return true; }

public:
	//BBK software streams palette data during forced blanking (e.g. at page/screen
	//transitions); the machine's PPU clone doesn't exhibit the 2C02 "background
	//palette hack", so showing it here flashes color bands the real machine never showed
	bool EnablePpuPaletteBgHack() override { return false; }

	//BBK software polls $2002 inside NMI-toggling copy loops. Its clone PPU clears the vblank
	//flag on $2002 reads like a 2C02 (so the standard behaviour applies), but it does NOT have
	//the 2C02 "$2002 read one dot before vblank suppresses the NMI" race - that race would drop
	//an occasional NMI when the poll happens to hit the vblank edge, losing a frame's
	//PPUCTRL/OAM refresh (a one-frame flash at screen transitions).
	bool EnablePpuNmiSuppressRace() override { return false; }

	//A 2C02 turns a $2007 write made while rendering is active into a write of the PPU bus
	//address' own low byte, at whatever address the render pipeline currently has on the bus.
	//That behaviour is unconfirmed even for the 2C02 (Mesen derives it from Visual NES), and the
	//machine's clone PPU does not do it: BBK software drains its VRAM queue one $2006/$2006/$2007
	//triple at a time and routinely spills the last few entries past the end of vblank onto the
	//pre-render line, where the smear would drop a byte of bus address into CHR-RAM. Because CHR
	//here is RAM and nothing rewrites it, a single spilled write permanently defaces a glyph -
	//e.g. it turns the text-window space tile into a dash. Dropping the write instead leaves the
	//tile alone, which is what the machine (and the reference emulator) show.
	bool EnablePpuVramWriteGlitch() override { return false; }

	//A 2C02 drives $2005 and $2006 from a single first/second-write toggle. This machine's clone
	//PPU latches them separately and re-aligns the $2006 latch whenever a $2007 access starts a
	//data transfer. A $2002 read still clears both latches, exactly as on a 2C02.
	//
	//Why it has to work this way: the BIOS owns the NMI vector and its handler reads $2002 before
	//chaining to the program's own handler, so an NMI can split any unguarded $2006,$2006 or
	//$2005,$2005 pair - and converted software does exactly that, hundreds of times per frame,
	//while it builds a screen. Under the shared toggle one such split inverts it permanently:
	//every later address is latched high/low swapped, the writes land in CHR space, and whole
	//columns of the screen are never written. Separate latches make a split $2005 pair harmless to
	//addressing, and the $2007 re-align caps a split $2006 pair at a single bad access instead of
	//corrupting everything after it.
	//
	//Do NOT "simplify" this by having $2002 skip the $2006 latch. That also fixes the screen
	//builds, but it strands the latch out of phase for software that relies on $2002 to resync
	//after an odd number of $2006 writes - it drops half of some programs' sprite tiles.
	bool EnablePpuSharedWriteToggle() override { return false; }

	//Off by default - an experiment, NOT emulated behaviour. It makes a tile take the attribute
	//fetched for the tile column before it, which cleans up the banded-framebuffer screens on
	//some titles and corrupts them on others, with no hardware-visible difference between the
	//two cases, so it cannot be applied unconditionally. The 98 revision is excluded because it
	//shifts the colors on its desktop.
	//The PPU latches this in its constructor, which NesConsole runs before InitSpecificMapper,
	//so _bbk98 is not set yet; the header tells the revisions apart (as in GetWorkRamSize).
	bool EnablePpuAttributeLag() override
	{
		return _romInfo.Header.GetPrgSize() <= 0x20000 && _console->GetNesConfig().BbkAttributeLag;
	}

protected:

	void InitMapper(RomData& romData) override
	{
		//The FD-1 drive unit includes the keyboard/mouse; the BIOS refuses to boot from
		//disk unless the keyboard responds, so auto-connect them (like the fork does)
		romData.Info.InputType = GameInputType::SuborKeyboardMouse1;
		romData.Info.System = GameSystem::Dendy;
	}

	void InitMapper() override
	{
		//The BBK machines are Dendy-timed famiclones
		_romInfo.System = GameSystem::Dendy;

		_lpcAudio.reset(new BbkLpcAudio(_console));

		if(!_swapListener) {
			_swapListener.reset(new DiskSwapListener(this));
			_emu->GetNotificationManager()->RegisterNotificationListener(_swapListener);
		}

		//$2000-$3FFF writes are intercepted for the PPU-shadow feature (forwarded to the PPU)
#ifndef BBK_DISABLE_PPU_SHADOW
		AddRegisterRange(0x2000, 0x3FFF, MemoryOperation::Write);
#endif

		_bbk98 = _prgSize > 0x20000;

		//$4000-$7FFF (from $4100, below the APU/input registers): fixed DRAM window near the
		//top of DRAM - $F8000 on the BBK-98 (1MB region), $78000 on the classic models
		SetCpuMemoryMapping(0x4100, 0x7FFF, PrgMemoryType::WorkRam, FixedDramWindow() + 0x100, MemoryAccessType::ReadWrite);

		if(_bbk98) {
			//The 98's BIOS sizes DRAM by probing it, and its video upload builds tiles out
			//of whatever the target bank holds - both need the zero-filled power-on state
			//the hardware (and the reference emulator) provide
			memset(_workRam, 0, _workRamSize);
			memset(_chrRam, 0, _chrRamSize);

		}

		_romBank16k = 0;
		_dram8000 = false;
		_regFF09 = 0;
		_ff09IrqIoOverride = false;
		_regFF11 = 0;
		_regFF08 = 0;
		_regFFB0 = 0;
		_lineIrqPending = false;

		_regFF14 = 0;
		_regFF1C = 0;
		_regFF24 = 0;
		_regFF2C = 0;
		_mapRam = false;
		_ff01D4 = false;

		_splitMode = false;
		_enableIrq = false;
		_lineCount = 0;
		_nrOfSR = _nrOfVR = 0;
		_queueIndex = 0;
		_stageCount = _stageCountVR = 0;
		_splitLine = 0;
		memset(_stageSR, 0, sizeof(_stageSR));
		memset(_stageVR, 0, sizeof(_stageVR));
		memset(_queueSR, 0, sizeof(_queueSR));
		memset(_queueVR, 0, sizeof(_queueVR));

		_lastPpuScanline = -2;

		//MMC3-clone mode is off until a game sets $FF01 bit 5. PRAM mask follows the DRAM size
		//(0x7F for the 1MB BBK-98, 0xFF for a 2MB part); YPRAM/YCRAM come from $FF3C at entry.
		_mmc3Mode = false;
		_regFF3C = 0;
		_pramMask = (uint8_t)((_workRamSize >> 13) - 1);
		_ypramMask = 0x3F;
		_ycramMask = 0x7F;
		Mmc3Reset();

		UpdatePrgBank8000();
		UpdatePrgBankA000();
		UpdatePrgBankC000();
		UpdatePrgBankE000();

		for(int i = 0; i < 8; i++) {
			_chrReg[i] = (uint8_t)i;
			SelectChrPage(i, i);
		}

		_lpcAudio->Reset();

		_printerNamed = false;
		_printer.Reset();
	}

	//Printed pages are named after the ROM, but the emulator's rom info is not filled in
	//yet while the mapper is being set up - resolve it on first use, like the disk image does.
	BbkPrinter& Printer()
	{
		if(!_printerNamed) {
			_printerNamed = true;
			_printer.SetRomName(FolderUtilities::GetFilename(_emu->GetRomInfo().RomFile.GetFilePath(), false));
		}
		return _printer;
	}

	void GetMemoryRanges(MemoryRanges& ranges) override
	{
		BaseMapper::GetMemoryRanges(ranges);

#ifndef BBK_DISABLE_PPU_SHADOW
		//Claim PPU register writes as well (the PPU registered first; override it and forward)
		ranges.SetAllowOverride();
		ranges.AddHandler(MemoryOperation::Write, 0x2000, 0x3FFF);
#endif
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		//$FF00-$FFFF reads: IO at addr % 8 == 0, otherwise DRAM when mapped there, ROM elsewhere.
		//DRAM normally hides the IO ports; with the overlay enabled via $FF11 alone, $FF09 bit 1
		//punches them back through (also forced for the duration of a raster IRQ, so the handler
		//can read $FFB0). Only the 8-aligned ports are affected - the rest of the page stays
		//DRAM, which is where the interrupt vectors software installs for itself live. Letting
		//the exposure spill onto the whole page made $FFFE/$FFFF read the BIOS ROM vector, so a
		//program's own handler never ran and whatever it was meant to acknowledge (the APU frame
		//counter here) held the line asserted, re-entering the BIOS dispatcher until the stack wrapped.
		bool dramHere = DramAtC000();
		bool ioVisible = !dramHere || (_bbk98 && !_mapRam && ((_regFF09 & 0x02) || _ff09IrqIoOverride));

		if((addr & 0x07) == 0 && ioVisible) {
			//IO read
			if(_bbk98) {
				//The 98's interrupt controller shadows part of the FDC range
				switch(addr) {
					case 0xFFB0:
						//Reading the pending-source port acknowledges the IRQ - the handler
						//re-enables interrupts (CLI) before dispatching, so the line must
						//drop as soon as the source id is read
						_lineIrqPending = false;
						CheckIrq();
						return 0x06;
					//$FF98 reads fall through to the FDC (IRQ status, port 3) - only the
					//write side belongs to the interrupt controller (EOI)
					case 0xFF08: return _regFF08;
					default: break;
				}
			}

			if(addr >= 0xFF80 && addr <= 0xFFB8) {
				CheckForDiskImage();
				_fdc.MarkActivity();
				return _fdc.Read((addr >> 3) & 0x07);
			}

			switch(addr) {
				case 0xFF18: return _lpcAudio->ReadStatus();
				case 0xFF40: return 0; //Printer data port is write-only
				case 0xFF48: return _printer.ReadStatus();
				case 0xFF50: return 0; //PC Card
				default: return 0;
			}
		}

		if(dramHere) {
			uint8_t page = DramPage(_regFF2C);
			return IsDramPageAbsent(page) ? 0 : _workRam[page * 0x2000 + (addr & 0x1FFF)];
		}

		//BIOS ROM, fixed last 8K page
		return _prgRom[(_prgSize - 0x2000) + (addr & 0x1FFF)];
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr < 0x4000) {
			//$2000-$3FFF: forward to the PPU (write-through), and shadow $2000/$2001/
			//$2004/$2005 into DRAM while the Inno shadow is active so games can read
			//their PPU register state back. Note: BBK games rely on write-through for
			//mid-frame effects (e.g. switching the bg pattern table mid-screen for a
			//separate bottom tileset via these writes).
			_console->GetPpu()->WriteRam(addr, value);

			if((addr & 0x07) == 0x01) {
				//$2001: track whether the display is on (the PPU's own flag is not public)
				_renderEnabled = (value & 0x18) != 0;
			}

			//The shadow lets software read its own PPU register state back - the BIOS interrupt
			//handler restores $2001 from it every frame. The 98's Inno revision mirrors
			//$2000/$2001/$2005 whenever the $C000 DRAM overlay is active ($FF01 D3 *or* $FF11
			//bit 7); gating it on $FF01 D3 alone left the shadow stale at "rendering enabled",
			//so the handler switched rendering back on in the middle of the CHR-RAM uploads the
			//disk software performs with the display off, scattering them across CHR-RAM.
			//$FF01 D4 switches the shadow off, and software relies on that: it lands in the
			//fixed $4000-$7FFF window at $6000/$6001/$6005, which cartridge-style titles use as
			//their save RAM, so the loader sets D4 in the same write that enables MMC3-clone
			//mode. Ignoring D4 here left every $2000/$2001 write overwriting those bytes for as
			//long as the game ran.
			uint8_t reg = addr & 0x07;
			bool shadow = _bbk98
				? (DramAtC000() && !_ff01D4 && (reg == 0 || reg == 1 || reg == 5))
				: ((addr & 0x02) == 0 && _mapRam && !_ff01D4);
			if(shadow) {
				_workRam[(_bbk98 ? 0xFA000 : 0x7A000) + (addr & 0x1FFF)] = value;
			}
			return;
		}

		//In MMC3-clone mode, $8000-$FEFF writes are MMC3 register accesses (bank select/data,
		//mirroring, IRQ), not DRAM stores. $FF00-$FFFF stays BBK IO (the games drive MMC3
		//entirely via $8000-$E001, all below $FF00).
		if(_mmc3Mode && addr >= 0x8000 && addr < 0xFF00) {
			Mmc3Write(addr, value);
			return;
		}

		//$FF00-$FFFF: writes always hit IO
		if(_bbk98) {
			if(addr == 0xFFB0) {
				//IRQ mask - writing also acknowledges the pending IRQ
				_regFFB0 = value;
				_lineIrqPending = false;
				CheckIrq();
				return;
			}
			if(addr == 0xFF98) {
				//EOI - acknowledges the vblank IRQ and drops the IO-register exposure it forced
				_ff09IrqIoOverride = false;
				_lineIrqPending = false;
				CheckIrq();
				return;
			}
			if(addr == 0xFF08) {
				//Bit 5: vblank IRQ master enable
				_regFF08 = value;
				return;
			}
			if(addr == 0xFF09) {
				//Bit 6 is the top DRAM address line, so the banked windows have to follow it
				bool addrLineChanged = ((_regFF09 ^ value) & 0x40) != 0;
				_regFF09 = value;
				if(addrLineChanged) {
					UpdatePrgBank8000();
					UpdatePrgBankA000();
					UpdatePrgBankC000();
					UpdatePrgBankE000();
				}
				return;
			}
			if(addr == 0xFF11) {
				//BIOS-call nesting counter; bit 7 maps DRAM over $C000-$FFFF
				_regFF11 = value;
				UpdatePrgBankC000();
				UpdatePrgBankE000();
				return;
			}
			if(addr == 0xFF19) {
				//Written at the start of a BIOS call - drops the $C000-$FFFF DRAM overlay
				_regFF11 = 0;
				UpdatePrgBankC000();
				UpdatePrgBankE000();
				return;
			}
		}

		if(addr >= 0xFF80 && addr <= 0xFFB8) {
			CheckForDiskImage();
			_fdc.MarkActivity();
			_fdc.Write((addr >> 3) & 0x07, value);
			return;
		}

		switch(addr) {
			case 0xFF00: //Keyboard LED port (D2: !CapsLock, D1: !NumLock)
				break;

			case 0xFF01: //VideoCtrlPort
				//D6: [holtek] split mode, D4: [inno] PPU reg shadow disable,
				//D3: [inno] $C000-$FFFF DRAM/ROM, D2: [holtek] IRQ count enable,
				//D[1:0]: [holtek] nametable mirroring
				switch(value & 0x03) {
					case 0: SetMirroringType(MirroringType::Vertical); break;
					case 1: SetMirroringType(MirroringType::Horizontal); break;
					case 2: SetMirroringType(MirroringType::ScreenAOnly); break;
					case 3: SetMirroringType(MirroringType::ScreenBOnly); break;
				}

				_splitMode = (value & 0x40) != 0;
				_enableIrq = (value & 0x04) != 0;
				CheckIrq();

				//Leaving split mode hands $0000-$1FFF back to the bank registers
				if(_bbk98) {
					UpdateChrBanks98();
				}

				_ff01D4 = (value & 0x10) != 0;

				_mapRam = (value & 0x08) != 0;
				UpdatePrgBankC000();
				UpdatePrgBankE000();

				//D5: MMC3-clone banking mode. Games run entirely through the MMC3 register
				//file mapped into DRAM; masks come from $FF3C (set just before this write).
				{
					bool mmc3 = (value & 0x20) != 0;
					if(mmc3) {
						_ypramMask = ~(_regFF3C & 0xF0) & 0x3F;    //0x0F:128K / 0x1F:256K / 0x3F:512K
						_ycramMask = (_regFF3C & 0x04) ? 0xFF : 0x7F;
						if(!_mmc3Mode) {
							_mmc3Mode = true;
							AddRegisterRange(0x8000, 0xFEFF, MemoryOperation::Write);
						}
						Mmc3Reset();
						Mmc3Sync();
					} else if(_mmc3Mode) {
						_mmc3Mode = false;
						RemoveRegisterRange(0x8000, 0xFEFF, MemoryOperation::Write);
						UpdatePrgBank8000();
						UpdatePrgBankA000();
						UpdatePrgBankC000();
						UpdatePrgBankE000();
					}
				}
				break;

			case 0xFF02: //IntCountPortL
				_lineCount = value;
				break;

			case 0xFF06: //IntCountPortH
				_lineCount &= 0x0F;
				_lineCount |= (value & 0x0F) << 4;
				break;

			case 0xFF04: //DRAMPagePort (16K granularity; ROM/DRAM select on D5, or D7 on the BBK-98)
				if(_bbk98) {
					//D7 selects DRAM/ROM, D6-D0 = 16K page of either; D7 shifts out of
					//the 8-bit DRAM bank value so it is tracked separately
					_dram8000 = (value & 0x80) != 0;
					if(!_dram8000) {
						_romBank16k = value & 0x7F;
					}
					_regFF14 = (uint8_t)(value << 1);
					_regFF1C = (uint8_t)(value << 1) | 0x01;
				} else {
					_regFF14 = (value << 1) & 0x7F;
					_regFF1C = ((value << 1) & 0x3F) | 0x01;
				}
				UpdatePrgBank8000();
				UpdatePrgBankA000();
				break;

			case 0xFF14: //DRAM page for $8000-$9FFF
				if(_bbk98) {
					_regFF14 = value;
					_dram8000 = true;
				} else {
					_regFF14 = (value & 0x3F) | 0x40;
				}
				UpdatePrgBank8000();
				UpdatePrgBankA000();
				break;

			case 0xFF1C: //DRAM page for $A000-$BFFF
				if(_bbk98) {
					_regFF1C = value;
					_dram8000 = true;
				} else {
					_regFF1C = value & 0x3F;
				}
				//The fork clears the ROM-select here as well (bank at $8000 stays as-is
				//because _regFF14 bit 6 drives both slots)
				UpdatePrgBankA000();
				break;

			case 0xFF24: //DRAM page for $C000-$DFFF
				_regFF24 = value & DramBankMask();
				//Re-map whenever DRAM is actually there - on the BBK-98 the overlay can come from
				//$FF11 bit 7 instead of $FF01 bit 3, and gating on the latter alone left the window
				//pointing at the previously selected page (the fork re-applies both from SetBank())
				if(DramAtC000()) {
					UpdatePrgBankC000();
				}
				break;

			case 0xFF2C: //DRAM page for $E000-$FFFF
				_regFF2C = value & DramBankMask();
				if(DramAtC000()) {
					UpdatePrgBankE000();
				}
				break;

			case 0xFF3C: //MMC3-clone size/mask config (upper nibble = YPRAM mask, bit 2 = YCRAM)
				_regFF3C = value;
				break;

			//Holtek split engine
			case 0xFF12: //Init
				if(_bbk98) {
					//Clears the live queue
					_nrOfSR = 0;
					_nrOfVR = 0;
					_queueIndex = 0;
					break;
				}
				if(!_enableIrq) {
					_nrOfSR = 0;
					_nrOfVR = 0;
					if(_splitMode) {
						_queueIndex = 0;
					}
				}
				CheckIrq();
				break;

			case 0xFF0A: //Scanline count queue
				if(_bbk98) {
					if(_stageCount < 64) {
						_stageSR[_stageCount++] = value;
					}
					break;
				}
				_queueSR[_nrOfSR & 0x1F] = value;
				if(_enableIrq) {
					_nrOfSR = 0;
				} else {
					_nrOfSR = (_nrOfSR + 1) & 0x1F;
				}
				CheckIrq();
				break;

			case 0xFF1A: //Video bank queue
				if(_bbk98) {
					if(_stageCountVR < 64) {
						_stageVR[_stageCountVR++] = value;
					}
					break;
				}
				_queueVR[_nrOfVR & 0x1F] = value;
				if(_enableIrq) {
					_nrOfVR = 0;
				} else {
					_nrOfVR = (_nrOfVR + 1) & 0x1F;
				}
				CheckIrq();
				break;

			case 0xFF22: //Start
				if(_bbk98) {
					//Commits the staged queue for the frames that follow
					memcpy(_queueSR, _stageSR, sizeof(_queueSR));
					memcpy(_queueVR, _stageVR, sizeof(_queueVR));
					_nrOfSR = _stageCount;
					_nrOfVR = _stageCountVR;
					_stageCount = 0;
					_stageCountVR = 0;
					break;
				}
				if(_splitMode) {
					if(!_enableIrq) {
						_queueIndex = 0;
						_lineCount = 0;
					}
					ApplySplitEntry();
				}
				CheckIrq();
				break;

			case 0xFF2A: //Restart the split queue from the top (BBK-98 only)
				if(_bbk98) {
					_queueIndex = 0;
				}
				break;

			//CHR banking - 2K pages for $0000-$1FFF
			case 0xFF03: WriteChrReg2k(0, value); break;
			case 0xFF0B: WriteChrReg2k(1, value); break;
			case 0xFF13: WriteChrReg2k(2, value); break;
			case 0xFF1B: WriteChrReg2k(3, value); break;

			//CHR banking - 1K pages
			case 0xFF23: WriteChrReg1k(0, value); break;
			case 0xFF2B: WriteChrReg1k(1, value); break;
			case 0xFF33: WriteChrReg1k(2, value); break;
			case 0xFF3B: WriteChrReg1k(3, value); break;
			case 0xFF43: WriteChrReg1k(4, value); break;
			case 0xFF4B: WriteChrReg1k(5, value); break;
			case 0xFF53: WriteChrReg1k(6, value); break;
			case 0xFF5B: WriteChrReg1k(7, value); break;

			//LPC speech synthesizer
			case 0xFF10: _lpcAudio->WriteControl(value); break;
			case 0xFF18: _lpcAudio->WriteData(value); break;

			//Parallel port printer ($FF48 is the host-to-machine data/handshake half of the
			//PC-card link, which nothing on these disks uses)
			case 0xFF40: Printer().WriteData(value); break;
			case 0xFF48: break;
			case 0xFF50: Printer().WriteControl(value); break;

			default:
				break;
		}
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);

		SV(_fdc);
		SV(_lpcAudio);
		SV(_printer);

		SV(_regFF14); SV(_regFF1C); SV(_regFF24); SV(_regFF2C);
		SV(_mapRam); SV(_ff01D4);
		SV(_romBank16k); SV(_dram8000); SV(_regFF08); SV(_regFFB0); SV(_lineIrqPending);
		SV(_regFF09); SV(_ff09IrqIoOverride); SV(_regFF11);
		SV(_splitMode); SV(_enableIrq); SV(_lineCount);
		SV(_nrOfSR); SV(_nrOfVR); SV(_queueIndex);
		SVArray(_queueSR, 64);
		SVArray(_queueVR, 64);
		SVArray(_stageSR, 64);
		SVArray(_stageVR, 64);
		SVArray(_chrReg, 8);
		SV(_stageCount); SV(_stageCountVR); SV(_splitLine); SV(_renderEnabled);
		SV(_lastPpuScanline); SV(_irqPending); SV(_irqApplied);

		SV(_mmc3Mode); SV(_mmc3Cmd); SV(_mmc3Prg0); SV(_mmc3Prg1);
		SV(_mmc3Chr01); SV(_mmc3Chr23); SV(_mmc3Chr4); SV(_mmc3Chr5); SV(_mmc3Chr6); SV(_mmc3Chr7);
		SV(_mmc3IrqLatch); SV(_mmc3IrqCounter); SV(_mmc3IrqPreset); SV(_mmc3IrqPresetVbl); SV(_mmc3IrqEnable);
		SV(_regFF3C); SV(_pramMask); SV(_ypramMask); SV(_ycramMask);
	}

public:
	//Disk swapping - driven by the FDS disk shortcut keys via DiskSwapListener
	vector<string> GetDiskFileList()
	{
		string folder = GetConfiguredDiskFolder();
		if(folder.empty()) {
			folder = FolderUtilities::GetFolderName(_emu->GetRomInfo().RomFile.GetFilePath());
		}
		vector<string> files = FolderUtilities::GetFilesInFolder(folder, { ".img", ".ima" }, false);
		std::sort(files.begin(), files.end());
		return files;
	}

	uint32_t GetDiskCount()
	{
		return (uint32_t)GetDiskFileList().size();
	}

	//Full path of the disk image currently inserted in the drive, or "" if the drive is empty
	string GetCurrentDiskFilename()
	{
		return _fdc.IsDiskInserted() ? _fdc.GetDiskFilename() : "";
	}

	void EjectDisk()
	{
		auto lock = _emu->AcquireLock();
		if(_fdc.IsDiskInserted()) {
			MessageManager::DisplayMessage("BBK", "Disk ejected: " + FolderUtilities::GetFilename(_fdc.GetDiskFilename(), true));
			_fdc.EjectDisk(); //Saves pending changes first
		}
		//Forget any remembered selection so a later power cycle boots from the paired disk
		_persistedDiskRom.clear();
		_persistedDiskPath.clear();
	}

	void InsertDisk(uint32_t index)
	{
		auto lock = _emu->AcquireLock();
		vector<string> disks = GetDiskFileList();
		if(index < disks.size()) {
			_fdc.EjectDisk();
			if(_fdc.LoadDiskImage(disks[index])) {
				_diskChecked = true;
				//Remember the selection so a power cycle reboots from this disk, not the paired one
				_persistedDiskRom = _emu->GetRomInfo().RomFile.GetFilePath();
				_persistedDiskPath = disks[index];
				MessageManager::DisplayMessage("BBK", "Disk inserted: " + FolderUtilities::GetFilename(disks[index], true));
			}
		}
	}

	void InsertNextDisk()
	{
		vector<string> disks = GetDiskFileList();
		if(disks.empty()) {
			return;
		}

		int current = -1;
		for(size_t i = 0; i < disks.size(); i++) {
			if(disks[i] == _fdc.GetDiskFilename()) {
				current = (int)i;
				break;
			}
		}
		InsertDisk((current + 1) % disks.size());
	}

	void ProcessCpuClock() override
	{
		BaseProcessCpuClock();

		_lpcAudio->Clock();
		_fdc.Clock();
		_printer.Clock();

		//Run the per-scanline Holtek logic once per line, during hblank after the sprite
		//fetches (PPU cycle >= 321). The reference emulator renders its line N *then*
		//calls HSync(N), so HSync(N)'s CHR swaps first affect display row N+1 (line 0 is
		//the dummy line - it never draws a row for it). Firing HSync(N) at row N's hblank
		//reproduces that exactly; firing one line earlier corrupts one row per split
		//band (rows 15, 31, 47...) - e.g. a hi-color banner that queues SR=$F0 gives
		//16-line CHR bands which must begin exactly at display row 16k. The pre-render
		//line runs no HSync.
		int32_t scanline = _console->GetPpu()->GetCurrentScanline();
		uint32_t cycle = _console->GetPpu()->GetCurrentCycle();
		if(scanline != _lastPpuScanline && cycle >= 321) {
			_lastPpuScanline = scanline;
			if(scanline >= 0) {
				HSync(scanline);
			}

			//Disk activity LED (green square, top-right corner) - off by default, toggled in the NES config
			if(scanline == 240 && _fdc.IsActive() && _console->GetNesConfig().BbkShowDiskLed) {
				_emu->GetDebugHud()->DrawRectangle(245, 11, 8, 8, 0x000000, true, 1);
				_emu->GetDebugHud()->DrawRectangle(246, 12, 6, 6, 0x00E040, true, 1);
			}
		}

		//Apply the IRQ line state near the end of the scanline. The reference emulator executes the
		//CPU in whole-scanline batches, so its mapper IRQ handler always starts at the
		//beginning of a scanline; asserting here means the cycle-accurate CPU takes the
		//IRQ right around the next line's start, matching that timing. Asserting
		//earlier makes the handler's raster-timed writes land at spots that vary from
		//frame to frame (occasional flicker).
		if(!_irqApplied && cycle >= 334) {
			_irqApplied = true;
			if(_irqPending) {
				_console->GetCpu()->SetIrqSource(IRQSource::External);
			} else {
				_console->GetCpu()->ClearIrqSource(IRQSource::External);
			}
		}
	}

	void SaveBattery() override
	{
		if(_fdc.IsDirty()) {
			if(_fdc.SaveDiskImage()) {
				MessageManager::Log("[BBK] Disk image saved: " + _fdc.GetDiskFilename());
			} else {
				MessageManager::Log("[BBK] Failed to save disk image!");
			}
		}
	}

	~BbkMapper() override
	{
		//Flush disk changes when the ROM is closed
		if(_fdc.IsDirty()) {
			_fdc.SaveDiskImage();
		}
	}
};
