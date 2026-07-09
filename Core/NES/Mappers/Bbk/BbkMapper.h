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

	//Holtek ASIC state
	bool _splitMode = false;
	bool _enableIrq = false;
	uint16_t _lineCount = 0;
	uint8_t _nrOfSR = 0;
	uint8_t _nrOfVR = 0;
	uint8_t _queueSR[32] = {};
	uint8_t _queueVR[32] = {};
	uint8_t _queueIndex = 0;

	int32_t _lastPpuScanline = -2;
	bool _irqPending = false;
	bool _irqApplied = true;
	bool _diskChecked = false;

	//A power cycle recreates the mapper, so the inserted disk is remembered here (like a floppy
	//physically staying in the drive) and re-mounted instead of the paired boot disk. Scoped to
	//the ROM path so a different game doesn't inherit a stale selection.
	inline static string _persistedDiskRom;
	inline static string _persistedDiskPath;

	//Set from the UI when a .img/.ima is opened directly: the next BBK boot mounts this image
	//instead of the paired disk (like loading an FDS disk boots the FDS BIOS with that disk).
	inline static string _pendingBootDiskPath;

	void UpdatePrgBank8000()
	{
		if(_regFF14 & 0x40) {
			SetCpuMemoryMapping(0x8000, 0x9FFF, _regFF14 & 0x3F, PrgMemoryType::WorkRam, MemoryAccessType::ReadWrite);
		} else {
			SelectPrgPage(0, _regFF14 & 0x0F);
		}
	}

	void UpdatePrgBankA000()
	{
		//The DRAM/ROM selector for $A000-$BFFF is $FF14's bit 6 (not $FF1C's)
		if(_regFF14 & 0x40) {
			SetCpuMemoryMapping(0xA000, 0xBFFF, _regFF1C & 0x3F, PrgMemoryType::WorkRam, MemoryAccessType::ReadWrite);
		} else {
			SelectPrgPage(1, _regFF1C & 0x0F);
		}
	}

	void UpdatePrgBankC000()
	{
		if(_mapRam) {
			SetCpuMemoryMapping(0xC000, 0xDFFF, _regFF24 & 0x3F, PrgMemoryType::WorkRam, MemoryAccessType::ReadWrite);
		} else {
			SelectPrgPage(2, 0x0E);
		}
	}

	void UpdatePrgBankE000()
	{
		//$FF00-$FFFF is additionally covered by the register handlers
		if(_mapRam) {
			SetCpuMemoryMapping(0xE000, 0xFFFF, _regFF2C & 0x3F, PrgMemoryType::WorkRam, MemoryAccessType::ReadWrite);
		} else {
			SelectPrgPage(3, 0x0F);
		}
	}

	void SelectChrPage2k(uint8_t slot2k, uint8_t page2k)
	{
		SelectChrPage(slot2k * 2, (page2k & 0x0F) * 2);
		SelectChrPage(slot2k * 2 + 1, (page2k & 0x0F) * 2 + 1);
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
		if(EvaluateIrq()) {
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
	void HSync(int32_t scanline)
	{
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
		//taking precedence over the paired/persisted disk (cleared so it only applies to this boot).
		if(!_pendingBootDiskPath.empty()) {
			string pending = _pendingBootDiskPath;
			_pendingBootDiskPath.clear();
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
	uint32_t GetChrRamSize() override { return 0x8000; }
	uint16_t GetChrRamPageSize() override { return 0x400; }
	uint32_t GetWorkRamSize() override { return 0x80000; }
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

		//$4000-$7FFF (from $4100, below the APU/input registers): fixed DRAM window at $78000
		SetCpuMemoryMapping(0x4100, 0x7FFF, PrgMemoryType::WorkRam, 0x78100, MemoryAccessType::ReadWrite);

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
		//$FF00-$FFFF reads: DRAM when mapRam is set, otherwise IO at addr % 8 == 0, ROM elsewhere
		if(_mapRam) {
			return _workRam[(_regFF2C & 0x3F) * 0x2000 + (addr & 0x1FFF)];
		}

		if((addr & 0x07) == 0) {
			//IO read
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

			if((addr & 0x02) == 0 && _mapRam && !_ff01D4) {
				_workRam[0x7A000 + (addr & 0x1FFF)] = value;
			}
			return;
		}

		//$FF00-$FFFF: writes always hit IO
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

			case 0xFF04: //DRAMPagePort (16K granularity, ROM/DRAM select on D5)
				_regFF14 = (value << 1) & 0x7F;
				_regFF1C = ((value << 1) & 0x3F) | 0x01;
				UpdatePrgBank8000();
				UpdatePrgBankA000();
				break;

			case 0xFF14: //DRAM page for $8000-$9FFF
				_regFF14 = (value & 0x3F) | 0x40;
				UpdatePrgBank8000();
				UpdatePrgBankA000();
				break;

			case 0xFF1C: //DRAM page for $A000-$BFFF
				_regFF1C = value & 0x3F;
				//The fork clears the ROM-select here as well (bank at $8000 stays as-is
				//because _regFF14 bit 6 drives both slots)
				UpdatePrgBankA000();
				break;

			case 0xFF24: //DRAM page for $C000-$DFFF
				_regFF24 = value & 0x3F;
				if(_mapRam) {
					UpdatePrgBankC000();
				}
				break;

			case 0xFF2C: //DRAM page for $E000-$FFFF
				_regFF2C = value & 0x3F;
				if(_mapRam) {
					UpdatePrgBankE000();
				}
				break;

			//Holtek split engine
			case 0xFF12: //Init
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
				_queueSR[_nrOfSR & 0x1F] = value;
				if(_enableIrq) {
					_nrOfSR = 0;
				} else {
					_nrOfSR = (_nrOfSR + 1) & 0x1F;
				}
				CheckIrq();
				break;

			case 0xFF1A: //Video bank queue
				_queueVR[_nrOfVR & 0x1F] = value;
				if(_enableIrq) {
					_nrOfVR = 0;
				} else {
					_nrOfVR = (_nrOfVR + 1) & 0x1F;
				}
				CheckIrq();
				break;

			case 0xFF22: //Start
				if(_splitMode) {
					if(!_enableIrq) {
						_queueIndex = 0;
						_lineCount = 0;
					}
					ApplySplitEntry();
				}
				CheckIrq();
				break;

			//CHR banking - 2K pages for $0000-$1FFF
			case 0xFF03: SelectChrPage2k(0, value); break;
			case 0xFF0B: SelectChrPage2k(1, value); break;
			case 0xFF13: SelectChrPage2k(2, value); break;
			case 0xFF1B: SelectChrPage2k(3, value); break;

			//CHR banking - 1K pages
			case 0xFF23: SelectChrPage(0, value & 0x1F); break;
			case 0xFF2B: SelectChrPage(1, value & 0x1F); break;
			case 0xFF33: SelectChrPage(2, value & 0x1F); break;
			case 0xFF3B: SelectChrPage(3, value & 0x1F); break;
			case 0xFF43: SelectChrPage(4, value & 0x1F); break;
			case 0xFF4B: SelectChrPage(5, value & 0x1F); break;
			case 0xFF53: SelectChrPage(6, value & 0x1F); break;
			case 0xFF5B: SelectChrPage(7, value & 0x1F); break;

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
		SV(_splitMode); SV(_enableIrq); SV(_lineCount);
		SV(_nrOfSR); SV(_nrOfVR); SV(_queueIndex);
		SVArray(_queueSR, 32);
		SVArray(_queueVR, 32);
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

	//Called from the UI (interop) before loading a BBK BIOS ROM so the fresh boot mounts a
	//specific .img the user opened directly. Static because no mapper exists yet at that point.
	static void SetPendingBootDisk(string path) { _pendingBootDiskPath = path; }

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
