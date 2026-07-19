#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/BaseNesPpu.h"
#include "NES/NesConsole.h"
#include "NES/NesCpu.h"
#include "NES/Mappers/Bbk/BbkFdc.h"
#include "NES/Mappers/Bbk/BbkLpcAudio.h"
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
// - $FF11 bit 7 additionally maps DRAM over $C000-$FFFF (a BIOS-call nesting counter,
//   cleared by writing $FF19); $FF09 bit 1 exposes the IO registers to reads while
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
	//IO registers unless $FF09 bit 1 is set - the IRQ hardware forces that bit (and $FF98
	//restores it) so the interrupt handler can read $FFB0/$FF08 with the overlay active.
	uint8_t _regFF09 = 0;
	uint8_t _prevFF09 = 0;
	uint8_t _regFF11 = 0;

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

	int32_t _lastPpuScanline = -2;
	bool _irqPending = false;
	bool _irqApplied = true;
	bool _diskChecked = false;

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
		//The DRAM/ROM selector for $A000-$BFFF is $FF14's bit 6 (not $FF1C's)
		if(DramAt8000()) {
			MapDramPage(0xA000, 0xBFFF, DramPage(_regFF1C));
		} else if(_bbk98) {
			SelectPrgPage(1, _romBank16k * 2 + 1);
		} else {
			SelectPrgPage(1, _regFF1C & 0x0F);
		}
	}

	//DRAM at $C000-$FFFF: classic models use $FF01 bit 3 only; the 98 also maps it via $FF11 bit 7
	bool DramAtC000() { return _mapRam || (_bbk98 && (_regFF11 & 0x80)); }

	void UpdatePrgBankC000()
	{
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
		//Expose the IO registers to the handler even if the $FF11 DRAM overlay is active;
		//the $FF98 EOI restores the previous $FF09 value
		_prevFF09 = _regFF09;
		_regFF09 |= 0x02;
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

	void HSync(int32_t scanline)
	{
		if(_bbk98) {
			HSync98(scanline);
			return;
		}

		//Restart the split sequence at the top of each frame while IRQ handling is pending
		if(scanline == 0 && _splitMode) {
			//Continue using the settings of the last frame
			_queueIndex = 0;
			ApplySplitEntry();
		}

		if(scanline >= 240) {
			return;
		}

		_irqPending = EvaluateIrq();
		_irqApplied = false;

		if(_lineCount == 255) {
			if(_queueIndex != _nrOfSR) {
				ApplySplitEntry();
			}
		} else if(_splitMode || _enableIrq) {
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

	//BBK software polls $2002 inside NMI-toggling copy loops; on a stock 2C02 a read
	//between vblank-flag-set and the next NMI enable clears the flag and eats the NMI
	//(losing that frame's PPUCTRL/OAM refresh - visible as a one-frame wrong-pattern-
	//table flash at screen transitions). The clone PPU evidently only clears
	//the flag at the pre-render line.
	bool EnablePpuVblankFlagClearOnRead() override { return false; }

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
		_prevFF09 = 0;
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

		UpdatePrgBank8000();
		UpdatePrgBankA000();
		UpdatePrgBankC000();
		UpdatePrgBankE000();

		for(int i = 0; i < 8; i++) {
			SelectChrPage(i, i);
		}

		_lpcAudio->Reset();
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
		//$FF00-$FFFF reads: DRAM when mapped there, otherwise IO at addr % 8 == 0, ROM elsewhere.
		//With the overlay enabled via $FF11 alone, $FF09 bit 1 lets IO reads through (set by
		//the IRQ hardware so the handler can read $FFB0 while the overlay is active).
		if(_mapRam || (_bbk98 && (_regFF11 & 0x80) && !(_regFF09 & 0x02))) {
			uint8_t page = DramPage(_regFF2C);
			return IsDramPageAbsent(page) ? 0 : _workRam[page * 0x2000 + (addr & 0x1FFF)];
		}

		if((addr & 0x07) == 0) {
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
				case 0xFF50: return 0; //PC Card
				default: return 0;
			}
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

			if((addr & 0x02) == 0 && _mapRam && !_ff01D4) {
				_workRam[(_bbk98 ? 0xFA000 : 0x7A000) + (addr & 0x1FFF)] = value;
			}
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
				//EOI / mask restore - acknowledges the vblank IRQ and restores $FF09
				//(the IRQ hardware forced its bit 1 on to expose the IO registers)
				_regFF09 = _prevFF09;
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

				_ff01D4 = (value & 0x10) != 0;

				_mapRam = (value & 0x08) != 0;
				UpdatePrgBankC000();
				UpdatePrgBankE000();
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
				if(_mapRam) {
					UpdatePrgBankC000();
				}
				break;

			case 0xFF2C: //DRAM page for $E000-$FFFF
				_regFF2C = value & DramBankMask();
				if(_mapRam) {
					UpdatePrgBankE000();
				}
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
			case 0xFF03: SelectChrPage2k(0, value); break;
			case 0xFF0B: SelectChrPage2k(1, value); break;
			case 0xFF13: SelectChrPage2k(2, value); break;
			case 0xFF1B: SelectChrPage2k(3, value); break;

			//CHR banking - 1K pages (8-bit registers on the BBK-98, 5-bit on the classic models)
			case 0xFF23: SelectChrPage(0, value & (_bbk98 ? 0xFF : 0x1F)); break;
			case 0xFF2B: SelectChrPage(1, value & (_bbk98 ? 0xFF : 0x1F)); break;
			case 0xFF33: SelectChrPage(2, value & (_bbk98 ? 0xFF : 0x1F)); break;
			case 0xFF3B: SelectChrPage(3, value & (_bbk98 ? 0xFF : 0x1F)); break;
			case 0xFF43: SelectChrPage(4, value & (_bbk98 ? 0xFF : 0x1F)); break;
			case 0xFF4B: SelectChrPage(5, value & (_bbk98 ? 0xFF : 0x1F)); break;
			case 0xFF53: SelectChrPage(6, value & (_bbk98 ? 0xFF : 0x1F)); break;
			case 0xFF5B: SelectChrPage(7, value & (_bbk98 ? 0xFF : 0x1F)); break;

			//LPC speech synthesizer
			case 0xFF10: _lpcAudio->WriteControl(value); break;
			case 0xFF18: _lpcAudio->WriteData(value); break;

			//PC-Card / parallel port (not emulated)
			case 0xFF40: case 0xFF48: case 0xFF50:
				break;

			default:
				break;
		}
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);

		SV(_fdc);
		SV(_lpcAudio);

		SV(_regFF14); SV(_regFF1C); SV(_regFF24); SV(_regFF2C);
		SV(_mapRam); SV(_ff01D4);
		SV(_romBank16k); SV(_dram8000); SV(_regFF08); SV(_regFFB0); SV(_lineIrqPending);
		SV(_regFF09); SV(_prevFF09); SV(_regFF11);
		SV(_splitMode); SV(_enableIrq); SV(_lineCount);
		SV(_nrOfSR); SV(_nrOfVR); SV(_queueIndex);
		SVArray(_queueSR, 64);
		SVArray(_queueVR, 64);
		SVArray(_stageSR, 64);
		SVArray(_stageVR, 64);
		SV(_stageCount); SV(_stageCountVR); SV(_splitLine); SV(_renderEnabled);
		SV(_lastPpuScanline); SV(_irqPending); SV(_irqApplied);
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
