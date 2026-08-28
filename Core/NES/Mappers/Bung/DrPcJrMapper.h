#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/NesConsole.h"
#include "NES/NesCpu.h"
#include "NES/NesMemoryManager.h"
#include "NES/NesControlManager.h"
#include "NES/NesPpu.h"
#include "NES/Input/Sb2kKeyboard.h"
#include "NES/Input/Sb2kMouse.h"
#include "NES/NesControlManager.h"
#include "NES/Mappers/Bbk/PcFdc.h"
#include "NES/Mappers/Bbk/BbkPrinter.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include "Shared/NotificationManager.h"
#include "Shared/Emulator.h"
#include "Shared/SystemActionManager.h"
#include <algorithm>
#include "Utilities/Serializer.h"

//Bung Doctor PC Jr. family - reached as mapper 173 here, the way the VirtuaNES-BBK fork
//numbers it. Standard mapper 173 is Idea-Tek, so the two are told apart by size: a real
//173 cart is small and carries CHR ROM, while these machines are a 128KB BIOS with none.
//Ported from that fork's MapperDrPCJr, whose own base is NintendulatorNRS by NewRisingSun.
//
//Three BIOS revisions share the board, told apart by PRG CRC32. This port covers the
//KW2000 (KW-SC2000), CRC 9982BF45.
//
//STATE OF THIS PORT: the BIOS's own boot path only. The machine also has a floppy
//controller, an LPC speech chip, an AT keyboard and a whole second personality for running
//cartridge images, none of which is here yet - the register notes below say where each
//one attaches.
//
//Memory, all of it RAM except the BIOS:
//
// - 128KB BIOS ROM, addressed in 4KB pages
// - 512KB PRG-RAM, banked as 32KB into $6000-$DFFF (one bank IS that whole window)
// - 512KB CHR-RAM
// - 32KB save RAM, banked as 8KB into $6000-$7FFF when $4180 bit 7 asks for it
//
//Registers are $4180-$41BF, stored verbatim as regs[$00-$3F]; the ones this port acts on:
//
// - $4180 bit 7   swap save RAM into $6000-$7FFF; bits 0-2 pick the $E000 BIOS page
// - $4182         mirroring, applied through MirrorSync
// - $4184/$4186  parallel printer: $4184 is the data byte, $4186 bit 0 the strobe
// - $4188/89/8B   floppy controller: $4188 is the main status register on read and the
//                 data rate select on write, $4189 the data register, $418B the digital
//                 output register - see PcFdc
// - $418E/$418F   AT keyboard interface, clock and data lines of a PS/2 style link
// - $4190/$4191   PRG-RAM bank: $4190 is the READ bank and $4191 the WRITE bank. They are
//                 genuinely separate, which is why writes into the window are intercepted
//                 rather than mapped - Mesen keeps one page pointer per address, so a
//                 read-here/write-there window cannot be expressed in the page table.
// - $4181 bits 0-1 how wide a bank each of $4198-$419F names: 1KB, 2KB, 4KB, or 8KB
//                 from $4198 alone
// - $4198-$419F   CHR-RAM banks, at the width $4181 selects
// - $418D bits 0-1 the per-cell tile latch: 2 draws 2bpp tiles from a per-cell 4KB bank,
//                 3 draws 1bpp glyphs from a 2KB bank and colours them per cell
// - $41A1/$41A2   IRQ counter, low and high
// - $41A3         IRQ enable
// - $41AC         speech chip (not implemented - reports ready so the BIOS does not wait)
// - $41AF bits 0-1 pick the 32KB BIOS group the $E000/$F000 pages come from
// - $41A5         leaves load mode, entering the cartridge personality (not implemented)
// - $42FC-$42FF   enters game mode and sets mirroring (not implemented)
class DrPcJrMapper : public BaseMapper
{
public:
	static bool IsKw2000(uint32_t prgCrc) { return prgCrc == 0x9982BF45; }
	static bool IsKw3000(uint32_t prgCrc) { return prgCrc == 0xCFF8F964; }

private:
	//The FDS disk shortcuts, reused to eject and swap floppies - the same arrangement
	//YuxingMapper uses for its media.
	class DiskSwapListener final : public INotificationListener
	{
	private:
		DrPcJrMapper* _mapper;

	public:
		DiskSwapListener(DrPcJrMapper* mapper) : _mapper(mapper) {}

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

	shared_ptr<DiskSwapListener> _swapListener;
	int32_t _diskIndex = -1;

	//A power cycle rebuilds the mapper, so the disk the user chose has to outlive it, or the
	//machine goes back to whatever is paired with the rom by name - and when nothing is
	//paired, boots with an empty drive, which looks exactly like the selection having done
	//nothing. Keyed by rom path so another machine does not inherit it; an empty path means
	//"ejected on purpose", which must also survive.
	inline static string _persistedDiskRom;
	inline static string _persistedDiskPath;
	inline static bool _persistedDiskValid = false;

	//$4181 bits 4-5. Only MachineNew is ported - the other three are cartridge modes.
	static constexpr uint8_t MachineOld = 0;
	static constexpr uint8_t MachineNew = 1;

	//The 32KB SRAM lives at the top of the work RAM allocation
	static constexpr uint32_t SramBase = 0x80000;

	uint8_t _regs[0x40] = {};

	//The whole of $4020-$5FFF is RAM on this board - the reference reaches it through its
	//generic bank pointer, so every address in the range that is not one of the registers
	//above still reads back what was written to it. The BIOS does use it: it writes $4200
	//during start-up. Leaving the range unmapped instead makes those reads open bus.
	uint8_t _exRam[0x2000] = {};
	bool _loadMode = true;
	uint16_t _irqCounter = 0;
	bool _irqEnabled = false;
	uint8_t _irqStatus = 0;

	//--- per-tile CHR banking ("external latch") -----------------------------------------
	//How this machine gets more than 256 tiles on screen. $418D bit 1 turns it on; with it
	//set, every write to a nametable also records the CHR bank that was current at the time
	//($4194) into a shadow of that nametable. At render time each tile's pattern is then
	//fetched from the 4KB bank its own cell recorded, instead of from the pattern table.
	//
	//Mechanically this is MMC5's extended-attribute trick, and Mesen's hook for it is the
	//same: watch for a nametable fetch, then substitute the two pattern fetches that follow.
	//The attribute fetch in between is left alone - unlike MMC5 this machine takes its
	//attributes from the normal attribute table.
	//
	//Not ported: the reference's `bAUTO` path (a second bank source used by one title's
	//logo screen) and its per-title special cases, which its own comments mark as such.
	//The splash screen ("Bung Wins 98") banks a 4KB CHR page in from the nametable address
	//itself: while $4194 sits in $40-$43, entering $2000-$2FFF selects a page derived from
	//it plus which nametable is being fetched. The reference calls this its LOGO case, and
	//sets a flag that also changes which bank the latch below uses.
	//--- mouse -------------------------------------------------------------------------
	//There is no serial protocol here: the machine's BIOS expects a three-byte report to
	//have been deposited at $FFAB-$FFAD by the time it looks, so the mapper writes one
	//into PRG-RAM every frame. $4016 turns it on - which is also the controller strobe, so
	//the report only lands while the last value written there was non-zero. That is what
	//the reference does, oddities included; software that never raises the strobe simply
	//never sees the mouse.
	bool _mouseEnabled = false;
	uint32_t _mouseFrame = 0;

	void MousePoll()
	{
		//$FFAB-$FFAD are PRG-RAM through the system-mode banking, so there is nowhere to
		//put the report until the machine has left load mode
		if(!_mouseEnabled || _loadMode) {
			return;
		}

		uint32_t frame = _console->GetPpu()->GetFrameCount();
		if(frame == _mouseFrame) {
			return;
		}
		_mouseFrame = frame;

		shared_ptr<Sb2kMouse> mouse = _console->GetControlManager()->GetControlDevice<Sb2kMouse>();
		if(!mouse) {
			return;
		}

		int8_t dx, dy;
		uint8_t buttons;
		mouse->TakeDelta(dx, dy, buttons);

		//Byte 0 carries both buttons and the top two bits of each delta; the other two
		//carry the low six bits. The high bits are the machine's own framing.
		uint8_t bx = (uint8_t)dx;
		uint8_t by = (uint8_t)dy;
		uint8_t report[3] = {
			(uint8_t)(0xC0 | ((buttons & 0x01) << 5) | ((buttons & 0x02) << 3) | ((by & 0xC0) >> 4) | ((bx & 0xC0) >> 6)),
			(uint8_t)(0x80 | (bx & 0x3F)),
			(uint8_t)(0x80 | (by & 0x3F))
		};
		for(int i = 0; i < 3; i++) {
			uint32_t offset = SystemBankOffset((uint16_t)(0xFFAB + i));
			if(offset < _workRamSize) {
				_workRam[offset] = report[i];
			}
		}
	}

	uint8_t _ntData = 0;
	bool _logoMode = false;

	//With this set the per-tile bank comes from a table the machine keeps in the top pages
	//of CHR-RAM, rather than from the nametable-write shadow (which holds one constant value
	//and so can only ever draw one bank's worth of tiles). The machine turns it on by writing
	//CHR at $1800-$1FFF while $4194 is $7E and $4198 is $3F - which is what its graphical
	//screens do, and why they came out with the wrong tiles until this was here.
	bool _autoBank = false;

	uint8_t _exRamNt[0x800] = {};
	uint16_t _extNtAddr = 0;
	int8_t _extFetchCounter = 0;

	//$418D bit 1 turns the latch on; bit 0 then picks between the two tile formats -
	//mode 2 draws ordinary 2bpp tiles out of a per-cell 4K bank, mode 3 draws 1bpp
	//glyphs out of a 2K bank and colours them from the per-cell byte.
	bool ExtLatchEnabled() { return (_regs[0x0D] & 0x03) >= 2; }
	uint8_t ChrMask4K() { return (uint8_t)(((_regs[0x03] & 0x0F) << 3) | 7); }
	uint8_t ChrMask2K() { return (uint8_t)(((_regs[0x03] & 0x0F) << 4) | 15); }

	//A .CDV is a game rather than a machine: it brings its own register settings, and its
	//PRG and CHR are what the BIOS would otherwise have loaded from a floppy. Kept from the
	//rom so a power cycle can lay them down again.
	vector<uint8_t> _cdvHeader;
	vector<uint8_t> _cdvTrainer;
	vector<uint8_t> _cdvPrg;
	vector<uint8_t> _cdvChr;
	bool IsCdv() { return !_cdvHeader.empty(); }
	bool _cdvApuReady = false;

	//Which BIOS this is. Several of the latch's branches are chosen by it, because the
	//three machines format their per-cell tables differently.
	//  0 = Dr. PC Jr. BIOS 1.0a/1.5a, 1 = KW2000 (KW-SC2000), 2 = KW3000
	uint8_t _romType = 0;

	//Which operating system the mounted floppy carries, from a signature in its boot
	//area - the same three the reference machine recognises.
	//  0 = none/unknown, 1 = BUNG EC_CE, 2 = BUNG/KW DOS, 3 = KW WINS98
	uint8_t _diskType = 0;

	void DetectDiskType()
	{
		_diskType = 0;
		const vector<uint8_t>& disk = _fdc.GetDiskData();
		if(disk.size() < 0x4200) {
			return;
		}

		//The signature always sits in the same 7KB window past the boot sector
		auto has = [&](const char* tag) {
			size_t len = strlen(tag);
			for(size_t i = 0x2600; i + len <= 0x2600 + 0x1C00 && i + len <= disk.size(); i++) {
				if(memcmp(disk.data() + i, tag, len) == 0) {
					return true;
				}
			}
			return false;
		};

		if(has("FCEC_CE")) {
			_diskType = 1;
		} else if(has("SMDOS") && has("MCCDOS")) {
			_diskType = 2;
		} else if(has("JR_WINFD") && has("BUNGWINS")) {
			_diskType = 3;
		}
	}

	//Cycles 257-320 are the sprite pattern fetches. They run through the same read hook
	//as the background, but the latch must not touch them - substituting a background
	//bank into a sprite tile is what defaced the splash screens.
	bool IsBackgroundFetch()
	{
		uint32_t cycle = _console->GetPpu()->GetCurrentCycle();
		return cycle <= 256 || cycle >= 321;
	}

	//The shadow is two 1KB pages, picked by the low bit of the nametable index
	static uint16_t ExRamIndex(uint16_t addr) { return (uint16_t)((((addr >> 10) & 1) << 10) | (addr & 0x3FF)); }

	//--- printer -----------------------------------------------------------------------
	//A parallel port in the plainest form: $4184 holds the byte and $4186 bit 0 is the
	//strobe, pulsed once per byte. The word processor drives it with Epson ESC/P bit
	//images - it rasterises its own Chinese glyphs rather than relying on a font in the
	//printer - which is exactly what BbkPrinter already renders for the other machines.
	BbkPrinter _printer;
	bool _printerNamed = false;
	uint8_t _lptData = 0;
	uint8_t _lptCtrl = 0;

	//The page is named after the ROM, and that name only exists once the ROM is loaded
	BbkPrinter& Printer()
	{
		if(!_printerNamed) {
			_printerNamed = true;
			_printer.SetRomName(FolderUtilities::GetFilename(_emu->GetRomInfo().RomFile.GetFilePath(), false));

			//This machine's driver sends no line feeds: it sets the line spacing to one
			//graphics row and relies on the carriage return to advance the paper.
			_printer.SetAutoLineFeed(true);
		}
		return _printer;
	}

	void WriteLptCtrl(uint8_t value)
	{
		//Latch on the leading edge of the strobe
		if((value & 0x01) && !(_lptCtrl & 0x01)) {
			Printer().WriteData(_lptData);
		}
		_lptCtrl = value;
	}

	//--- floppy ------------------------------------------------------------------------
	//The same PC-compatible controller the YuXing machines drive - see PcFdc, which says
	//why these BIOSes need that model rather than BbkFdc. The BIOS reaches it through three
	//of the mapper's own registers instead of a port range: $4188 is the main status
	//register on read and the data rate select on write, $4189 the data register, and
	//$418B the digital output register.
	PcFdc _fdc;
	bool _floppyChecked = false;

	NesControlManager* NesControls() { return (NesControlManager*)_console->GetControlManager(); }

	//Where the mounted disk sits in the swap list, so "next disk" continues from it
	void RememberDiskIndex(const string& diskPath)
	{
		vector<string> disks = GetDiskFileList();
		string leaf = FolderUtilities::GetFilename(diskPath, true);
		for(size_t i = 0; i < disks.size(); i++) {
			if(FolderUtilities::GetFilename(disks[i], true) == leaf) {
				_diskIndex = (int32_t)i;
				return;
			}
		}
	}

	//Settings -> NES -> the BBK disk folder, shared with the other learning machines here
	string GetConfiguredDiskFolder()
	{
		const char* folder = _console->GetNesConfig().BbkDiskFolder;
		return folder[0] ? string(folder) : string();
	}

	//Paired by name with the rom, the way the other machines here mount their media.
	//Deferred to the first bus access, as YuxingMapper does: the emulator's rom info is not
	//filled in yet during InitMapper OR during the first CPU cycles - asking that early
	//gives an empty path and the disk is silently never mounted.
	void MountPairedFloppy()
	{
		if(!_swapListener) {
			_swapListener.reset(new DiskSwapListener(this));
			_emu->GetNotificationManager()->RegisterNotificationListener(_swapListener);
		}

		if(_floppyChecked) {
			return;
		}
		_floppyChecked = true;

		string romPath = _emu->GetRomInfo().RomFile.GetFilePath();

		if(_persistedDiskValid && _persistedDiskRom == romPath) {
			if(_persistedDiskPath.empty()) {
				return; //ejected before the power cycle - leave the drive empty
			}
			if(_fdc.LoadDiskImage(_persistedDiskPath)) {
				MessageManager::Log("[Dr.PC Jr.] Re-inserted disk: " + _persistedDiskPath);
				DetectDiskType();
				RememberDiskIndex(_persistedDiskPath);
				return;
			}
		}

		string baseName = FolderUtilities::GetFilename(romPath, false);
		vector<string> folders = { FolderUtilities::GetFolderName(romPath) };
		string configured = GetConfiguredDiskFolder();
		if(!configured.empty() && configured != folders[0]) {
			folders.push_back(configured);
		}

		for(string& folder : folders)
		for(string ext : { ".img", ".IMG", ".ima", ".IMA" }) {
			string diskPath = FolderUtilities::CombinePath(folder, baseName + ext);
				if(_fdc.LoadDiskImage(diskPath)) {
				MessageManager::Log("[Dr.PC Jr.] Mounted disk image: " + diskPath);
				DetectDiskType();
				RememberDiskIndex(diskPath);
				return;
			}
		}
	}

public:
	//Disk swapping - driven by the FDS disk shortcut keys via DiskSwapListener, and listed
	//in the UI through NesConsole::GetBbkDiskList
	uint32_t GetDiskCount()
	{
		return (uint32_t)GetDiskFileList().size();
	}

	//Full path of the disk currently in the drive, or "" when the drive is empty
	string GetCurrentDiskFilename()
	{
		return _fdc.IsDiskInserted() ? _fdc.GetDiskFilename() : "";
	}

	//Every floppy image sitting beside the rom, in name order - what the swap shortcuts
	//step through.
	vector<string> GetDiskFileList()
	{
		//Both the rom's own folder and the configured one, rather than only the configured
		//one: this machine's disks tend to sit beside the rom while that setting is pointed
		//at another machine's collection, and a list that silently excluded them is what
		//"no disk selection" looks like from the outside.
		vector<string> folders = { FolderUtilities::GetFolderName(_emu->GetRomInfo().RomFile.GetFilePath()) };
		string configured = GetConfiguredDiskFolder();
		if(!configured.empty() && configured != folders[0]) {
			folders.push_back(configured);
		}

		vector<string> files;
		for(string& folder : folders) {
			for(string& file : FolderUtilities::GetFilesInFolder(folder, { ".img", ".ima" }, false)) {
				files.push_back(file);
			}
		}
		std::sort(files.begin(), files.end());
		return files;
	}

	void EjectDisk()
	{
		auto lock = _emu->AcquireLock();
		if(_fdc.IsDiskInserted()) {
			MessageManager::DisplayMessage("Dr.PC Jr.", "Disk ejected: " +
				FolderUtilities::GetFilename(_fdc.GetDiskFilename(), true));
			_fdc.EjectDisk(); //Saves pending changes first
			_diskType = 0;
			_diskIndex = -1;
			_persistedDiskRom = _emu->GetRomInfo().RomFile.GetFilePath();
			_persistedDiskPath = "";
			_persistedDiskValid = true;
		}
	}

	void InsertDisk(uint32_t index)
	{
		auto lock = _emu->AcquireLock();
		vector<string> disks = GetDiskFileList();
		if(index >= disks.size()) {
			return;
		}

		//A floppy can be changed while the machine runs: the controller raises its
		//disk-change line and the BIOS picks the new disk up on its next access.
		_fdc.SaveDiskImage();
		if(_fdc.LoadDiskImage(disks[index])) {
			DetectDiskType();
			_floppyChecked = true;
			_diskIndex = (int32_t)index;
			_persistedDiskRom = _emu->GetRomInfo().RomFile.GetFilePath();
			_persistedDiskPath = disks[index];
			_persistedDiskValid = true;
			MessageManager::DisplayMessage("Dr.PC Jr.", "Disk inserted: " +
				FolderUtilities::GetFilename(disks[index], true));
		}
	}

	void InsertNextDisk()
	{
		vector<string> disks = GetDiskFileList();
		if(disks.empty()) {
			return;
		}
		InsertDisk((uint32_t)((_diskIndex + 1) % (int32_t)disks.size()));
	}

private:
	//--- AT keyboard -------------------------------------------------------------------
	//A PS/2 keyboard bit-banged over two lines in $418E: bit 0 is data and bit 1 is clock,
	//both driven by the host; reading the same register gives the keyboard's own two lines
	//back in the same bits. $418F reads the interrupt status and clears the keyboard's IRQ.
	//
	//Only the serial protocol lives here. Which keys are down comes from Sb2kKeyboard, the
	//same device the SB-2000 uses - it is not on the controller bus either, and it already
	//reports one transition at a time as scan-code set 1. This keyboard talks set 2, so the
	//codes are translated on the way out.
	static constexpr uint8_t KbdStateIdle = 0;
	static constexpr uint8_t KbdStateRecvRequest = 1;
	static constexpr uint8_t KbdStateRecvStartBit = 2;
	static constexpr uint8_t KbdStateRecvDataBit7 = 3;
	static constexpr uint8_t KbdStateRecvParityBit = 11;
	static constexpr uint8_t KbdStateRecvStopBit = 12;
	static constexpr uint8_t KbdStateRecvAck = 13;
	static constexpr uint8_t KbdStateSendStartBit = 14;
	static constexpr uint8_t KbdStateSendDataBit7 = 15;
	static constexpr uint8_t KbdStateSendParityBit = 23;
	static constexpr uint8_t KbdStateSendStopBit = 24;

	uint8_t _kbdCtrl = 0x03;
	bool _kbdClock = false;
	bool _kbdData = true;
	int32_t _kbdClockCount = 0;
	int32_t _kbdLatch = 0;
	int32_t _kbdParity = 0;
	uint8_t _kbdState = KbdStateIdle;
	bool _kbdRaiseIrq = false;
	uint8_t _kbdQueue[32] = {};
	uint8_t _kbdQueueLen = 0;
	uint32_t _kbdPollTimer = 0;

	void KbdPush(uint8_t value)
	{
		if(_kbdQueueLen < sizeof(_kbdQueue)) {
			_kbdQueue[_kbdQueueLen++] = value;
		}
	}

	uint8_t KbdPop()
	{
		uint8_t value = _kbdQueue[0];
		for(uint8_t i = 1; i < _kbdQueueLen; i++) {
			_kbdQueue[i - 1] = _kbdQueue[i];
		}
		if(_kbdQueueLen) {
			_kbdQueueLen--;
		}
		return value;
	}

	//Set 1 (what Sb2kKeyboard reports) to set 2 (what this keyboard sends)
	static uint8_t KbdSet1ToSet2(uint8_t code)
	{
		static constexpr uint8_t table[128] = {
			0x00, 0x76, 0x16, 0x1E, 0x26, 0x25, 0x2E, 0x36, 0x3D, 0x3E, 0x46, 0x45, 0x4E, 0x55, 0x66, 0x0D,
			0x15, 0x1D, 0x24, 0x2D, 0x2C, 0x35, 0x3C, 0x43, 0x44, 0x4D, 0x54, 0x5B, 0x5A, 0x14, 0x1C, 0x1B,
			0x23, 0x2B, 0x34, 0x33, 0x3B, 0x42, 0x4B, 0x4C, 0x52, 0x0E, 0x12, 0x5D, 0x1A, 0x22, 0x21, 0x2A,
			0x32, 0x31, 0x3A, 0x41, 0x49, 0x4A, 0x59, 0x7C, 0x11, 0x29, 0x58, 0x05, 0x06, 0x04, 0x0C, 0x03,
			0x0B, 0x83, 0x0A, 0x01, 0x09, 0x77, 0x7E, 0x6C, 0x75, 0x7D, 0x7B, 0x6B, 0x73, 0x74, 0x79, 0x69,
			0x72, 0x7A, 0x70, 0x71, 0x00, 0x00, 0x00, 0x78, 0x07, 0x00, 0x00, 0x1F, 0x27, 0x2F, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		return table[code & 0x7F];
	}

	//A byte the host clocked in - the few commands the reference answers
	void KbdHostCommand(uint8_t cmd)
	{
		switch(cmd) {
			case 0xFF:
				//Reset: acknowledge, then report the power-on self test passed
				_kbdQueueLen = 0;
				KbdPush(0xFA);
				KbdPush(0xAA);
				break;
			case 0xEE:
				KbdPush(0xEE);
				break;
			case 0xF2:
				//Identify: acknowledge, then the two-byte keyboard id
				KbdPush(0xFA);
				KbdPush(0xAB);
				KbdPush(0x83);
				break;
			default:
				//Everything else is acknowledged, including the argument byte of the
				//two-byte commands ($ED, $F0, $F3) - a real keyboard answers those the
				//same way. Staying silent instead leaves a host that waits for the
				//acknowledgement stuck in its setup loop forever.
				KbdPush(0xFA);
				break;
		}
	}

	void KbdPollKeys()
	{
		NesControlManager* controls = (NesControlManager*)_console->GetControlManager();
		shared_ptr<Sb2kKeyboard> kbd = controls->GetControlDevice<Sb2kKeyboard>();
		if(!kbd) {
			return;
		}

		int32_t keyEvent = kbd->GetNextKeyEvent();
		if(keyEvent < 0) {
			return;
		}

		uint8_t code = KbdSet1ToSet2((uint8_t)(keyEvent & 0xFF));
		if(!code) {
			return;
		}

		if(keyEvent & 0x100) {
			//Release: $F0 then the code
			KbdPush(0xF0);
		}
		KbdPush(code);
		_kbdRaiseIrq = true;
	}

	//The Bung machine's PPU is a famiclone part: it clears the vblank flag on a $2002 read
	//but has no 2C02 "read one dot before vblank" race. The software's main loop polls
	//$2002 in a tight wait, so with the race in play a third of the NMIs never fire, and
	//the frame handler - which is where this machine polls its keyboard - starves.
	bool EnablePpuNmiSuppressRace() override { return false; }

	//One step of the two-wire state machine, run once per CPU cycle. The lines change state
	//every 64 cycles, which is the ~35us the real device takes.
	void KbdClock()
	{
		if((_kbdCtrl & 0x01) && !(_kbdCtrl & 0x02)) {
			_kbdState = KbdStateIdle;
		}

		if(_kbdState == KbdStateIdle) {
			//Both wires are pulled up, so an idle line reads high. Leaving data wherever the
			//last byte left it reads as a start bit that never ends, and software that waits
			//for the line to go quiet before talking to the keyboard waits forever.
			_kbdData = true;
			if(!_kbdClock && !(_kbdClockCount++ & 0x3F)) {
				_kbdClock = true;
			}
			if((_kbdCtrl & 0x01) && !(_kbdCtrl & 0x02)) {
				_kbdState++;
			}
			if((_kbdCtrl & 0x03) == 0x03 && _kbdQueueLen) {
				_kbdState = KbdStateSendStartBit;
				_kbdClockCount = 0;
				_kbdClock = false;
			}
		} else if(_kbdState == KbdStateRecvRequest) {
			_kbdClock = true;
			if(!(_kbdCtrl & 0x01) && (_kbdCtrl & 0x02)) {
				_kbdState++;
				_kbdClockCount = 0;
				_kbdClock = false;
			}
			if((_kbdCtrl & 0x03) == 0x03 && _kbdQueueLen) {
				_kbdState = KbdStateSendStartBit;
				_kbdClockCount = 0;
				_kbdClock = false;
			}
		}

		if(_kbdState >= KbdStateRecvStartBit) {
			if((_kbdClockCount++ & 0x3F) != 0) {
				return;
			}
			_kbdClock = !_kbdClock;

			if(_kbdClock) {
				//Rising edge - the host's bits are sampled here
				if(_kbdState == KbdStateRecvStartBit) {
					if(_kbdCtrl & 0x01) {
						_kbdState = KbdStateIdle;
					} else {
						_kbdState++;
						_kbdParity = 0;
					}
				} else if(_kbdState >= KbdStateRecvDataBit7 && _kbdState < KbdStateRecvParityBit) {
					_kbdLatch >>= 1;
					if(_kbdCtrl & 0x01) {
						_kbdLatch |= 0x80;
					}
					_kbdParity += _kbdCtrl & 0x01;
					_kbdState++;
				} else if(_kbdState == KbdStateRecvParityBit) {
					_kbdParity += _kbdCtrl & 0x01;
					_kbdState++;
				} else if(_kbdState == KbdStateRecvStopBit) {
					if(!(_kbdCtrl & 0x01) || !(_kbdParity & 1)) {
						_kbdState = KbdStateIdle;
					} else {
						KbdHostCommand((uint8_t)_kbdLatch);
						_kbdState++;
					}
				} else if(_kbdState == KbdStateRecvAck) {
					_kbdData = false;
					_kbdState = _kbdQueueLen ? KbdStateSendStartBit : KbdStateIdle;
				}
			} else {
				//Falling edge - the keyboard's bits go out here
				if(_kbdState == KbdStateSendStartBit) {
					_kbdData = false;
					_kbdLatch = KbdPop();
					_kbdParity = 0;
					_kbdState++;
				} else if(_kbdState >= KbdStateSendDataBit7 && _kbdState < KbdStateSendParityBit) {
					_kbdData = (_kbdLatch & 0x01) != 0;
					_kbdParity += _kbdLatch & 0x01;
					_kbdLatch >>= 1;
					_kbdState++;
				} else if(_kbdState == KbdStateSendParityBit) {
					_kbdData = !(_kbdParity & 0x01);
					_kbdState++;
				} else if(_kbdState == KbdStateSendStopBit) {
					_kbdData = true;
					_kbdState = _kbdQueueLen ? KbdStateSendStartBit : KbdStateIdle;
				}
			}
		}
	}

	uint8_t BiosBank() { return _regs[0x00] & 0x07; }
	uint8_t BiosGroup() { return _regs[0x2F] & 0x03; }
	bool SaveRamEnabled() { return (_regs[0x00] & 0x80) != 0; }
	uint8_t PrgReadBank() { return _regs[0x10] & 0x0F; }
	uint8_t PrgWriteBank() { return _regs[0x11] & 0x0F; }

	//$4181: bits 4-5 machine mode, bits 2-3 PRG bank size, bits 0-1 CHR bank size
	uint8_t MachineMode() { return (_regs[0x01] >> 4) & 0x03; }
	uint8_t NewPrgSize() { return (_regs[0x01] >> 2) & 0x03; }
	//$4183: bits 4-7 mask the PRG bank number, bits 0-3 the CHR one
	uint8_t PrgMask() { return (_regs[0x03] >> 4) & 0x0F; }

	//Where a CPU address reads from once the BIOS has left load mode and is running out of
	//PRG-RAM. Returns the offset into work RAM, so the write path can use it too.
	uint32_t SystemBankOffset(uint16_t addr)
	{
		switch(NewPrgSize()) {
			case 0: {
				//Four 8KB banks, $4190-$4193
				uint32_t bank = _regs[0x10 + (((addr >> 12) >> 1) & 3)] & ((PrgMask() << 2) | 3);
				return bank * 0x2000 + (addr & 0x1FFF);
			}
			case 1: {
				//Two 16KB banks, $4190 and $4192
				uint32_t bank = _regs[0x10 + (((addr >> 12) >> 1) & 2)] & ((PrgMask() << 1) | 1);
				return bank * 0x4000 + (addr & 0x3FFF);
			}
			default: {
				//One 32KB bank, $4190 - note it covers $8000-$FFFF here, where the same
				//register in load mode covers $6000-$DFFF instead
				uint32_t bank = _regs[0x10] & PrgMask();
				return bank * 0x8000 + (addr & 0x7FFF);
			}
		}
	}

	void UpdatePrgMapping()
	{
		if(_loadMode) {
			//$E000 and $F000 are the only ROM in the machine's map; everything below is RAM.
			//Both 4KB pages come from the same 32KB BIOS group, $F000 always taking its last.
			SelectPrgPage(6, (BiosGroup() << 3) | BiosBank());
			SelectPrgPage(7, (BiosGroup() << 3) | 7);

			//One 32KB PRG-RAM bank covers $6000-$DFFF, but NOT in address order: the bank is
			//indexed by bits 12-14 of the address, so $8000-$DFFF takes its first six 4KB
			//pages and $6000-$7FFF takes the last two. Mapping it as one straight run instead
			//shifts the whole thing by two pages, which is exactly the rotation that showed up
			//when the unpacked image was compared against the ROM.
			uint32_t bankStart = PrgReadBank() * 0x8000;
			SetCpuMemoryMapping(0x8000, 0xDFFF, PrgMemoryType::WorkRam, bankStart, MemoryAccessType::Read);

			if(SaveRamEnabled()) {
				SetCpuMemoryMapping(0x6000, 0x7FFF, PrgMemoryType::WorkRam, SramBase + BiosGroup() * 0x2000, MemoryAccessType::Read);
			} else {
				SetCpuMemoryMapping(0x6000, 0x7FFF, PrgMemoryType::WorkRam, bankStart + 0x6000, MemoryAccessType::Read);
			}
			return;
		}

		//Out of load mode, $8000-$FFFF is the BIOS ROM unless the machine has asked for the
		//PRG-RAM window. The ROM layout is the one the reference sets up at reset - the
		//first two 8KB banks low and the last two high - and it is what the machine is
		//actually running from at the BUNG BIOS screen: a dump of the reference at $F800
		//there matches ROM offset $1F800 (the last 8KB bank, +$1800) byte for byte.
		SelectPrgPage(0, 0);
		SelectPrgPage(1, 1);
		SelectPrgPage(2, 2);
		SelectPrgPage(3, 3);
		uint16_t lastPage = (uint16_t)(GetPrgPageCount() - 1);
		SelectPrgPage(4, (uint16_t)(lastPage - 3));
		SelectPrgPage(5, (uint16_t)(lastPage - 2));
		SelectPrgPage(6, (uint16_t)(lastPage - 1));
		SelectPrgPage(7, lastPage);

		//The system-bank mode redirects the whole window into PRG-RAM instead
		if(MachineMode() == MachineNew) {
			switch(NewPrgSize()) {
				case 0:
					for(int i = 0; i < 4; i++) {
						uint16_t start = (uint16_t)(0x8000 + i * 0x2000);
						SetCpuMemoryMapping(start, (uint16_t)(start + 0x1FFF), PrgMemoryType::WorkRam, SystemBankOffset(start), MemoryAccessType::Read);
					}
					break;
				case 1:
					SetCpuMemoryMapping(0x8000, 0xBFFF, PrgMemoryType::WorkRam, SystemBankOffset(0x8000), MemoryAccessType::Read);
					SetCpuMemoryMapping(0xC000, 0xFFFF, PrgMemoryType::WorkRam, SystemBankOffset(0xC000), MemoryAccessType::Read);
					break;
				default:
					SetCpuMemoryMapping(0x8000, 0xFFFF, PrgMemoryType::WorkRam, SystemBankOffset(0x8000), MemoryAccessType::Read);
					break;
			}
		}

		//$6000-$7FFF is the save RAM once load mode is over, whatever $4180 bit 7 says
		SetCpuMemoryMapping(0x6000, 0x7FFF, PrgMemoryType::WorkRam, SramBase + BiosGroup() * 0x2000, MemoryAccessType::Read);
	}

	//$4198-$419F select CHR. How many of them are consulted, and how wide a bank each
	//one names, comes from $4181: the eight registers cover 1KB each, four cover 2KB
	//each, two cover 4KB each, or $4198 alone covers the whole 8KB. The mask widens with
	//the bank width so a given register always spans the same address bits.
	void UpdateChrMapping()
	{
		if(_loadMode) {
			//Until the machine leaves load mode it is always the whole-8KB form
			for(int i = 0; i < 8; i++) {
				SelectChrPage(i, (uint16_t)((_regs[0x18] & 0x3F) * 8 + i));
			}
			return;
		}

		uint8_t chrMask = _regs[0x03] & 0x0F;
		switch(_regs[0x01] & 0x03) {
			case 0: {
				uint8_t mask = (uint8_t)((chrMask << 5) | 31);
				for(int i = 0; i < 8; i++) {
					SelectChrPage(i, (uint16_t)(_regs[0x18 | i] & mask));
				}
				break;
			}

			case 1: {
				uint8_t mask = (uint8_t)((chrMask << 4) | 15);
				for(int i = 0; i < 8; i++) {
					SelectChrPage(i, (uint16_t)((_regs[0x18 | (i & ~1)] & mask) * 2 + (i & 1)));
				}
				break;
			}

			case 2: {
				uint8_t mask = (uint8_t)((chrMask << 3) | 7);
				for(int i = 0; i < 8; i++) {
					SelectChrPage(i, (uint16_t)((_regs[0x18 | (i & ~3)] & mask) * 4 + (i & 3)));
				}
				break;
			}

			default: {
				uint8_t mask = (uint8_t)((chrMask << 2) | 3);
				for(int i = 0; i < 8; i++) {
					SelectChrPage(i, (uint16_t)((_regs[0x18] & mask) * 8 + i));
				}
				break;
			}
		}
	}

	//$42FC-$42FF carry a second mirroring selection, which the top half of $4182 can defer
	//to instead of naming an arrangement itself. Zero until the machine enters game mode.
	uint8_t _mirroring = 0;

	//$4182 bits 4-6 pick the nametable arrangement. The first four values name one
	//outright; the last four hand the choice to $42FC-$42FF, either in part or in full.
	//Leaving those four unhandled is not harmless: the arrangement then keeps whatever it
	//had, and a program that expected two nametables writes both of them onto one page.
	void MirrorSync()
	{
		switch((_regs[0x02] >> 4) & 0x07) {
			case 0: SetMirroringType(MirroringType::ScreenAOnly); break;
			case 1: SetMirroringType(MirroringType::ScreenBOnly); break;
			case 2: SetMirroringType(MirroringType::Vertical); break;
			case 3: SetMirroringType(MirroringType::Horizontal); break;

			case 4:
				SetMirroringType((_mirroring & 1) ? MirroringType::ScreenBOnly : MirroringType::ScreenAOnly);
				break;

			case 5:
				SetMirroringType((_mirroring & 1) ? MirroringType::Horizontal : MirroringType::Vertical);
				break;

			default:
				switch(_mirroring & 3) {
					case 0: SetMirroringType(MirroringType::ScreenAOnly); break;
					case 1: SetMirroringType(MirroringType::ScreenBOnly); break;
					case 2: SetMirroringType(MirroringType::Vertical); break;
					default: SetMirroringType(MirroringType::Horizontal); break;
				}
				break;
		}
	}

	//The machine is running its own software rather than a cartridge personality
	bool SystemMode() { return ((_regs[0x01] >> 4) & 0x03) == 1; }

protected:
	uint16_t GetPrgPageSize() override { return 0x1000; }
	//1KB, the finest the eight CHR registers can address - the wider forms are built out
	//of runs of these pages in UpdateChrMapping.
	uint16_t GetChrPageSize() override { return 0x400; }
	uint16_t GetChrRamPageSize() override { return 0x400; }
	uint32_t GetChrRamSize() override { return 0x80000; }

	//512KB of PRG-RAM plus the 32KB the BIOS calls its SRAM, in one allocation. The SRAM is
	//NOT asked for as save RAM: BaseMapper only honours GetSaveRamSize() when the rom's
	//battery bit is set, and this one's is not, so the request would be dropped and the
	//buffer left zero-length - which corrupts the heap rather than failing visibly. Nothing
	//is known to battery-back this RAM on the real machine either.
	uint32_t GetWorkRamSize() override { return SramBase + 0x8000; }
	uint32_t GetSaveRamSize() override { return 0; }

	bool EnableCpuClockHook() override { return true; }
	bool EnableCustomVramRead() override { return true; }
	bool EnableVramAddressHook() override { return true; }

	void NotifyVramAddressChange(uint16_t addr) override
	{
		if((addr >> 12) != 2) {
			return;
		}
		_ntData = (uint8_t)((addr >> 8) & 0x03);
		if(_regs[0x14] >= 0x40 && _regs[0x14] <= 0x43) {
			uint32_t bank = (uint32_t)(_regs[0x14] + _ntData - 2);
			SetPpuMemoryMapping(0x1000, 0x1FFF, ChrMemoryType::ChrRam,
				(bank * 0x1000) & (_chrRamSize - 1), MemoryAccessType::ReadWrite);
			_logoMode = true;
		}
	}

	uint8_t MapperReadVram(uint16_t addr, MemoryOperationType type) override
	{
		//$4198 = $3F opens a window onto the per-cell bank table. The 2KB that shadows the
		//two nametables appears at $1800-$1FFF, one byte per cell, at the cell's own offset
		//- so a program that is about to draw over part of the screen can read back what it
		//stamped there, and put it back when it removes what it drew. Without the window a
		//read lands in CHR instead and the program saves pixels where it wanted banks, then
		//restores those pixels as banks: the text comes back in whatever glyphs the wrong
		//banks happen to hold.
		//
		//Only the processor sees this. The pattern fetches behind the picture come through
		//the same hook and must still reach CHR, so the window is opened for $2007 alone.
		if(type == MemoryOperationType::Read && _regs[0x18] == 0x3F && addr >= 0x1800 && addr < 0x2000) {
			return _exRamNt[addr - 0x1800];
		}

		//Sprite fetches share this hook but are drawn straight from the CHR banking, so
		//they have to be let through untouched.
		if(ExtLatchEnabled() && IsBackgroundFetch()) {
			if(addr >= 0x2000 && addr <= 0x2FFF && (addr & 0x3FF) < 0x3C0) {
				//A nametable fetch: the next three reads are this tile's attribute and its
				//two pattern bytes
				_extNtAddr = addr;
				_extFetchCounter = 3;
			} else if(_extFetchCounter > 0) {
				_extFetchCounter--;
				if(_extFetchCounter <= 1) {
					return ReadLatchedTile(addr);
				}
			}
		}
		return InternalReadVram(addr);
	}

	//One of the two pattern bytes of a latched tile. Every tile carries two per-cell
	//bytes: one the machine stamps into the shadow as it writes the nametable, and one
	//it keeps in a table in the top pages of CHR-RAM. Which of them selects the bank,
	//and how the bytes are then read, is what the branches below decide.
	uint8_t ReadLatchedTile(uint16_t addr)
	{
		uint16_t ntOffset = _extNtAddr & 0x3FF;
		uint8_t ntIndex = (uint8_t)((_extNtAddr >> 10) & 3);

		uint32_t autoIndex = (0x1F8u + ((ntIndex & 1) | 4u | ((_regs[0x18] << 1) & 2u))) * 1024u + ntOffset;
		uint8_t autoBank = _chrRam[autoIndex & (_chrRamSize - 1)];
		uint8_t exBank = _exRamNt[ExRamIndex(_extNtAddr)];

		//The machine treats a screen whose sprites and background share a pattern table
		//as ordinary output and mostly leaves it alone.
		BaseNesPpu* ppu = _console->GetPpu();
		bool sameTable = ppu->GetBgPatternAddr() == ppu->GetSpritePatternAddr();

		if((_regs[0x0D] & 0x03) == 3) {
			if(sameTable) {
				return InternalReadVram(addr);
			}

			//1bpp glyphs: 256 of them, 8 bytes each, in the 2K bank the shadow names.
			//The nametable byte picks the glyph, and its low bit picks which half of the
			//16-byte pair - so a cell addresses twice as many glyphs as it has tiles.
			uint16_t tileAddr = addr & 0xFFF7;
			uint32_t tile = (tileAddr >> 5) & 0x7F;
			uint32_t line = (tileAddr & 7) | ((tileAddr >> 1) & 8);
			uint32_t index = ((uint32_t)(exBank & ChrMask2K()) * 2 + (tile >> 6)) * 1024 + (((tile << 4) & 0x3FF) | line);
			uint8_t data = _chrRam[index & (_chrRamSize - 1)];

			//The one byte feeds both planes, each through its own 2-bit recipe, which is
			//what gives a 1bpp glyph its four colours.
			switch(((addr & 0x08) ? (autoBank >> 2) : autoBank) & 0x03) {
				case 0: return 0x00;
				case 1: return 0xFF;
				case 2: return data;
				default: return data ^ 0xFF;
			}
		}

		uint32_t bank;
		if(sameTable) {
			bank = _autoBank ? autoBank : exBank;

			//BUNGMINE draws from a second table one page further up
			if(_regs[0x14] == 0x60 && _regs[0x18] == 0x30) {
				uint32_t mineIndex = (0x1FAu + ((ntIndex & 1) | 4u)) * 1024u + ntOffset;
				bank = _chrRam[mineIndex & (_chrRamSize - 1)];
			}

			//Only these three combinations redirect the fetch at all - anything else
			//draws the tile from the ordinary CHR banking.
			if(!(_romType == 1 || _romType == 2 || (_romType == 0 && _diskType == 1))) {
				return InternalReadVram(addr);
			}
		} else {
			bank = (uint32_t)((_regs[0x18] << 1) | autoBank);
			if(_logoMode || (_romType == 0 && _diskType == 1) ||
				((_romType == 1 || _romType == 2) && _diskType == 3) || IsBwinBanner(ntOffset)) {
				bank = exBank;
			}
		}

		uint32_t offset = (bank & ChrMask4K()) * 0x1000 + (addr & 0x0FFF);
		return _chrRam[offset & (_chrRamSize - 1)];
	}

	//BUNG Windows draws its banner from the shadow while the rest of the screen comes
	//from the per-cell table. It is recognised by the text already on the nametable
	//rather than by any register, so the test is a look at what is on screen.
	bool IsBwinBanner(uint16_t ntOffset)
	{
		if(!((ntOffset > 0x360 && ntOffset < 0x37F) || (ntOffset > 0x380 && ntOffset < 0x39F))) {
			return false;
		}
		return InternalReadVram(0x2367) == 0x53 && InternalReadVram(0x2377) == 0x20 &&
			InternalReadVram(0x2387) == 0x6E && InternalReadVram(0x2397) == 0x31;
	}

	void MapperWriteVram(uint16_t addr, uint8_t value) override
	{
		//Uploading CHR into the top of the pattern space while those two registers hold
		//these values is what arms the per-tile bank table
		if(_regs[0x14] == 0x7E) {
			if(_regs[0x18] == 0x3F && addr >= 0x1800 && addr < 0x2000) {
				_autoBank = true;
			}
		} else {
			_autoBank = false;
		}

		//Every nametable write stamps the current CHR bank into the shadow
		if((_regs[0x0D] & 0x02) && addr >= 0x2000) {
			_exRamNt[ExRamIndex(addr)] = _regs[0x14];
		}
		InternalWriteVram(addr, value);
	}

	void ProcessCpuClock() override
	{
		BaseProcessCpuClock();

		//A game off the disc is normally started by the machine, which has already quietened
		//the frame counter by the time it hands over. Started on its own it never does that
		//itself and never acknowledges the interrupt either, so the first CLI would drop it
		//into its own handler for good. Stand in for the hand-over on the first cycle, once
		//the sound hardware exists to be written to.
		if(IsCdv() && !_cdvApuReady) {
			_cdvApuReady = true;
			_console->GetMemoryManager()->Write(0x4017, 0x40, MemoryOperationType::Write);
		}

		//Look for a key transition about 50 times a second - the queue paces the rest
		if(++_kbdPollTimer >= 35464) {
			_kbdPollTimer = 0;
			KbdPollKeys();
		}

		KbdClock();
		MousePoll();
		_printer.Clock();
		_fdc.Clock();

		if(_kbdRaiseIrq) {
			_console->GetCpu()->SetIrqSource(IRQSource::External);
		} else {
			_console->GetCpu()->ClearIrqSource(IRQSource::External);
		}

		if(_irqEnabled) {
			//$4182 bits 0-1 pick how the counter runs: 2 counts up to $FFFF, 3 counts down
			//to zero, and either way the IRQ fires once and disables itself.
			uint8_t irqType = _regs[0x02] & 0x03;
			if(irqType == 2) {
				if(++_irqCounter >= 0xFFFF) {
					_irqEnabled = false;
					_console->GetCpu()->SetIrqSource(IRQSource::External);
				}
			} else if(irqType == 3) {
				if(_irqCounter == 0 || --_irqCounter == 0) {
					_irqEnabled = false;
					_irqCounter = 0;
					_console->GetCpu()->SetIrqSource(IRQSource::External);
				}
			}
		}
	}

	//$4016 is the controller strobe, but this machine also uses it to say whether the
	//mouse report is wanted. Only the write is claimed - reads still belong to the
	//control manager.
	void GetMemoryRanges(MemoryRanges& ranges) override
	{
		BaseMapper::GetMemoryRanges(ranges);
		ranges.AddHandler(MemoryOperation::Read, 0x4016, 0x4017);
		ranges.AddHandler(MemoryOperation::Write, 0x4016);
		ranges.SetAllowOverride();
	}

	uint16_t RegisterStartAddress() override { return 0x4020; }
	uint16_t RegisterEndAddress() override { return 0x5FFF; }
	bool AllowRegisterRead() override { return true; }

	void InitMapper(RomData& romData) override
	{
		romData.Info.System = GameSystem::Dendy;
		_romType = IsKw2000(romData.Info.Hash.PrgCrc32) ? 1 : (IsKw3000(romData.Info.Hash.PrgCrc32) ? 2 : 0);

		_cdvHeader = romData.CdvHeader;
		_cdvTrainer = romData.CdvTrainer;
		if(IsCdv()) {
			//The rom's own PRG and CHR are the game; the machine runs them out of its RAM,
			//so keep a copy to lay down at every reset rather than mapping them in place.
			_cdvPrg = romData.PrgRom;
			_cdvChr = romData.CdvChr;

			//This runs after InitMapper(), so the machine's own setup is already in place
			//and the game's state goes on top of it
			InitCdv();
			UpdatePrgMapping();
			UpdateChrMapping();
		}
	}

	//A game brings the state the BIOS would have set up for it: the registers, the code and
	//graphics in RAM, and no load mode to leave.
	void InitCdv()
	{
		for(int i = 0; i < 0x40; i++) {
			_regs[i] = _cdvHeader[0x10 + i];
		}
		_loadMode = false;

		memcpy(_workRam, _cdvPrg.data(), std::min((size_t)_workRamSize, _cdvPrg.size()));
		if(!_cdvChr.empty()) {
			memcpy(_chrRam, _cdvChr.data(), std::min((size_t)_chrRamSize, _cdvChr.size()));
		}

		//A startup block goes where the header says, in the window the machine keeps at $6000
		if(_cdvHeader[0x0A] && _cdvHeader[0x0D] && !_cdvTrainer.empty()) {
			uint32_t offset = (uint32_t)((_cdvHeader[0x0A] << 8) & 0x1FFF);
			size_t len = std::min(_cdvTrainer.size(), (size_t)(0x2000 - offset));
			memcpy(_workRam + SramBase + offset, _cdvTrainer.data(), len);
		}

		//$4194 bit 6 says the game is one of the machine's own rather than a plain cartridge
		//conversion. Those keep the machine's tile latch and its screen arrangement, and are
		//treated as though its own operating system were in the drive - which is what decides
		//the two latch branches, since a game carries no BIOS to be recognised by.
		if(_regs[0x01] & 0x40) {
			_diskType = 1;
			SetMirroringType(MirroringType::Vertical);
		} else {
			SetMirroringType(MirroringType::ScreenAOnly);
		}
	}

	void InitMapper() override
	{
		_romInfo.System = GameSystem::Dendy;

		memset(_regs, 0, sizeof(_regs));
		memset(_exRam, 0, sizeof(_exRam));
		_regs[0x03] = 0xFF;
		_loadMode = true;
		_irqCounter = 0;
		_irqEnabled = false;
		_irqStatus = 0;

		_kbdCtrl = 0x03;
		_kbdClock = false;
		_kbdData = true;
		_kbdClockCount = 0;
		_kbdLatch = 0;
		_kbdParity = 0;
		_kbdState = KbdStateIdle;
		_kbdRaiseIrq = false;
		_kbdQueueLen = 0;
		_kbdPollTimer = 0;

		_mouseEnabled = false;
		_mouseFrame = 0;
		_ntData = 0;
		_mirroring = 0;
		_logoMode = false;
		_autoBank = false;
		_diskType = 0;
		memset(_exRamNt, 0, sizeof(_exRamNt));
		_extNtAddr = 0;
		_extFetchCounter = 0;

		_lptData = 0;
		_lptCtrl = 0;
		_printer.Reset();

		_fdc.SetClearGeometryOnReset(true);
		_fdc.Reset();
		_floppyChecked = false;

		memset(_chrRam, 0, _chrRamSize);
		memset(_workRam, 0, _workRamSize);

		//Writes into the RAM window have to reach the mapper - see the $4190/$4191 note
		AddRegisterRange(0x6000, 0xFFFF, MemoryOperation::Write);

		//$4016/$4017 sit below the mapper's own register range, so they need claiming
		//twice: once in GetMemoryRanges for the memory handler, and here so the access is
		//dispatched as a register rather than falling through to RAM
		AddRegisterRange(0x4016, 0x4016, MemoryOperation::Write);
		AddRegisterRange(0x4016, 0x4017, MemoryOperation::Read);

		SetMirroringType(MirroringType::Vertical);
		UpdatePrgMapping();
		UpdateChrMapping();

	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		MountPairedFloppy();

		switch(addr) {
			case 0x4016:
			case 0x4017:
				//Nothing answers the controller ports on this machine. The BIOS polls
				//$4017 looking for a serial mouse the hardware reports as absent, and
				//reads back a clean zero rather than the open bus a controller would
				//leave there - anything else and the poll sees phantom data.
				return 0;

			case 0x4188: _fdc.MarkActivity(); return _fdc.Read(4);
			case 0x4189: _fdc.MarkActivity(); return _fdc.Read(5);
			case 0x418B: _fdc.MarkActivity(); return _fdc.Read(2);

			case 0x418E:
				//Bits 6-7 always read set; the keyboard's own clock and data come back in
				//bits 1 and 0, and the rest is whatever the host last wrote.
				_kbdRaiseIrq = false;
				return (uint8_t)((_kbdCtrl & 0x3C) | 0xC0 | (_kbdClock ? 0x02 : 0) | (_kbdData ? 0x01 : 0));

			case 0x418F:
				_kbdRaiseIrq = false;
				return _irqStatus;

			case 0x41A1: return (uint8_t)(_irqCounter & 0xFF);
			case 0x41A2: return (uint8_t)(_irqCounter >> 8);

			//$41AB bit 4 and $41AF bit 0 are the drive's presence lines, reported the way the
			//reference reports them - set only once an image is actually mounted. The machine
			//boots to its own screen either way; it does not wait for a disk.
			case 0x41AB: return _fdc.IsDiskInserted() ? (_regs[0x2B] | 0x10) : _regs[0x2B];
			case 0x41AF: return _fdc.IsDiskInserted() ? (_regs[0x2F] | 0x01) : _regs[0x2F];

			//Speech reports ready, so a BIOS waiting on it is not left spinning
			case 0x41AC: return 0x40;
			case 0x41AE: return _regs[0x2E];
		}

		if(addr >= 0x4180 && addr <= 0x41BF) {
			return _regs[addr & 0x3F];
		}
		return _exRam[addr - 0x4020];
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		MountPairedFloppy();

		if(addr >= 0x6000) {
			if(_loadMode) {
				//The RAM window. Reads come from the bank $4190 names and writes go to the
				//one $4191 names, so this cannot be left to the page table. $E000-$FFFF is
				//BIOS ROM here, so writes there go nowhere.
				if(addr >= 0xE000) {
					return;
				} else if(addr < 0x8000 && SaveRamEnabled()) {
					_workRam[SramBase + (BiosGroup() * 0x2000) + (addr & 0x1FFF)] = value;
				} else {
					//Same page order as the read mapping above
					uint32_t offset = (addr < 0x8000) ? (0x6000 + (addr - 0x6000)) : (addr - 0x8000);
					_workRam[(PrgWriteBank() * 0x8000) + offset] = value;
				}
			} else if(addr < 0x8000) {
				_workRam[SramBase + (BiosGroup() * 0x2000) + (addr & 0x1FFF)] = value;
			} else if(MachineMode() == MachineNew) {
				//Same banks the reads come through. Bounds-checked, NOT masked: the work RAM
				//is 0x88000 bytes (512KB + the 32KB SRAM) and masking with size-1 is only
				//valid for a power of two - it silently folded $7F800 down to $07800, so the
				//machine's writes landed half a megabyte away from where it read them back.
				uint32_t offset = SystemBankOffset(addr);
				if(offset < _workRamSize) {
					_workRam[offset] = value;
				}
			}
			return;
		}

		if(addr < 0x6000) {
			_exRam[addr - 0x4020] = value;
		}

		if(addr == 0x4016) {
			//The KW3000 reports a mouse unconditionally; the KW2000 asks for one here
			_mouseEnabled = (_romType == 2) || (value != 0);

			//This is still the controller strobe - the mapper only listens in
			NesControls()->WriteRam(addr, value);
			return;
		}

		if(addr >= 0x4180 && addr <= 0x41BF) {
			_regs[addr & 0x3F] = value;
		}

		switch(addr) {
			case 0x4180:
			case 0x4190:
			case 0x4191:
			case 0x41AF:
				UpdatePrgMapping();
				break;

			case 0x4182:
				MirrorSync();
				break;

			case 0x4184: _lptData = value; break;
			case 0x4186: WriteLptCtrl(value); break;

			case 0x4188: _fdc.MarkActivity(); _fdc.Write(7, value); break;
			case 0x4189: _fdc.MarkActivity(); _fdc.Write(5, value); break;
			case 0x418B: _fdc.MarkActivity(); _fdc.Write(2, value); break;

			case 0x418E:
				_kbdCtrl = value;
				break;

			case 0x4198: case 0x4199: case 0x419A: case 0x419B:
			case 0x419C: case 0x419D: case 0x419E: case 0x419F:
				UpdateChrMapping();
				break;

			case 0x41A1:
				_irqCounter = (uint16_t)((_irqCounter & 0xFF00) | value);
				break;
			case 0x41A2:
				_irqCounter = (uint16_t)((_irqCounter & 0x00FF) | (value << 8));
				break;
			case 0x41A3:
				_irqEnabled = (value & 0x01) != 0;
				_console->GetCpu()->ClearIrqSource(IRQSource::External);
				break;

			case 0x41A5:
				//The KW2000 BIOS unpacks its ROM into PRG-RAM and then writes here to hand
				//over to the copy. Everything above $6000 changes meaning at this point.
				//Leaving load mode on instead is demonstrably wrong: the very next thing the
				//BIOS does is set $4190 to a bank it never filled.
				_loadMode = false;
				UpdatePrgMapping();
				break;

			case 0x4181:
			case 0x4183:
			case 0x4192:
			case 0x4193:
				UpdatePrgMapping();
				break;

			case 0x42FC: case 0x42FD: case 0x42FE: case 0x42FF:
				//Enter game mode and set the mirroring $4182 can defer to
				_mirroring = (uint8_t)((addr & 1 ? 2 : 0) | (value & 0x10 ? 1 : 0));
				if(SystemMode()) {
					MirrorSync();
				}
				break;
		}
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SVArray(_regs, 0x40);
		SVArray(_exRam, 0x2000);
		SV(_loadMode);
		SV(_irqCounter);
		SV(_irqEnabled);
		SV(_irqStatus);
		SV(_kbdCtrl); SV(_kbdClock); SV(_kbdData); SV(_kbdClockCount);
		SV(_kbdLatch); SV(_kbdParity); SV(_kbdState); SV(_kbdRaiseIrq);
		SVArray(_kbdQueue, 32); SV(_kbdQueueLen); SV(_kbdPollTimer);
		SV(_fdc);
		SVArray(_exRamNt, 0x800); SV(_extNtAddr); SV(_extFetchCounter); SV(_diskType);
		SV(_ntData); SV(_logoMode); SV(_autoBank); SV(_mirroring);
		SV(_lptData); SV(_lptCtrl); SV(_printer);
		SV(_mouseEnabled); SV(_mouseFrame); SV(_cdvApuReady);

		if(!s.IsSaving()) {
			UpdatePrgMapping();
			UpdateChrMapping();
		}
	}
};
