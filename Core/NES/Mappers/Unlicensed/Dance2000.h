#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/NesConsole.h"
#include "NES/NesMemoryManager.h"
#include "NES/NesControlManager.h"
#include "NES/Mappers/Bbk/BbkPrinter.h"
#include "NES/Mappers/Bbk/BbkLpcAudio.h"
#include "NES/Mappers/Bbk/PcFdc.h"
#include "Shared/NotificationManager.h"
#include "Shared/MessageManager.h"
#include "NES/Mappers/Subor/SuborCarts.h"
#include "Utilities/FolderUtilities.h"

//The Subor learning cartridges on this board carry more than the bank registers:
// - $5300: the LPC-10 speech chip of the Subor V7 board (see Subor168) - status $80 = can take
//   data, $0F = end of speech. The add-on cards' programs wait on it before they read a key.
// - a parallel printer driven the way the BBK voice models drive theirs: a byte written to
//   $480F, after waiting for bit 0 of $4016 - the printer's ready line, and also the port 1
//   controller's data line, so which of the two is attached is the printer setting.
// - on the V5, a floppy drive for SB DOS: a PC-style uPD765 with the digital output register
//   written at $5501, the data register written at $5505 and read at $5605, the main status
//   read at $5604, and its interrupt on the CPU's IRQ line. Disks are 1.44MB PC images.
//   SB DOS loads a program into 256KB of RAM that $5000 bit 7 maps at $8000 (a 16KB page in
//   bits 0-3, written through the pointer at $80/$81) and checks its header there before
//   running it - a .EXE in 32KB mode ($5200 bit 2), where bits 0-2 pick a 32KB RAM bank. The
//   RAM is apart from the 8KB at $6000-$7FFF, which SB DOS keeps its own state in.
class Dance2000 : public BaseMapper
{
private:
	class DiskSwapListener final : public INotificationListener
	{
	private:
		Dance2000* _mapper;

	public:
		DiskSwapListener(Dance2000* mapper) : _mapper(mapper) {}

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

	bool _hasFloppy = false;
	PcFdc _fdc;
	static constexpr uint32_t DramSize = 0x40000;
	vector<uint8_t> _dram;
	bool _diskChecked = false;
	shared_ptr<DiskSwapListener> _swapListener;
	//A power cycle recreates the mapper, so the disk in the drive is remembered here and put back
	//rather than the paired one - scoped to the cartridge it was put in under
	inline static string _persistedDiskRom;
	inline static string _persistedDiskPath;

	string GetConfiguredDiskFolder()
	{
		const char* folder = _console->GetNesConfig().BbkDiskFolder;
		return folder[0] ? string(folder) : string();
	}

	//Puts the disk in at the first look at the drive, once the rom's path is known: the one left in
	//by the last session of this cartridge, else "<rom name>.img/.ima" beside it or in the disk folder
	void CheckForDiskImage()
	{
		if(_diskChecked) {
			return;
		}
		_diskChecked = true;

		string romPath = _emu->GetRomInfo().RomFile.GetFilePath();
		if(_persistedDiskRom == romPath && !_persistedDiskPath.empty() && _fdc.LoadDiskImage(_persistedDiskPath)) {
			MessageManager::Log("[Subor] Re-mounted disk image: " + _persistedDiskPath);
			return;
		}

		string baseName = FolderUtilities::GetFilename(romPath, false);
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
					if(_fdc.LoadDiskImage(diskPath)) {
						MessageManager::Log("[Subor] Mounted disk image: " + diskPath);
						return;
					}
				}
			}
		}
	}

	void UpdateFdcIrq()
	{
		if(_fdc.IsIrqAsserted()) {
			_console->GetCpu()->SetIrqSource(IRQSource::External);
		} else {
			_console->GetCpu()->ClearIrqSource(IRQSource::External);
		}
	}
	uint8_t _prgReg = 0;
	uint8_t _mode = 0;
	uint8_t _lastNt = 0;

	bool _isSuborCart = false;
	bool _printerAlways = false;
	bool _printerNamed = false;
	BbkPrinter _printer;
	unique_ptr<BbkLpcAudio> _lpcAudio;

	//Named lazily: the rom path is not available yet when the mapper is constructed
	BbkPrinter& Printer()
	{
		if(!_printerNamed) {
			_printerNamed = true;
			_printer.SetRomName(FolderUtilities::GetFilename(_emu->GetRomInfo().RomFile.GetFilePath(), false));
		}
		return _printer;
	}

protected:
	bool AllowRegisterRead() override { return true; }
	uint16_t GetPrgPageSize() override { return 0x4000; }
	uint16_t GetChrPageSize() override { return 0x1000; }
	bool EnableVramAddressHook() override { return true; }
	bool EnableCpuClockHook() override { return true; }

	void ProcessCpuClock() override
	{
		BaseProcessCpuClock();
		if(_isSuborCart) {
			_printer.Clock();
			_lpcAudio->Clock();
		}
		if(_hasFloppy) {
			//As on the SB-2000 (see Sb2kMapper), the Subor console's clone APU has no frame-counter
			//IRQ: the cartridge never writes $4017 or reads $4015, and SB DOS enables interrupts
			//with a handler that only answers the drive, so the 2A03's power-on frame IRQ would
			//re-enter it forever
			_console->GetCpu()->ClearIrqSource(IRQSource::FrameCounter);
			_fdc.Clock();
			UpdateFdcIrq();
		}
	}

	//$4016 is below the range the base mapper claims, so it has to be asked for to be seen
	void GetMemoryRanges(MemoryRanges& ranges) override
	{
		BaseMapper::GetMemoryRanges(ranges);
		if(_isSuborCart) {
			ranges.AddHandler(MemoryOperation::Read, 0x4016);
			ranges.SetAllowOverride();
		}
	}

	void InitMapper() override
	{
		_prgReg = _mode = _lastNt = 0;
		AddRegisterRange(0x5000, 0x5FFF, MemoryOperation::Write);
		RemoveRegisterRange(0x8000, 0xFFFF, MemoryOperation::Write);

		_isSuborCart = SuborCarts::IsSuborCart(_romInfo.Hash.PrgCrc32);
		_printerAlways = SuborCarts::HasPrinterAlways(_romInfo.Hash.PrgCrc32);
		_hasFloppy = SuborCarts::HasFloppyDrive(_romInfo.Hash.PrgCrc32);
		if(_hasFloppy) {
			_dram.assign(DramSize, 0);
			AddRegisterRange(0x5600, 0x56FF, MemoryOperation::Read);
			_fdc.Reset();
			_diskChecked = false;
			if(!_swapListener) {
				_swapListener.reset(new DiskSwapListener(this));
				_emu->GetNotificationManager()->RegisterNotificationListener(_swapListener);
			}
		}
		if(_isSuborCart) {
			AddRegisterRange(0x480F, 0x480F, MemoryOperation::Write);
			AddRegisterRange(0x4016, 0x4016, MemoryOperation::Read);
			AddRegisterRange(0x5300, 0x53FF, MemoryOperation::Read);
			_printerNamed = false;
			_printer.Reset();
			_lpcAudio.reset(new BbkLpcAudio(_console, BbkLpcAudio::LpcVariant::Sb2k));
			_lpcAudio->Reset();
		}
		UpdateState();
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SV(_mode);
		SV(_prgReg);
		SV(_lastNt);
		if(_isSuborCart) {
			SV(_printer);
			SV(_lpcAudio);
		}
		if(_hasFloppy) {
			SV(_fdc);
			SVVector(_dram);
			if(!s.IsSaving()) {
				//The RAM window points into _dram, which the base mapper cannot save
				UpdateState();
			}
		}
	}

	void SaveBattery() override
	{
		BaseMapper::SaveBattery();
		if(_hasFloppy && _fdc.IsDirty()) {
			_fdc.SaveDiskImage();
		}
	}

	void NotifyVramAddressChange(uint16_t addr) override
	{
		if(_mode & 0x02) {
			if((addr & 0x3000) == 0x2000) {
				//The page follows the nametable the fetch lands in, so it is the mirrored address
				//line that picks it: A11 with horizontal mirroring, A10 with vertical. Taking A11
				//always left a left/right split on one page - the Subor cartridges' Internet page
				//puts its text panel in the right-hand nametable under vertical mirroring.
				uint32_t currentNametable = (addr >> (GetMirroringType() == MirroringType::Vertical ? 10 : 11)) & 0x01;
				if(currentNametable != _lastNt) {
					_lastNt = currentNametable;
					SelectChrPage(0, _lastNt);
				}
			}
		} else {
			if(_lastNt != 0) {
				_lastNt = 0;
				SelectChrPage(0, _lastNt);
			}
		}
	}

	void UpdateState()
	{
		SelectChrPage(0, _lastNt);
		SelectChrPage(1, 1);
		//The Subor learning cartridges on this board are up to 2MB and use every bank bit up to bit 6:
		//a 512KB one keeps its font and half its programs in banks $10-$1F, a 1MB one starts its
		//menu from bank $2F or $3F, and a 2MB one is a 1MB cartridge with an add-on card in banks
		//$40-$7F. Smaller boards simply wrap.
		if(_hasFloppy && (_prgReg & 0x80) && (_mode & 0x04)) {
			//How SB DOS starts a program it loaded: the whole 32KB window on the RAM
			SetCpuMemoryMapping(0x8000, 0xFFFF, _dram.data(), (_prgReg & 0x07) * 0x8000, DramSize, MemoryAccessType::ReadWrite);
		} else if(_mode & 0x04) {
			SelectPrgPage2x(0, (_prgReg & 0x3F) << 1);
		} else if(_hasFloppy && (_prgReg & 0x80)) {
			SetCpuMemoryMapping(0x8000, 0xBFFF, _dram.data(), (_prgReg & 0x0F) * 0x4000, DramSize, MemoryAccessType::ReadWrite);
			SelectPrgPage(1, 0);
		} else {
			SelectPrgPage(0, _prgReg & 0x7F);
			SelectPrgPage(1, 0);
		}
		SetMirroringType(_mode & 0x01 ? MirroringType::Horizontal : MirroringType::Vertical);
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		if(addr == 0x4016) {
			//With the printer on the line it reads ready; with a pad on it, the pad's data - which
			//the print routine takes for "not ready" and reports as a printer error
			uint8_t value = ((NesControlManager*)_console->GetControlManager())->ReadRam(addr);
			bool printer = _printerAlways || _console->GetNesConfig().Yuyin2Printer;
			return printer ? ((value & 0xFE) | 0x01) : value;
		}
		if(_isSuborCart && (addr & 0xFF00) == 0x5300) {
			return (_lpcAudio->IsBusy() ? 0x00 : 0x80) | (_lpcAudio->IsSpeechEnd() ? 0x0F : 0x00);
		}
		if(_hasFloppy && (addr & 0xFF00) == 0x5600) {
			CheckForDiskImage();
			_fdc.MarkActivity();
			uint8_t value = _fdc.Read((uint8_t)(addr & 0x07));
			UpdateFdcIrq();
			return value;
		}
		//On the 256KB multicart bit 6 takes the ROM off the bus. The bigger Subor cartridges use it
		//as a bank bit - a 1MB one reads its font with it set, and a 2MB one's add-on card is there.
		bool romOff = (_prgReg & 0x40) && GetPrgPageCount() <= 16;
		return romOff ? _console->GetMemoryManager()->GetOpenBus() : InternalReadRam(addr);
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr == 0x480F) {
			Printer().WriteData(value);
			return;
		}
		if(_isSuborCart && (addr & 0xFF00) == 0x5300) {
			_lpcAudio->WriteData(value);
			return;
		}
		if(_hasFloppy && (addr & 0xFFF8) == 0x5500 && (addr & 0x07) != 0) {
			//$5500 itself stays a plain register; SB DOS writes the digital output register at
			//$5501, where a PC has it at offset 2
			CheckForDiskImage();
			_fdc.MarkActivity();
			uint8_t reg = (uint8_t)(addr & 0x07);
			_fdc.Write(reg == 1 ? 2 : reg, value);
			UpdateFdcIrq();
			return;
		}
		if(addr == 0x5000) {
			_prgReg = value;
			UpdateState();
		} else if(addr == 0x5200) {
			_mode = value;
			if(_mode & 0x04) {
				UpdateState();
			}
		}
	}

public:
	bool HasFloppyDrive() { return _hasFloppy; }

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

	uint32_t GetDiskCount() { return (uint32_t)GetDiskFileList().size(); }

	string GetCurrentDiskFilename() { return _fdc.IsDiskInserted() ? _fdc.GetDiskFilename() : ""; }

	void EjectDisk()
	{
		if(!_hasFloppy) {
			return;
		}
		auto lock = _emu->AcquireLock();
		if(_fdc.IsDiskInserted()) {
			MessageManager::DisplayMessage("Subor", "Disk ejected: " + FolderUtilities::GetFilename(_fdc.GetDiskFilename(), true));
			_fdc.EjectDisk(); //Saves pending changes first
		}
		_persistedDiskRom.clear();
		_persistedDiskPath.clear();
	}

	void InsertDisk(uint32_t index)
	{
		if(!_hasFloppy) {
			return;
		}
		auto lock = _emu->AcquireLock();
		vector<string> disks = GetDiskFileList();
		if(index < disks.size()) {
			_fdc.EjectDisk();
			if(_fdc.LoadDiskImage(disks[index])) {
				_diskChecked = true;
				_persistedDiskRom = _emu->GetRomInfo().RomFile.GetFilePath();
				_persistedDiskPath = disks[index];
				MessageManager::DisplayMessage("Subor", "Disk inserted: " + FolderUtilities::GetFilename(disks[index], true));
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
		InsertDisk((uint32_t)((current + 1) % disks.size()));
	}
};