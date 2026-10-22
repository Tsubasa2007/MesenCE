#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/NesConsole.h"
#include "NES/NesCpu.h"
#include "NES/NesMemoryManager.h"
#include "NES/Mappers/Bbk/PcFdc.h"
#include "NES/Mappers/Bbk/BbkLpcAudio.h"
#include "NES/Mappers/Bbk/BbkPrinter.h"
#include "Shared/NotificationManager.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/Serializer.h"

//八通 BT-686, the 华通 (Huatong) learning machine, with its 系统程序卡 (system program card) in the
//cartridge slot. Read off the card's code; there is no reference emulator for it.
//
//- The image is all PRG: the machine's built-in card (BT686-3: a 512KB mask ROM, 8KB and 32KB of
//  SRAM, a 74LS273), then the system card (512KB, a 27C040 behind a 74LS273). Both are the CPU's:
//  the mask ROM holds the reset code, the 16x16 font, the floppy boot loader and other data, and
//  the PPU sees only CHR RAM, 32KB of it. An image of the mask ROM alone is the machine with its
//  cartridge slot empty.
//- $4100-$410F: eight one-bit latches, set by any access - A1-A3 pick the latch, A0 is the value.
//  Latches 0 and 1 pick what $8000-$FFFF shows, in the 32KB page $4500 selects:
//    1 clear, 0 clear - the built-in mask ROM
//    1 set, 0 clear   - the system card, open bus without one
//    1 clear, 0 set   - the RAM card, 128KB: HT-DOS's DISKCOPY says "You ram is not 128k"
//                       when it is missing, the input method programs refuse to run with
//                       less, and a course disk only runs with exactly that (below)
//    1 set, 0 set     - another slot, empty here
//  Everything powers up clear, so the machine starts in page 0 of the mask ROM. Its reset code
//  clears latches 3-7 and then looks for something to run, three times over: the fourth slot,
//  the system card and RAM, each taken if its vectors are not $FFFF and (for the last two) $FFFC
//  cannot be written - and with none, the floppy boot in the mask ROM's last page.
//  Latch 3 is the mirroring, set = horizontal: the program launcher, finding a YuXing program
//  (one that writes $55xx), copies bit 3 of the value it writes there - the YuXing machines'
//  horizontal mirroring bit. The boot clears it, and the menu, which never sets it, draws its
//  labels in the left name table and a plain screen in the right one: vertical.
//  Latch 2 bands the screen, as the 华通 card for the YuXing machines does (see
//  YuxingMapper::SetHuatongBand): the four bands of eight tile rows each take $1000-$1FFF from
//  their own 4KB of CHR RAM, and which one is shown is the last name table row on the PPU bus -
//  the drawing's own fetches row by row, and software pointing $2006 at $2000, $2100, $2200 or
//  $2300 before it uploads a band's tiles. HT-DOS's console and its programs' full-screen text
//  are drawn this way.
//  Latches 4 and 5 the launcher sets from a code in a loaded program's header ($FFFE/$FFFF, a
//  byte and its complement), comparing the code's two nibbles; both clear without one. With
//  latch 5 set, RAM's $C000-$FFFF stays on the card's last 16KB whatever $4500 says, and only
//  $8000-$BFFF follows it: a course disk's program, loaded into the top of RAM, sets latch 5 and
//  switches $4500 while running from $C000, and keeps calling into the system card with it set
//  - which still pages 32KB at a time. What latch 4 switches is not known, nor latch 6.
//  Latch 7 is the printer's strobe (below); the BASICs also hold it low for a moment before
//  they first talk to the speech chip.
//- $4200/$4201/$4205 and $4304/$4305: the floppy controller, at the YuXing machines' addresses.
//- $4600: the speech chip - the YuXing machines' LPC decoder and coefficient set, fed through
//  a 4-bit port. Each byte goes as two writes of $C0 | nibble, the low one first, with the card
//  waiting for bit 7 (ready) before each; a phrase ends with $DF, $DF and $00. Unlike the
//  YuXing's, the bytes are not scrambled. The phrases are in the built-in mask ROM. That is the
//  V3.0 system card; V2.0 writes whole bytes instead, each phrase as it is stored - a header
//  ended by $FC (which V3.0 skips), the frames, then 24 bytes of $FF - and at power-on writes
//  $FF three times, waiting for ready after each, to see that the chip is there.
//- $4700 and latch 7: the printer. The byte goes out a bit at a time, eight writes to $4700 of
//  the byte shifted left after each (bit 7 is the one taken), and latch 7 pulsed low and high
//  strobes it. Bit 0 of $4017 is the printer's ready line, which the word processor waits for
//  before each byte; nothing here drives it, but with the machine's standard pad in port 2 it
//  reads 1 by then, the keyboard scan having clocked the pad's report out. It prints Epson
//  24-pin bit images (see BbkPrinter), saved as PNG pages.
//- $6000-$7FFF: 8KB of RAM.
//- The keyboard is a Subor keyboard on $4016/$4017 (the card scans it with $04/$06).
class HuatongBt686 : public BaseMapper
{
private:
	class DiskSwapListener final : public INotificationListener
	{
	private:
		HuatongBt686* _mapper;

	public:
		DiskSwapListener(HuatongBt686* mapper) : _mapper(mapper) {}

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

	static constexpr uint32_t RamSize = 0x20000;
	static constexpr uint32_t MaskRomSize = 0x80000;

	uint8_t _latches = 0;
	uint8_t _page = 0;
	uint8_t _band = 0;
	vector<uint8_t> _ram;
	PcFdc _fdc;
	unique_ptr<BbkLpcAudio> _lpcAudio;
	uint8_t _speechByte = 0;
	bool _speechHalf = false;
	//The V2.0 card's whole-byte stream (see WriteSpeechByte)
	bool _byteSpeech = false;
	bool _speechInHeader = true;
	uint8_t _speechStops = 0;
	bool _diskChecked = false;
	uint8_t _printShift = 0;
	//The page is named after the ROM, and that name only exists once the ROM is loaded
	BbkPrinter _printer;
	bool _printerNamed = false;
	shared_ptr<DiskSwapListener> _swapListener;
	//A power cycle recreates the mapper, so the disk in the drive is remembered here and put back
	//rather than the paired one - scoped to the BIOS it was put in under
	inline static string _persistedDiskRom;
	inline static string _persistedDiskPath;

	bool Latch(int i) { return (_latches >> i) & 0x01; }

	BbkPrinter& Printer()
	{
		if(!_printerNamed) {
			_printerNamed = true;
			_printer.SetRomName(FolderUtilities::GetFilename(_emu->GetRomInfo().RomFile.GetFilePath(), false));
		}
		return _printer;
	}

	string GetConfiguredDiskFolder()
	{
		const char* folder = _console->GetNesConfig().BbkDiskFolder;
		return folder[0] ? string(folder) : string();
	}

	//Puts the disk in at the first look at the drive, once the rom's path is known: the one left in
	//by the last session, else "<rom name>.img/.ima" beside it or in the disk folder
	void CheckForDiskImage()
	{
		if(_diskChecked) {
			return;
		}
		_diskChecked = true;

		string romPath = _emu->GetRomInfo().RomFile.GetFilePath();
		if(_persistedDiskRom == romPath && !_persistedDiskPath.empty() && _fdc.LoadDiskImage(_persistedDiskPath)) {
			MessageManager::Log("[BT-686] Re-mounted disk image: " + _persistedDiskPath);
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
						MessageManager::Log("[BT-686] Mounted disk image: " + diskPath);
						return;
					}
				}
			}
		}
	}

	void UpdateState()
	{
		uint8_t page = _page & 0x0F;
		if(Latch(1) && Latch(0)) {
			RemoveCpuMemoryMapping(0x8000, 0xFFFF);
		} else if(Latch(1)) {
			if(_prgSize >= MaskRomSize * 2) {
				SelectPrgPage2x(0, (MaskRomSize >> 14) + (page << 1));
			} else {
				RemoveCpuMemoryMapping(0x8000, 0xFFFF);
			}
		} else if(!Latch(0)) {
			SelectPrgPage2x(0, page << 1);
		} else {
			SetCpuMemoryMapping(0x8000, 0xBFFF, _ram.data(), (page * 0x8000) % RamSize, RamSize, MemoryAccessType::ReadWrite);
			uint32_t upper = Latch(5) ? RamSize - 0x4000 : (page * 0x8000 + 0x4000) % RamSize;
			SetCpuMemoryMapping(0xC000, 0xFFFF, _ram.data(), upper, RamSize, MemoryAccessType::ReadWrite);
		}
		SetMirroringType(Latch(3) ? MirroringType::Horizontal : MirroringType::Vertical);
		UpdateChr();
	}

	bool IsBanded() { return Latch(2); }

	void UpdateChr()
	{
		SelectChrPage(0, 0, ChrMemoryType::ChrRam);
		SelectChrPage(1, IsBanded() ? _band * 2 + 1 : 1, ChrMemoryType::ChrRam);
	}

protected:
	uint16_t GetPrgPageSize() override { return 0x4000; }
	uint16_t GetChrPageSize() override { return 0x1000; }
	uint32_t GetChrRamSize() override { return 0x8000; }
	uint16_t GetChrRamPageSize() override { return 0x1000; }
	uint32_t GetWorkRamSize() override { return 0x2000; }
	uint32_t GetWorkRamPageSize() override { return 0x2000; }
	bool ForceWorkRamSize() override { return true; }
	uint32_t GetSaveRamSize() override { return 0; }
	uint16_t RegisterStartAddress() override { return 0x4100; }
	uint16_t RegisterEndAddress() override { return 0x4700; }
	bool AllowRegisterRead() override { return true; }
	bool EnableCpuClockHook() override { return true; }
	bool EnableVramAddressHook() override { return true; }

	//Dendy timing, as the YuXing machines'. The word processor draws its menu bar from the right
	//name table and switches to the left one a fixed delay after the NMI that follows its
	//screen updates. With the NMI 20 lines before the frame, the switch lands on line 17, under
	//the bar's 16-pixel text, at the same point every frame. With PAL's 70-line vblank it came on
	//line 11, cutting the tops off the bar's text, and within a few dots of the PPU's
	//line advance, so the picture jumped a line whenever the updates took longer.
	//313 lines rather than 312, as on the YuXing machines: the boot waits for vblank by polling
	//$2002 while its NMI handler reads it too, and a frame of a whole number of CPU cycles
	//(312 lines) keeps the poll losing that race for ever.
	int32_t GetDendyScanlineCount() override { return 313; }
	int32_t GetDendyNmiScanline() override { return 292; }

	void InitMapper() override
	{
		RemoveRegisterRange(0x4100, 0x4700, MemoryOperation::Any);
		AddRegisterRange(0x4100, 0x410F, MemoryOperation::Any);
		AddRegisterRange(0x4200, 0x4207, MemoryOperation::Write);
		AddRegisterRange(0x4300, 0x4307, MemoryOperation::Read);
		AddRegisterRange(0x4500, 0x4500, MemoryOperation::Write);
		AddRegisterRange(0x4600, 0x4600, MemoryOperation::Any);
		AddRegisterRange(0x4700, 0x4700, MemoryOperation::Write);

		//Blank, not zero: the word processor counts a RAM page as a saved file when the page's
		//checksum bytes match the sum and XOR of its contents, which an all-zero page does - every
		//page then listed as a file with an empty name
		_ram.assign(RamSize, 0xFF);
		SetCpuMemoryMapping(0x6000, 0x7FFF, 0, PrgMemoryType::WorkRam);
		_fdc.Reset();
		_fdc.SetSilentWithoutDisk(true);
		_diskChecked = false;
		//The card writes the end of a phrase the moment the chip takes the last byte, while
		//it still holds a FIFO's worth to speak - the reset waits for those, as on the Dr. PC Jr.
		_lpcAudio.reset(new BbkLpcAudio(_console, BbkLpcAudio::LpcVariant::Yuxing));
		_lpcAudio->DeferResetUntilSpoken();
		_lpcAudio->Reset();
		_byteSpeech = _romInfo.Hash.PrgCrc32 == V20Crc;
		_speechInHeader = true;
		_speechStops = 0;
		if(!_swapListener) {
			_swapListener.reset(new DiskSwapListener(this));
			_emu->GetNotificationManager()->RegisterNotificationListener(_swapListener);
		}
		UpdateState();
	}

	void Reset(bool softReset) override
	{
		//The reset line clears the latches and the page register: page 0 of the mask ROM
		_latches = 0;
		_page = 0;
		_printer.Reset();
		UpdateState();
	}

	//The band is the last name table row on the bus, whoever put it there. Not the attribute
	//bytes: they all sit in the last band's rows.
	void NotifyVramAddressChange(uint16_t addr) override
	{
		if(IsBanded() && addr >= 0x2000 && addr < 0x3000 && (addr & 0x3FF) < 0x3C0) {
			uint8_t band = (uint8_t)((addr >> 8) & 0x03);
			if(band != _band) {
				_band = band;
				UpdateChr();
			}
		}
	}

	void ProcessCpuClock() override
	{
		BaseProcessCpuClock();
		_fdc.Clock();
		_lpcAudio->Clock();
		_printer.Clock();
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		if(addr <= 0x410F) {
			SetLatch(addr);
			return _console->GetMemoryManager()->GetOpenBus();
		}
		if((addr & 0xFFF8) == 0x4300) {
			CheckForDiskImage();
			_fdc.MarkActivity();
			return _fdc.Read((uint8_t)(addr & 0x07));
		}
		if(addr == 0x4600) {
			return _lpcAudio->IsReady() ? 0x80 : 0x00;
		}
		return _console->GetMemoryManager()->GetOpenBus();
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr <= 0x410F) {
			SetLatch(addr);
		} else if((addr & 0xFFF8) == 0x4200) {
			CheckForDiskImage();
			_fdc.MarkActivity();
			switch(addr & 0x07) {
				case 0: _fdc.Write(7, value); break;
				case 1: _fdc.Write(2, value); break;
				case 5: _fdc.Write(5, value); break;
				default: break;
			}
		} else if(addr == 0x4500) {
			_page = value;
			UpdateState();
		} else if(addr == 0x4600) {
			if(_byteSpeech) {
				WriteSpeechByte(value);
			} else {
				WriteSpeech(value);
			}
		} else if(addr == 0x4700) {
			_printShift = (uint8_t)((_printShift << 1) | (value >> 7));
		}
	}

	void WriteSpeech(uint8_t value)
	{
		if((value & 0xF0) == 0xC0) {
			if(!_speechHalf) {
				_speechByte = value & 0x0F;
			} else {
				_lpcAudio->WriteData((uint8_t)(_speechByte | (value << 4)));
			}
			_speechHalf = !_speechHalf;
		} else if(value == 0x00) {
			//The end of a phrase: the next one starts from a reset decoder
			_speechHalf = false;
			_lpcAudio->WriteControl(0);
			_lpcAudio->WriteControl(1);
		}
	}

	//V2.0: the header goes up to its $FC, the frames go to the decoder, and the run of $FF
	//after them ends the phrase. A single $FF can be frame data, so the run has to reach a few
	//bytes before it counts - the ones before that are stop codes the decoder ends on anyway.
	void WriteSpeechByte(uint8_t value)
	{
		if(_speechInHeader) {
			if(value == 0xFC) {
				_speechInHeader = false;
				_speechStops = 0;
			}
			return;
		}
		if(value == 0xFF && ++_speechStops == 4) {
			_speechInHeader = true;
			_lpcAudio->WriteControl(0);
			_lpcAudio->WriteControl(1);
			return;
		} else if(value != 0xFF) {
			_speechStops = 0;
		}
		_lpcAudio->WriteData(value);
	}

	void SetLatch(uint16_t addr)
	{
		uint8_t bit = (uint8_t)(1 << ((addr >> 1) & 0x07));
		uint8_t latches = (addr & 0x01) ? (uint8_t)(_latches | bit) : (uint8_t)(_latches & ~bit);
		if(latches != _latches) {
			if((_latches & 0x80) && !(latches & 0x80)) {
				Printer().WriteData(_printShift);
			}
			_latches = latches;
			UpdateState();
		}
	}

	void SaveBattery() override
	{
		BaseMapper::SaveBattery();
		if(_fdc.IsDirty()) {
			_fdc.SaveDiskImage();
		}
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SV(_latches);
		SV(_page);
		SV(_band);
		SVVector(_ram);
		SV(_fdc);
		SV(_lpcAudio);
		SV(_speechByte);
		SV(_speechHalf);
		SV(_speechInHeader);
		SV(_speechStops);
		SV(_printShift);
		SV(_printer);
		if(!s.IsSaving()) {
			UpdateState();
		}
	}

public:
	//PRG CRC32s: the mask ROM followed by each system card, and the mask ROM alone
	static constexpr uint32_t V20Crc = 0x09F6C5CD;
	static constexpr uint32_t V30Crc = 0xE451E5A7;
	static constexpr uint32_t NoCardCrc = 0xB4300B49;

	static bool IsBt686(uint32_t prgCrc) { return prgCrc == V20Crc || prgCrc == V30Crc || prgCrc == NoCardCrc; }

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
		auto lock = _emu->AcquireLock();
		if(_fdc.IsDiskInserted()) {
			MessageManager::DisplayMessage("BT-686", "Disk ejected: " + FolderUtilities::GetFilename(_fdc.GetDiskFilename(), true));
			_fdc.EjectDisk(); //Saves pending changes first
		}
		_persistedDiskRom.clear();
		_persistedDiskPath.clear();
	}

	void InsertDisk(uint32_t index)
	{
		auto lock = _emu->AcquireLock();
		vector<string> disks = GetDiskFileList();
		if(index < disks.size()) {
			_fdc.SaveDiskImage();
			if(_fdc.LoadDiskImage(disks[index])) {
				_diskChecked = true;
				_persistedDiskRom = _emu->GetRomInfo().RomFile.GetFilePath();
				_persistedDiskPath = disks[index];
				MessageManager::DisplayMessage("BT-686", "Disk inserted: " + FolderUtilities::GetFilename(disks[index], true));
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
