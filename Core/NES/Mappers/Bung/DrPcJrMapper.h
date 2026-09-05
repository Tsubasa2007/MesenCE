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
#include "NES/Mappers/Bung/DrPcJrCdDrive.h"
#include "NES/Mappers/Bung/DrPcJrGameChips.h"
#include "NES/Mappers/Bbk/BbkPrinter.h"
#include "NES/Mappers/Bbk/BbkLpcAudio.h"
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
// - $41A0         raster IRQ: the scanline count $4182 mode 1 counts up from
// - $41A1/$41A2   IRQ counter, low and high
// - $41A3         IRQ enable
// - $41AC         speech chip: $5x feeds it a nibble, anything else resets it
// - $41AF bits 0-1 pick the 32KB BIOS group the $E000/$F000 pages come from
// - $41A5         leaves load mode, entering the cartridge personality (not implemented)
// - $42FC-$42FF   enters game mode and sets mirroring (not implemented)
class DrPcJrMapper : public BaseMapper
{
public:
	static bool IsKw2000(uint32_t prgCrc) { return prgCrc == 0x9982BF45; }
	static bool IsKw3000(uint32_t prgCrc) { return prgCrc == 0xCFF8F964; }

	//The two original Bung machines are only 32KB, which is the size a real Idea-Tek 173
	//cart is, so the size-and-no-CHR test that picks the KW machines out cannot see them.
	//They have to be named. 1.0a and 1.5a differ only in their own version strings.
	static bool IsDrPcJr32k(uint32_t prgCrc) { return prgCrc == 0xC8FBEF89 || prgCrc == 0x98F2033B; }

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
	inline static string _persistedDiscRom;
	inline static string _persistedDiscPath;
	inline static string _persistedDiskRom;
	inline static string _persistedDiskPath;
	inline static bool _persistedDiskValid = false;

	//$4181 bits 4-5: which personality the window takes once a game has the machine.
	//0 is a converted board that banks itself, 1 the machine's own system-bank window,
	//and 2 and 3 hand the game's own mapper writes to an imitated controller - see
	//DrPcJrGameChips.
	static constexpr uint8_t MachineOld = 0;
	static constexpr uint8_t MachineNew = 1;
	static constexpr uint8_t MachineMmc1 = 2;
	static constexpr uint8_t MachineMmc3 = 3;

	//The 32KB SRAM lives at the top of the work RAM allocation
	static constexpr uint32_t SramBase = 0x80000;

	uint8_t _regs[0x40] = {};

	//The whole of $4020-$5FFF is RAM on this board - the reference reaches it through its
	//generic bank pointer, so every address in the range that is not one of the registers
	//above still reads back what was written to it. The BIOS does use it: it writes $4200
	//during start-up. Leaving the range unmapped instead makes those reads open bus.
	//$4020-$5FFF is the machine's own RAM, and the software it loads runs out of it. It is
	//the base mapper's array rather than one of our own purely so the debugger can see it:
	//reads of this range go through ReadRegister, so a private array is invisible to every
	//tool - emu.read returns open bus for it, which has cost two investigations.
	bool _loadMode = true;
	bool _placeUseB = false;
	uint16_t _irqCounter = 0;
	bool _irqEnabled = false;
	uint8_t _lineCounter = 0;
	int32_t _lastIrqScanline = -2;
	bool _lineIrqPending = false;
	//The cycle counter's interrupt, held the same way the line one is. Setting the CPU's
	//line from inside the count would be undone on the very next clock by the line
	//rewritten at the top of ClockCpu, which only knows about the sources it lists.
	bool _counterIrqPending = false;
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
		//put the report until the machine has left load mode - and once a disc game is
		//running there is nowhere for it again, because those three bytes are now the game's
		//own code. Writing the report there anyway corrupts the game: the neutral report is
		//C0 80 80, and on one disc title it landed on $9FAB, turning BEQ/LDA into
		//CPY #$80 / NOP #$71 / ASL $C9 / LDY #$90 and running the processor into the $02 two
		//bytes later, which jams it. The machine is a cartridge now; its own mouse is not
		//part of that.
		if(!_mouseEnabled || _loadMode || GameWindow()) {
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

		//Whether the machine has actually taken the last report, which decides whether there
		//is anything to build a new one out of. It answers two different ways, and the
		//difference matters: having read all three bytes it strips the framing bit from the
		//second and third as it decodes them, but its polling loop also blindly zeroes the
		//first byte at the top of a frame whether or not anything was there. Reading only
		//the first byte therefore cannot tell "taken" from "thrown away unread", and a
		//report replaced on the second reading is a report whose movement is gone - the
		//device's accumulator was drained to build it. The machine takes one every second
		//frame, so that was half of every gesture, and the pointer covered about a third of
		//the distance the hand moved. Nothing on its desktop could be hit.
		uint32_t dataOffset = SystemBankOffset(0xFFAC);
		bool taken = dataOffset >= _workRamSize || (_workRam[dataOffset] & 0x80) == 0;

		if(taken) {
			int8_t dx, dy;
			uint8_t buttons;
			mouse->TakeDelta(dx, dy, buttons);

			//A mouse sitting still with a button held down says nothing at all: it sends one
			//report when the button goes down and another when it comes up. Repeating "still
			//held" every frame is not harmless chatter. The machine only reports a press as
			//an edge against a latch of what is currently held, and a program is free to
			//clear that latch itself once it has taken the event - one of them does, every
			//pass of its main loop. Against a report that keeps saying "held", the edge is
			//found again and again, and one click of a button arrives as four presses. The
			//program acted on all four and finished where it started, which read as the
			//click doing nothing at all.
			if(dx == 0 && dy == 0 && buttons == _lastMouseButtons) {
				return;
			}
			_lastMouseButtons = buttons;

			//Byte 0 carries both buttons and the top two bits of each delta; the other two
			//carry the low six bits. The high bits are the machine's own framing.
			uint8_t bx = (uint8_t)dx;
			uint8_t by = (uint8_t)dy;
			_mouseReport[0] = (uint8_t)(0xC0 | ((buttons & 0x01) << 5) | ((buttons & 0x02) << 3) | ((by & 0xC0) >> 4) | ((bx & 0xC0) >> 6));
			_mouseReport[1] = (uint8_t)(0x80 | (bx & 0x3F));
			_mouseReport[2] = (uint8_t)(0x80 | (by & 0x3F));
		}

		//Put it back either way. An untaken report is simply re-armed with the movement it
		//already carries, so a blind clear costs a frame of latency rather than the gesture.
		for(int i = 0; i < 3; i++) {
			uint32_t offset = SystemBankOffset((uint16_t)(0xFFAB + i));
			if(offset < _workRamSize) {
				_workRam[offset] = _mouseReport[i];
			}
		}
	}

	//The last report built, kept so an untaken one can be re-armed without drawing fresh
	//movement out of the device. See MousePoll.
	uint8_t _mouseReport[3] = { 0xC0, 0x80, 0x80 };

	//What the buttons were in the last report, so an unchanged one is not sent again
	uint8_t _lastMouseButtons = 0;

	uint8_t _ntData = 0;
	bool _logoMode = false;

	//With this set the per-tile bank comes from a table the machine keeps in the top pages
	//of CHR-RAM, rather than from the nametable-write shadow (which holds one constant value
	//and so can only ever draw one bank's worth of tiles). The machine turns it on by writing
	//CHR at $1800-$1FFF while $4194 is $7E and $4198 is $3F - which is what its graphical
	//screens do, and why they came out with the wrong tiles until this was here.
	bool _autoBank = false;
	//Set by a write to $42FC-$42FF: a disc game has taken the machine over
	bool _gameLaunched = false;
	//What a write to $8000-$FFFF means once a game has taken the machine over. The game is
	//a converted cartridge and still banks itself the way its own board did, and the
	//hand-over value at $42FC-$42FF says which board that was: 0 and 7 do not bank at all,
	//1-4 move the PRG window (1 and 4 the CHR with it), 5 and 6 move only the CHR.
	uint8_t _gameMode = 0;
	//The four 8KB PRG-RAM banks the game's window is made of, and its 8KB CHR bank
	uint8_t _gamePrgBanks[4] = { 0, 1, 2, 3 };
	uint8_t _gameChrBank = 0;
	//The window is laid out once, on the first hand-over - see StartGameMode
	bool _gameReset = false;
	//Which controller the game asked the machine to be: 0 for a converted board that
	//banks itself, otherwise MachineMmc1 or MachineMmc3, whose registers live in these
	uint8_t _gameChip = 0;
	DrPcJrMmc1 _mmc1;
	DrPcJrMmc3 _mmc3;
	//The game's eight 1KB CHR pages. A board that moves a whole 8KB at a time fills
	//these from _gameChrBank; the two controllers name them one by one.
	uint16_t _gameChrPages[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

	uint8_t _exRamNt[0x800] = {};
	uint16_t _extNtAddr = 0;
	int8_t _extFetchCounter = 0;

	//$418D bit 1 turns the latch on; bit 0 then picks between the two tile formats -
	//mode 2 draws ordinary 2bpp tiles out of a per-cell 4K bank, mode 3 draws 1bpp
	//glyphs out of a 2K bank and colours them from the per-cell byte.
	bool ExtLatchEnabled() { return (_regs[0x0D] & 0x03) >= 2; }
	uint16_t ChrMask1K() { return (uint16_t)(((_regs[0x03] & 0x0F) << 5) | 31); }
	uint8_t ChrMask4K() { return (uint8_t)(((_regs[0x03] & 0x0F) << 3) | 7); }
	uint8_t ChrMask2K() { return (uint8_t)(((_regs[0x03] & 0x0F) << 4) | 15); }
	uint8_t ChrMask8K() { return (uint8_t)(((_regs[0x03] & 0x0F) << 2) | 3); }
	uint8_t PrgMask8K() { return (uint8_t)(((_regs[0x03] >> 4) << 2) | 3); }
	uint8_t PrgMask16K() { return (uint8_t)(((_regs[0x03] >> 4) << 1) | 1); }
	uint8_t PrgMask32K() { return (uint8_t)(_regs[0x03] >> 4); }
	//How much PRG-RAM the game was given, in 8KB banks
	uint8_t GamePrgBankCount() { return (uint8_t)((((_regs[0x03] >> 4) & 0x0F) + 1) << 2); }

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

	//--- speech ------------------------------------------------------------------------
	//The same LPC-10 synthesizer the other learning machines here carry, in its plainest
	//form: no stream header, the "PE" coefficient set, and nothing marking the end of a
	//phrase. It is fed a nibble at a time through $41AC - a write of $5x carries the low
	//four bits, least significant first, so two writes make a byte with the first nibble in
	//the low half. Any other value resets the decoder, which is how one phrase is separated
	//from the next.
	unique_ptr<BbkLpcAudio> _speech;
	uint8_t _speechByte = 0;
	uint8_t _speechNibbleCount = 0;

	void WriteSpeech(uint8_t value)
	{
		if((value & 0xF0) != 0x50) {
			_speechNibbleCount = 0;
			_speechByte = 0;
			_speech->WriteControl(0);
			_speech->WriteControl(1);
			return;
		}

		if(_speechNibbleCount == 0) {
			_speechByte = (uint8_t)(value & 0x0F);
			_speechNibbleCount = 1;
		} else {
			_speechByte |= (uint8_t)(value << 4);
			_speechNibbleCount = 0;
			_speech->WriteData(_speechByte);
		}
	}

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
	DrPcJrCdDrive _cd;
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

	//The configured folder and the rom's own can name the same place in two spellings - the
	//one built from the rom path ends in a separator and the setting does not - and listing
	//it twice puts every disk in the list twice.
	static bool SameFolder(const string& a, const string& b)
	{
		auto tidy = [](string path) {
			std::transform(path.begin(), path.end(), path.begin(), [](char c) {
				return c == '\\' ? '/' : (char)::tolower((uint8_t)c);
			});
			while(!path.empty() && path.back() == '/') { path.pop_back(); }
			return path;
		};
		return tidy(a) == tidy(b);
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

		string baseName = FolderUtilities::GetFilename(romPath, false);
		vector<string> folders = { FolderUtilities::GetFolderName(romPath) };
		string configured = GetConfiguredDiskFolder();
		if(!configured.empty() && !SameFolder(configured, folders[0])) {
			folders.push_back(configured);
		}

		//The KW machines' player front end reads a CD as well as a floppy. Only they have
		//one - the two 32KB machines use a different, unimplemented interface - so the
		//search is gated on the rom type and nothing changes for anything else.
		//
		//This has to happen before anything below can return. The drive is part of the
		//machine, so whether it is there cannot depend on what is in the floppy drive - and
		//the floppy paths below return early once they have something to mount. Leaving the
		//drive marked absent makes $41AF answer out of the last value written to it instead
		//of from the drive, and its bit 6 - the busy line - then stays set for good: the
		//BIOS send routine at $5860 waits for a drive that is never ready, the player's
		//retry at $672D never gives up, and its event loop stops running, so the front end
		//takes no further input at all. That only happened once a disk had been chosen from
		//the media list, because only then are the persisted-disk statics set - which is why
		//it never showed up in a headless run.
		if(_romType == 1 || _romType == 2) {
			_cd.SetPresent(true);
			//A disc chosen from the media list survives a power cycle, the way a floppy
			//does. It is the only way onto the KW3000 at the moment: it does not look at the
			//drive again once it has settled, so a disc going in mid-run is never noticed.
			if(_persistedDiscRom == romPath && !_persistedDiscPath.empty()) {
				_cd.LoadDisc(_persistedDiscPath);
			}

			if(!_cd.IsMounted() && !(_persistedDiscRom == romPath && _persistedDiscPath.empty())) {
				for(string& folder : folders)
				for(string ext : { ".cue", ".CUE", ".bin", ".BIN", ".iso", ".ISO" }) {
					if(_cd.LoadDisc(FolderUtilities::CombinePath(folder, baseName + ext))) {
						break;
					}
				}
			}
		}

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

	//Full path of whatever is in a drive, or "" when both are empty. The KW machines take a
	//disc as well as a floppy and the same list offers both, so the disc answers when the
	//floppy drive is empty.
	string GetCurrentDiskFilename()
	{
		if(_fdc.IsDiskInserted()) {
			return _fdc.GetDiskFilename();
		}
		return _cd.IsMounted() ? _cd.GetDiscPath() : "";
	}

	//The mounted disc on its own. GetCurrentDiskFilename prefers whatever is in the floppy
	//drive, but a machine can have both, and the video tracks are only ever on the disc.
	string GetDiscPath() { return _cd.IsMounted() ? _cd.GetDiscPath() : ""; }

	//The video the machine has asked for, and the answer that its wait loop is holding out
	//for. Both belong to the drive; this is only the way out to the front end.
	bool TakeVideoPlayRequest(uint8_t& track, uint32_t& startMsf, uint32_t& endMsf)
	{
		return _cd.TakePlayRequest(track, startMsf, endMsf);
	}

	void EndVideoPlayback(bool completed) { _cd.EndPlayback(completed); }

	static bool IsDiscImage(const string& path)
	{
		string ext = path.size() >= 4 ? path.substr(path.size() - 4) : string();
		std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return (char)::tolower((uint8_t)c); });
		return ext == ".cue" || ext == ".bin" || ext == ".iso";
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
		if(!configured.empty() && !SameFolder(configured, folders[0])) {
			folders.push_back(configured);
		}

		//The KW machines read a CD too, so their list offers discs alongside floppies; the
		//two 32KB machines have a different, unimplemented CD interface and are left alone.
		std::unordered_set<string> extensions = { ".img", ".ima" };
		if(_romType == 1 || _romType == 2) {
			extensions.insert(".cue");
			extensions.insert(".bin");
			extensions.insert(".iso");
		}

		vector<string> files;
		for(string& folder : folders) {
			for(string& file : FolderUtilities::GetFilesInFolder(folder, extensions, false)) {
				files.push_back(file);
			}
		}
		std::sort(files.begin(), files.end());

		//A .cue and the image it names are one disc; drop the image so the list has one
		//entry per medium rather than two. Matched on the filename alone - the resolved path
		//comes back with a separator the folder listing does not use, so comparing whole
		//paths misses. Same as the YuXing list does.
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
			string name = leaf(file);
			if(name.size() >= 4 && name.compare(name.size() - 4, 4, ".cue") == 0) {
				named.push_back(leaf(DrPcJrCdDrive::ResolveCueSheet(file)));
			}
		}
		files.erase(std::remove_if(files.begin(), files.end(), [&named, &leaf](const string& file) {
			return std::find(named.begin(), named.end(), leaf(file)) != named.end();
		}), files.end());
		return files;
	}

	void EjectDisk()
	{
		auto lock = _emu->AcquireLock();
		if(_cd.IsMounted() && !_fdc.IsDiskInserted()) {
			MessageManager::DisplayMessage("Dr.PC Jr.", "Disc ejected: " +
				FolderUtilities::GetFilename(_cd.GetDiscPath(), true));
			_cd.EjectDisc();
			_diskIndex = -1;
			_persistedDiscRom = _emu->GetRomInfo().RomFile.GetFilePath();
			_persistedDiscPath = "";
			return;
		}

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

		if(IsDiscImage(disks[index])) {
			//A disc, not a floppy. The player reads the drive through its own link, so
			//nothing has to be signalled here - it polls and picks the new disc up.
			if(_cd.LoadDisc(disks[index])) {
				_diskIndex = (int32_t)index;
				_persistedDiscRom = _emu->GetRomInfo().RomFile.GetFilePath();
				_persistedDiscPath = disks[index];
				MessageManager::DisplayMessage("Dr.PC Jr.", "Disc inserted: " +
					FolderUtilities::GetFilename(disks[index], true));
			}
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
	uint32_t _kbdPollFrame = 0;

	//Typematic repeat. A real AT keyboard resends the make code while a key stays down,
	//after a delay - about half a second, then some ten a second. The reference times this
	//off the wall clock, which would make the rate depend on how fast the host happens to
	//be running and stop a recording replaying the same way twice, so it is counted in
	//frames here instead. Repeated makes only: the break code belongs to a real release.
	static constexpr uint16_t KbdRepeatDelay = 25;	//500ms at 50Hz
	static constexpr uint16_t KbdRepeatPeriod = 5;	//100ms
	uint16_t _kbdHold[Sb2kKeyboard::KeyCount] = {};

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

		uint8_t raw = (uint8_t)(keyEvent & 0xFF);
		uint8_t code = KbdSet1ToSet2(raw);
		if(!code) {
			return;
		}

		bool release = (keyEvent & 0x100) != 0;
		if(release && _diskType == 1) {
			//Running its own software, the machine's keyboard reports presses and nothing
			//else. Sending the release pair as well puts three bytes and three interrupts
			//behind every keystroke, and the program then acts on the release rather than
			//the press - which is what put it a keystroke behind the typist.
			return;
		}

		//The fake shift is lifted straight after the code rather than held until the key
		//comes back up. The machine reads the pair in one go either way, and while it is
		//running its own software the keyboard reports no releases at all - holding the
		//shift there would leave one down that nothing could ever lift.
		bool fakeShift = !release && KbdFakeShiftWanted(raw);
		if(fakeShift) {
			KbdPushKey(KbdLeftShift, false);
		}
		KbdPushKey(code, release);
		if(fakeShift) {
			KbdPushKey(KbdLeftShift, true);
		}
		KbdAssertIrq(true);
	}

	//One make or break
	void KbdPushKey(uint8_t code, bool release)
	{
		if(release) {
			//Release: $F0 then the code
			KbdPush(0xF0);
		}
		KbdPush(code);
	}

	//The navigation cluster sits on the keypad's codes, and with the Num Lock lamp lit the
	//machine reads those codes as digits. A keyboard gets the arrow keys through by
	//bracketing them in a shift the typist never pressed, which the machine's own decoder
	//takes as "this one is a cursor key after all". Without it the arrows type numbers into
	//the software's menus instead of moving the selection through them, and the only way to
	//steer anything is to turn Num Lock off by hand. A shift the typist really is holding
	//already reaches the same decision, so it wants no help.
	//Num Lock, as the machine itself has it. There is nowhere else to read it: the lamp
	//command that would tell a real keyboard never reaches this one, because the machine
	//asks to send in one step - data low, clock left alone - and this link only answers
	//the two-step request. The machine keeps the answer in the same PRG-RAM the mouse
	//report goes into, in the shift-flag byte's bit 5.
	bool KbdNumLockLit()
	{
		if(_loadMode) {
			return false;
		}

		uint32_t offset = SystemBankOffset(0xFFED);
		return offset < _workRamSize && (_workRam[offset] & 0x20) != 0;
	}

	//Which decoder is reading is visible in the frame handler the machine has installed, at
	//the NMI jump the vector points at. The machine's own decoder sends it to $E21E while a
	//program of its own is running and the first desktop sends it to $FF1C; both want the
	//bracket. The console personality leaves it at $FE18, and its decoder reads the bracket
	//as a shift the typist is holding - so there the arrows come out as digits instead.
	//Unlike the personality register this follows the handler in and out, which is what the
	//earlier attempt at telling the two apart got wrong.
	bool KbdConsoleDecoder()
	{
		uint32_t lo = SystemBankOffset(0xFF81);
		uint32_t hi = SystemBankOffset(0xFF82);
		if(lo >= _workRamSize || hi >= _workRamSize) {
			return false;
		}
		return _workRam[lo] == 0x18 && _workRam[hi] == 0xFE;
	}

	bool KbdFakeShiftWanted(uint8_t raw)
	{
		if(KbdConsoleDecoder() || !KbdNumLockLit() || !(raw & 0x80)) {
			return false;
		}

		uint8_t base = (uint8_t)(raw & 0x7F);
		if(base < 0x47 || base > 0x53) {
			return false;
		}

		NesControlManager* controls = (NesControlManager*)_console->GetControlManager();
		shared_ptr<Sb2kKeyboard> kbd = controls->GetControlDevice<Sb2kKeyboard>();
		return kbd && !kbd->IsShiftHeld();
	}

	//Left shift, in the set the keyboard sends
	static constexpr uint8_t KbdLeftShift = 0x12;

	//Modifiers do not repeat - holding shift must not fill the queue with shift.
	static bool KbdRepeats(uint8_t code)
	{
		return code != 0x12 && code != 0x59 && code != 0x14 && code != 0x11;
	}

	void KbdRepeatKeys()
	{
		//Running its own software the machine reports presses only, and does not repeat
		//them either - see KbdPollKeys.
		if(_diskType == 1) {
			return;
		}

		NesControlManager* controls = (NesControlManager*)_console->GetControlManager();
		shared_ptr<Sb2kKeyboard> kbd = controls->GetControlDevice<Sb2kKeyboard>();
		if(!kbd) {
			return;
		}

		for(uint8_t i = 0; i < Sb2kKeyboard::KeyCount; i++) {
			uint8_t raw = Sb2kKeyboard::GetScanCode(i);
			uint8_t code = KbdSet1ToSet2(raw);
			if(!kbd->IsKeyHeld(i) || !code || !KbdRepeats(code)) {
				_kbdHold[i] = 0;
				continue;
			}

			_kbdHold[i]++;
			if(_kbdHold[i] >= KbdRepeatDelay && (_kbdHold[i] - KbdRepeatDelay) % KbdRepeatPeriod == 0) {
				//A repeat is a fresh make, fake shift and all
				bool fakeShift = KbdFakeShiftWanted(raw);
				if(fakeShift) {
					KbdPushKey(KbdLeftShift, false);
				}
				KbdPushKey(code, false);
				if(fakeShift) {
					KbdPushKey(KbdLeftShift, true);
				}
				KbdAssertIrq(true);
			}
		}
	}

	//The Bung machine's PPU is a famiclone part: it clears the vblank flag on a $2002 read
	//but has no 2C02 "read one dot before vblank" race. The software's main loop polls
	//$2002 in a tight wait, so with the race in play a third of the NMIs never fire, and
	//the frame handler - which is where this machine polls its keyboard - starves.
	bool EnablePpuNmiSuppressRace() override { return false; }

	//One step of the two-wire state machine, run once per CPU cycle. The lines change state
	//every 64 cycles, which is the ~35us the real device takes.
	//The keyboard's bit in $418F has to latch rather than mirror the interrupt line. The
	//processor samples the interrupt at an instruction boundary and fetches the vector
	//several cycles later, and the keyboard drops its line as soon as its queue drains, so
	//a handler reading a live view can find nothing asserted and no way to tell what woke
	//it. Reading the register answers the interrupt and clears the bit.
	void KbdAssertIrq(bool raise)
	{
		_kbdRaiseIrq = raise;
		if(raise) {
			_irqStatus |= 0x10;
		}
	}

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

					//A key sends more than one byte - a release is $F0 and then the code - and
					//the host is told once per byte, not once per key. Announcing only the
					//first leaves the rest sitting in the queue until the next keypress
					//raises another interrupt, which puts every key one keystroke behind.
					KbdAssertIrq(_kbdQueueLen > 0);
				}
			}
		}
	}

	uint8_t BiosBank() { return _regs[0x00] & 0x07; }
	uint8_t BiosGroup() { return _regs[0x2F] & 0x03; }
	bool SaveRamEnabled() { return (_regs[0x00] & 0x80) != 0; }
	uint8_t PrgReadBank() { return _regs[0x10] & 0x0F; }
	uint8_t PrgWriteBank() { return _regs[0x11] & 0x0F; }

	//The KW3000 lays its window out differently - see UpdatePrgMapping. $41B4 names an 8KB
	//ROM bank in its low nibble and opens the save RAM with bit 7; $41B7 picks which 8KB of
	//that save RAM; $4190 names one 16KB RAM bank rather than the KW2000's 32KB one.
	uint8_t Kw3000RomBank() { return _regs[0x34] & 0x0F; }
	bool Kw3000SaveRamEnabled() { return (_regs[0x34] & 0x80) != 0; }
	uint8_t Kw3000SaveRamBank() { return _regs[0x37] & 0x03; }
	uint32_t Kw3000RamBank() { return _regs[0x10] & ((PrgMask() << 1) | 1); }

	//$6000-$7FFF on the KW3000, which does NOT follow load mode - the reference maps this
	//window the same way whether the machine is in load mode or running its own image, and
	//it is the save RAM $41B7 names rather than the KW2000's BIOS-group slice.
	void MapKw3000SaveRam()
	{
		uint32_t offset = SramBase + (Kw3000SaveRamEnabled() ? Kw3000SaveRamBank() * 0x2000 : 0);
		SetCpuMemoryMapping(0x6000, 0x7FFF, PrgMemoryType::WorkRam, offset, MemoryAccessType::Read);
	}

	//A transfer the drive carries out itself is given its destination in registers, and
	//the BIOS writes them immediately before the command goes out. A file arrives in two
	//pieces and each has its own set:
	//
	//  EDAD: LDX #$00 / STX $41A8      first piece:  $41A8 / $41A9 / $41AA
	//  EDB4: LDX #$02 / LSR A / ROR $00 / DEX / BNE      a 16-bit shift right by 2
	//  EDBC: STA $41AA / LDA $00 / STA $41A9
	//  EDE5: STX $41B8                 second piece: $41B8 / $41B9 / $41BA
	//  EDE8: LDX #$03 ...              shifted by 3 instead
	//
	//so the pair is an address with its low byte implied zero: $41A9 carries bits 8-15 and
	//$41AA bits 16-23. Measured, the first piece of a program 3 banks in comes out as
	//$00C000, which is where it has to be.
	//
	//Which set applies is simply whichever was written last - the BIOS fills one and sends
	//a command, fills the other and sends the next.
	uint32_t PlacedDestination()
	{
		uint8_t mid = _placeUseB ? _regs[0x39] : _regs[0x29];
		uint8_t high = _placeUseB ? _regs[0x3A] : _regs[0x2A];
		return (uint32_t)(((high << 8) | mid) << 8);
	}

	void PlaceDiscTransfer()
	{
		const uint8_t* data = nullptr;
		uint32_t len = 0;
		if(!_cd.TakePlacedTransfer(data, len)) {
			return;
		}

		//The two sets do not address the same memory. A file's first piece is the program -
		//it carries its own vectors at the top of its first 16KB, which is what the launcher
		//jumps through - so it goes to PRG-RAM. Its second piece is the pictures, far too
		//large to sit above it, and goes to the video memory, which is the same size and
		//holds exactly one of them. Both are addressed from zero, which is only possible
		//because they are different memories.
		uint32_t at = PlacedDestination();
		if(_placeUseB) {
			if(at < _chrRamSize) {
				memcpy(_chrRam + at, data, std::min(len, (uint32_t)(_chrRamSize - at)));
			}
		} else if(at < SramBase) {
			memcpy(_workRam + at, data, std::min(len, (uint32_t)(SramBase - at)));
		}
	}

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
			if(_romType == 2) {
				//The KW3000 divides the window three ways instead of two. The top 8KB is a
				//fixed ROM bank - the first of the image, not the last - which is why its
				//reset vector reads out of offset $1FFA and points at $E000. Below that
				//$C000-$DFFF is a banked 8KB ROM window, and only $8000-$BFFF is RAM: one
				//16KB bank, where the KW2000 has a 32KB one covering everything.
				SelectPrgPage(6, 0);
				SelectPrgPage(7, 1);
				SelectPrgPage(4, (uint16_t)(Kw3000RomBank() * 2));
				SelectPrgPage(5, (uint16_t)(Kw3000RomBank() * 2 + 1));
				SetCpuMemoryMapping(0x8000, 0xBFFF, PrgMemoryType::WorkRam, Kw3000RamBank() * 0x4000, MemoryAccessType::Read);

				//With $41B4 bit 7 closed nothing can write here, so bank 0 reads back as the
				//zeroes the reference's untouched scratch RAM would give.
				MapKw3000SaveRam();
				return;
			}

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

		//Once a game off a disc has taken over, $8000-$FFFF is the game sitting in PRG-RAM,
		//not the BIOS rom - mapping the rom there is what crashed it on launch. The window
		//is four 8KB banks the game picks itself: StartGameMode lays out the one it starts
		//in and SetGameBank moves it after that, the same way the board it was converted
		//from would have.
		//
		//$6000-$7FFF is the game's save RAM. Reads come from SramBase; writes still go
		//through the BIOS-group banking below, so a game that actually uses SRAM would read
		//back the wrong 8KB - untested, no disc game on hand touches it.
		if(GameWindow()) {
			for(int i = 0; i < 4; i++) {
				uint16_t start = (uint16_t)(0x8000 + i * 0x2000);
				SetCpuMemoryMapping(start, (uint16_t)(start + 0x1FFF), PrgMemoryType::WorkRam,
					(_gamePrgBanks[i] & PrgMask8K()) * 0x2000, MemoryAccessType::Read);
			}
			SetCpuMemoryMapping(0x6000, 0x7FFF, PrgMemoryType::WorkRam, SramBase, MemoryAccessType::Read);
			return;
		}

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
		if(_romType == 2) {
			MapKw3000SaveRam();
		} else {
			SetCpuMemoryMapping(0x6000, 0x7FFF, PrgMemoryType::WorkRam, SramBase + BiosGroup() * 0x2000, MemoryAccessType::Read);
		}
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

		//A game off a disc is a cartridge, and a cartridge's pattern window is a plain 8KB
		//run of CHR-RAM at the bank the game asked for. It cannot come from $4198-$419F: the
		//loader clears those on its way out and the width in $4181 is 1KB, which folds all
		//eight pages onto the first 1KB of the game's tiles. That it came out linear at all
		//is an accident of nothing having re-run this since load mode - which held for a
		//game that only ever wants its first bank, and left the second bank unreachable.
		if(GameWindow()) {
			for(int i = 0; i < 8; i++) {
				SelectChrPage(i, (uint16_t)(_gameChrPages[i] & ChrMask1K()));
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
	//The four arrangements $42FC-$42FF and the two controllers all name with two bits
	void SetMirroringValue(uint8_t value)
	{
		switch(value & 3) {
			case 0: SetMirroringType(MirroringType::ScreenAOnly); break;
			case 1: SetMirroringType(MirroringType::ScreenBOnly); break;
			case 2: SetMirroringType(MirroringType::Vertical); break;
			default: SetMirroringType(MirroringType::Horizontal); break;
		}
	}

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
				SetMirroringValue(_mirroring);
				break;
		}
	}

	//The machine is running its own software rather than a cartridge personality
	bool SystemMode() { return ((_regs[0x01] >> 4) & 0x03) == 1; }

	//The opposite end: a disc game has taken the machine over. $8000-$FFFF is the game
	//rather than the BIOS, and the learning machine's own devices are out of the picture -
	//the same condition UpdatePrgMapping uses to hand the window to the game.
	bool CartridgeMode() { return _gameLaunched && MachineMode() == MachineOld; }

	//A game holds the window whichever of the three cartridge personalities it asked
	//for; only MachineNew leaves it to the machine's own software.
	bool GameWindow() { return _gameLaunched && MachineMode() != MachineNew; }

	//The window a game starts in, and what it banks itself into afterwards. A converted
	//cartridge keeps its own board's banking, so the machine has to offer the same thing:
	//the hand-over value picks which shape, and a write anywhere in $8000-$FFFF is that
	//board's bank register.
	void SetGamePrg16K(uint8_t slot, uint8_t bank)
	{
		bank &= PrgMask16K();
		_gamePrgBanks[slot] = (uint8_t)(bank * 2);
		_gamePrgBanks[slot + 1] = (uint8_t)(bank * 2 + 1);
	}

	//A board that moves a whole 8KB of CHR at a time still has to leave the page list
	//behind it, because that is what the window is laid out from now that two of the
	//personalities name pages one by one.
	void SetGameChr8K(uint8_t bank)
	{
		_gameChrBank = bank;
		bank &= ChrMask8K();
		for(int i = 0; i < 8; i++) {
			_gameChrPages[i] = (uint16_t)(bank * 8 + i);
		}
	}

	void SetGamePrg32K(uint8_t bank)
	{
		bank &= PrgMask32K();
		for(int i = 0; i < 4; i++) {
			_gamePrgBanks[i] = (uint8_t)(bank * 4 + i);
		}
	}

	//The size of the game's CHR, in 8KB banks. The loader leaves the game's header in the
	//machine's own RAM at $600 and reads it back from there itself, so that is where the
	//two shapes that ask about CHR have to look for it.
	uint8_t GameChrBankCount()
	{
		uint8_t* ram = _console->GetMemoryManager()->GetInternalRam();
		static const char* Magic = "FC GAMES";
		for(int i = 0; i < 8; i++) {
			if(ram[0x600 + i] != (uint8_t)Magic[i]) {
				return 0;
			}
		}
		return ram[0x609];
	}

	//A write into the game's own window. The byte is masked with the size the game's header
	//declared, because the games do not write a bare bank number: the usual idiom reads a
	//byte out of the program and writes it straight back, so that the value on the bus
	//matches what is already there. The table one game reads holds $30, $31, $32, $33 for
	//banks 0-3 - everything above the size mask is that byte's own high bits and not part
	//of the selection.
	void SetGameBank(uint8_t value)
	{
		switch(_gameMode) {
			case 1:
				SetGamePrg16K(0, (uint8_t)((value >> 2) & 0x0F));
				SetGameChr8K((uint8_t)(value & 0x03));
				break;

			case 2:
			case 3:
				//Mode 2 banks the low half of the window, mode 3 the high half
				SetGamePrg16K((uint8_t)((_gameMode << 1) - 4), value);
				break;

			case 4:
				SetGamePrg32K((uint8_t)((value >> 4) & 0x07));
				SetGameChr8K((uint8_t)((value & 0x03) | ((value & 0x40) >> 4)));
				break;

			case 5:
			case 6:
				SetGameChr8K(value);
				break;

			default:
				//0 and 7 do not bank
				return;
		}

		UpdatePrgMapping();
		UpdateChrMapping();
	}

	//The whole window off the top of the PRG. A game with no CHR of its own only gets half
	//the PRG-RAM to take it from, so the top is the middle of the image rather than its end -
	//which is why this cannot be written out as "the last four banks".
	void SetGameTop32K(uint8_t prgBanks, uint8_t chrBanks)
	{
		uint8_t top = (uint8_t)(prgBanks >> ((prgBanks == 0x20 && chrBanks == 0) ? 1 : 0));
		for(int i = 0; i < 4; i++) {
			_gamePrgBanks[i] = (uint8_t)(top - 4 + i);
		}
	}

	//Called on the hand-over, once the game is in place and before it runs. The two sizes
	//are parameters because a game loaded on its own has no machine to ask: it carries them
	//in its own header, where the disc path reads them off $4183 and out of the loader's
	//copy of that header in RAM.
	void StartGameMode(uint8_t handover, uint8_t prgBanks, uint8_t chrBanks)
	{

		//Bits 5-7 name the shape; zero there leaves the machine to pick by size
		_gameMode = (uint8_t)((handover >> 5) & 0x07);
		if(_gameMode == 0) {
			_gameMode = (prgBanks == 0x20) ? 1 : 2;
		}

		//The shape follows every write here, but the window it starts in is laid out ONCE.
		//The startup stub a game brings with it writes this register a second time as it
		//runs - measured, the loader writes it and the stub writes it again in the same
		//frame - and re-laying the window underneath throws away what the stub has already
		//put there. This is the mirror image of the mirroring, which has to follow every
		//write; getting either of them the wrong way round garbles the game.
		//Shapes 5 and 6 do not bank PRG at all, so their window is the top 32KB and there is
		//nothing for the game to have put there - which makes this the one selection that is
		//laid out on EVERY hand-over rather than only the first. A game's startup stub relies
		//on it: one of them banks its way along the PRG copying its tiles into CHR-RAM, and
		//then names shape 6 and reads the window straight back, expecting the game to be
		//there. That stub checks the byte it reads against a constant it knows, which is what
		//pins the layout down - with the top 32KB in place the check passes, and with the
		//window the stub left behind, or with the layout the other shapes start in, it does
		//not.
		if(_gameMode == 5 || _gameMode == 6) {
			SetGameTop32K(prgBanks, chrBanks);
		}

		if(_gameReset) {
			return;
		}
		_gameReset = true;
		SetGameChr8K(0);

		//Most shapes start with the first 16KB low and the last 16KB high
		_gamePrgBanks[0] = 0;
		_gamePrgBanks[1] = 1;
		_gamePrgBanks[2] = (uint8_t)(prgBanks - 2);
		_gamePrgBanks[3] = (uint8_t)(prgBanks - 1);

		if(_gameMode == 1) {
			SetGameTop32K(prgBanks, chrBanks);
		} else if(_gameMode == 3) {
			//This shape banks the high half, so the fixed half is the one that goes low
			_gamePrgBanks[0] = (uint8_t)(prgBanks - 2);
			_gamePrgBanks[1] = (uint8_t)(prgBanks - 1);
			_gamePrgBanks[2] = (uint8_t)(prgBanks - 4);
			_gamePrgBanks[3] = (uint8_t)(prgBanks - 3);
		} else if(_gameMode == 4 && chrBanks == 0) {
			_gamePrgBanks[0] = 0;
			_gamePrgBanks[1] = 1;
			_gamePrgBanks[2] = 0x0E;
			_gamePrgBanks[3] = 0x0F;
		}
	}

	//The imitated controller has moved something. Both of them speak in the banks their
	//own hardware used, so what comes back is four 8KB PRG banks and eight 1KB CHR
	//pages, and the machine's own size masks are applied where the window is laid out -
	//exactly as they are for a board that banks itself.
	void ApplyGameChip(bool takeMirroring)
	{
		if(_gameChip == MachineMmc1) {
			_mmc1.GetPrgBanks(GamePrgBankCount(), _gamePrgBanks);
			_mmc1.GetChrPages(_gameChrPages);
			_mirroring = _mmc1.Mirroring();
		} else if(_gameChip == MachineMmc3) {
			_mmc3.GetPrgBanks(GamePrgBankCount(), _gamePrgBanks);
			_mmc3.GetChrPages(_gameChrPages);
			_mirroring = _mmc3.Mirroring();
		} else {
			return;
		}

		//A controller names the arrangement itself, and gets it - the reference lets the
		//chip win here rather than running the selection back through $4182, and 114 of
		//the 121 games concerned have $4182 deferring to the selection anyway. The
		//hand-over still goes through MirrorSync, so the arrangement a game starts with
		//is the machine's until the game says otherwise.
		if(takeMirroring) {
			SetMirroringValue(_mirroring);
		}
		UpdatePrgMapping();
		UpdateChrMapping();
	}

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

	//$4020-$5FFF, see the note on the array
	uint32_t GetMapperRamSize() override { return 0x2000; }
	uint32_t GetSaveRamSize() override { return 0; }

	bool EnableCpuClockHook() override { return true; }
	bool EnableCustomVramRead() override { return true; }
	bool EnableVramAddressHook() override { return true; }

	void NotifyVramAddressChange(uint16_t addr) override
	{
		//The imitated controller counts picture lines off this line going high
		if(_gameChip == MachineMmc3) {
			_mmc3.ClockA12(addr, _console->GetMasterClock());
		}

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

		//The per-cell bank table is a real thing the program uploads, not just something
		//inferred from $4194. It is readable back at PPU $1800-$1FFF while $4198 is $3F, and
		//the write side of that window was never implemented - so the shadow only ever held
		//the $4194 stamps, and a screen drawn from the program's own table came out garbled
		//whenever bAUTO was not armed. Record what the program actually writes.
		if(_regs[0x18] == 0x3F && addr >= 0x1800 && addr < 0x2000) {
			_exRamNt[addr - 0x1800] = value;
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

		//Look for a key transition once a frame, as the picture ends. Counting cycles
		//instead drifts against the picture, so the interrupt behind a keystroke lands at a
		//different point of the frame every time - sometimes in the middle of a redraw,
		//which is what left half-drawn text on the screen.
		uint32_t frame = _console->GetPpu()->GetFrameCount();
		if(frame != _kbdPollFrame) {
			_kbdPollFrame = frame;
			KbdPollKeys();
			KbdRepeatKeys();
			_cd.ClockFrame();
		}

		KbdClock();
		MousePoll();
		_speech->Clock();
		_printer.Clock();
		_fdc.Clock();

		//The controller's own counter joins the two the machine has. It has to be held
		//and answered rather than pulsed: this line is rewritten every CPU clock.
		if(_kbdRaiseIrq || _lineIrqPending || _counterIrqPending || (_gameChip == MachineMmc3 && _mmc3.IrqPending())) {
			_console->GetCpu()->SetIrqSource(IRQSource::External);
		} else {
			_console->GetCpu()->ClearIrqSource(IRQSource::External);
		}

		if(_irqEnabled) {
			//$4182 bits 0-1 pick how the counter runs: 2 counts up to $FFFF, 3 counts down
			//to zero, and either way the IRQ fires once and disables itself.
			uint8_t irqType = _regs[0x02] & 0x03;
			if(irqType == 1) {
				//Mode 1 does not use the cycle counter at all: it counts whole scanlines from
				//the value $41A0 was last loaded with, and trips when that eight-bit count
				//wraps. The system software splits the screen with it - the top band is the
				//console page and the bottom one the status line - by arming a one-line delay
				//in vblank and re-arming for the band boundary from inside the handler.
				int32_t scanline = _console->GetPpu()->GetCurrentScanline();
				if(scanline >= 0 && scanline < 240 && scanline != _lastIrqScanline) {
					_lastIrqScanline = scanline;
					if(++_lineCounter == 0) {
						//Held until the handler answers it by clearing $41A3. A single-cycle
						//pulse would be lost: the keyboard arm above rewrites the same line
						//every CPU clock and would drop it before the core sampled it.
						_lineIrqPending = true;
					}
				}
			} else if(irqType == 0 || irqType == 2) {
				//Mode 0 counts the same way mode 2 does. The disc games arm the counter once
				//a frame with $4182 holding neither 1 nor 3 in its low bits, and the count
				//they load - $9D84, so $627B clocks short of the wrap - lands the interrupt
				//around line 200 measured from the vblank they arm it in, which is where a
				//status line sits. Left unhandled the interrupt never came, nothing reset the
				//scroll part way down, and the status line scrolled with the game.
				if(++_irqCounter >= 0xFFFF) {
					_irqEnabled = false;
					_counterIrqPending = true;
				}
			} else if(irqType == 3) {
				if(_irqCounter == 0 || --_irqCounter == 0) {
					_irqEnabled = false;
					_irqCounter = 0;
					_counterIrqPending = true;
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
		} else if(_romType == 2) {
			//InitMapper() ran before the rom type was known, so redo the mapping now that
			//the KW3000 layout applies
			UpdatePrgMapping();
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

		//A game smaller than the window it is given has to appear in all of it - a 16KB one
		//is a half-size board, and its vectors live at the top of the window, not at the top
		//of its own image. On a disc the loader arranges that; here the file is all there is,
		//so repeat it. Without this a 16KB game reads its reset vector out of empty RAM.
		size_t span = (size_t)GamePrgBankCount() * 0x2000;
		if(!_cdvPrg.empty() && _cdvPrg.size() < span) {
			for(size_t at = _cdvPrg.size(); at < std::min((size_t)_workRamSize, span); at += _cdvPrg.size()) {
				memcpy(_workRam + at, _cdvPrg.data(),
					std::min(_cdvPrg.size(), std::min((size_t)_workRamSize, span) - at));
			}
		}
		if(!_cdvChr.empty()) {
			memcpy(_chrRam, _cdvChr.data(), std::min((size_t)_chrRamSize, _cdvChr.size()));
		}

		//A startup block goes where the header says, in the window the machine keeps at $6000
		if(_cdvHeader[0x0A] && _cdvHeader[0x0D] && !_cdvTrainer.empty()) {
			uint32_t offset = (uint32_t)((_cdvHeader[0x0A] << 8) & 0x1FFF);
			size_t len = std::min(_cdvTrainer.size(), (size_t)(0x2000 - offset));
			memcpy(_workRam + SramBase + offset, _cdvTrainer.data(), len);
		}

		//A game loaded on its own carries the hand-over the BIOS would have made for it, at
		//$0E of its header, and nothing here was acting on it: the window kept the machine's
		//own default and $4181 was never read for a controller either. Measured on three
		//files pulled off a disc - only the shape that does not bank at all came up, and the
		//two that do landed on a blank screen, one of them with the processor loose in RAM.
		_gameLaunched = true;
		if(MachineMode() == MachineMmc1 || MachineMode() == MachineMmc3) {
			_gameReset = true;
			_gameChip = MachineMode();
			if(_gameChip == MachineMmc1) {
				_mmc1.Reset();
			} else {
				_mmc3.Reset();
			}
			ApplyGameChip(false);
		} else if(MachineMode() == MachineOld) {
			StartGameMode(_cdvHeader[0x0E], GamePrgBankCount(), _cdvHeader[0x09]);
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
		memset(_mapperRam, 0, _mapperRamSize);
		_regs[0x03] = 0xFF;
		_loadMode = true;
		_irqCounter = 0;
		_irqEnabled = false;
		_lineCounter = 0;
		_lastIrqScanline = -2;
		_lineIrqPending = false;
		_counterIrqPending = false;
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
		_kbdPollFrame = 0;
		memset(_kbdHold, 0, sizeof(_kbdHold));

		_mouseEnabled = false;
		_mouseFrame = 0;
		_ntData = 0;
		_mirroring = 0;
		_logoMode = false;
		_autoBank = false;
		_gameLaunched = false;
		_gameMode = 0;
		_gamePrgBanks[0] = 0;
		_gamePrgBanks[1] = 1;
		_gamePrgBanks[2] = 2;
		_gamePrgBanks[3] = 3;
		_gameChrBank = 0;
		_gameReset = false;
		_gameChip = 0;
		_mmc1.Reset();
		_mmc3.Reset();
		for(int i = 0; i < 8; i++) {
			_gameChrPages[i] = (uint16_t)i;
		}
		_diskType = 0;
		memset(_exRamNt, 0, sizeof(_exRamNt));
		_extNtAddr = 0;
		_extFetchCounter = 0;

		_lptData = 0;
		_lptCtrl = 0;
		_printer.Reset();

		_speech.reset(new BbkLpcAudio(_console, BbkLpcAudio::LpcVariant::DrPcJr));
		_speechByte = 0;
		_speechNibbleCount = 0;

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
				//A disc game is a cartridge, and these are the joypad ports again. Left
				//answering zero the game polls them for ever and reads no buttons: it runs,
				//nothing the player does reaches it, and it plays itself to a game over.
				//Only the first port carries a joypad - the second holds the machine's mouse,
				//which is not on this bus at all - so a two-player game gets one pad. Moving
				//the mouse aside would desync every recording, so it stays where it is.
				//
				//This asks whether a game has the window, NOT whether it is the personality
				//that banks itself: a game that asked for one of the imitated controllers is
				//just as much a cartridge, and gating on the narrower test left every one of
				//them reading zero off both ports - 8 polls a frame, no buttons, for ever.
				if(GameWindow()) {
					return NesControls()->ReadRam(addr);
				}

				//The 32KB machines drive their own file browser off the joypad, so for those
				//the first port is a real one even while the BIOS has the machine. $F801
				//strobes $4016 and clocks eight bits out of it, taking bit 0 for the first
				//pad and bit 1 for the second the way a famiclone wires them, and acts on
				//what is newly pressed:
				//
				//  F801: LDX #$01 / STX $4016      strobe on
				//  F806: DEX / STX $4016           strobe off
				//  F810: LDA $4016
				//  F813: LSR A / ROL $8C           bit 0 -> the first pad
				//  F816: LSR A / ROL $C5           bit 1 -> the second
				//  F81C: LDA $8C / ORA $C5 / STA $8C
				//  F822: EOR $8B / AND $8C / STA $8D    newly pressed
				//
				//Answering zero left the browser unable to move its cursor or start
				//anything, which reads from the outside as a machine that has hung.
				//
				//The KW pair do not do this - they read their keyboard over $418E instead -
				//and their BIOS polls $4017 looking for a serial mouse the hardware reports
				//as absent, where it wants a clean zero rather than the open bus a
				//controller would leave there, or the poll sees phantom data.
				if(_romType == 0 && addr == 0x4016) {
					return NesControls()->ReadRam(addr);
				}
				return 0;

			case 0x4188: _fdc.MarkActivity(); return _fdc.Read(4);
			case 0x4189: _fdc.MarkActivity(); return _fdc.Read(5);
			case 0x418B: _fdc.MarkActivity(); return _fdc.Read(2);

			case 0x418E:
				//Bit 7 always reads set; the keyboard's own clock and data come back in bits 1
				//and 0, and the rest is whatever the host last wrote. Bit 6 is the floppy
				//drive's change line, inverted - set means nothing has changed. The disk driver
				//caches "the drive is where I left it" and only re-homes the head when this bit
				//goes low, so reporting it permanently high strands the head after the driver
				//has parked the controller: every later read asks for a cylinder the head is not
				//on. The same bit answers the BIOS call that asks whether the disk was swapped.
				_kbdRaiseIrq = false;
				return (uint8_t)((_kbdCtrl & 0x3C) | 0x80 | (_fdc.DiskChanged() ? 0 : 0x40) |
					(_kbdClock ? 0x02 : 0) | (_kbdData ? 0x01 : 0));

			case 0x418F: {
				//Which source is asking: bit 4 the keyboard, bit 5 the raster counter. The
				//shared handler dispatches on these, so with both reading zero every interrupt
				//looks alike and a line interrupt is mistaken for the keyboard, taken down a
				//path that never clears $41A3 and re-entered on the spot.
				//Bit 5 is the counter block, not the line counter alone: the cycle counter
				//shares the same registers and asks the same way. Leaving it out named no
				//source at all for an interrupt that was being held, so the handler took the
				//path that answers nothing and the machine re-entered it for ever.
				//
				//Reading here answers it, the way it answers the keyboard just below. The
				//other machine's software re-arms through $41A3 and so cleared it either way,
				//which is why only this one stopped.
				uint8_t status = (uint8_t)(_irqStatus | (_kbdRaiseIrq ? 0x10 : 0) |
					((_lineIrqPending || _counterIrqPending) ? 0x20 : 0));
				_irqStatus &= (uint8_t)~0x10;
				_kbdRaiseIrq = false;
				_counterIrqPending = false;
				return status;
			}

			case 0x41A1: return (uint8_t)(_irqCounter & 0xFF);
			case 0x41A2: return (uint8_t)(_irqCounter >> 8);

			//$41AB bit 4 and $41AF bit 0 are the drive's presence lines, reported the way the
			//reference reports them - set only once an image is actually mounted. The machine
			//boots to its own screen either way; it does not wait for a disk.
			case 0x41AB: return _fdc.IsDiskInserted() ? (_regs[0x2B] | 0x10) : _regs[0x2B];
			case 0x41AF: {
				uint8_t value = _fdc.IsDiskInserted() ? (_regs[0x2F] | 0x01) : _regs[0x2F];
				if(_cd.IsPresent()) {
					//Bits 7 and 6 belong to the drive; 0-1 are the BIOS group, so the rest
					//is left exactly as the machine last wrote it
					value &= 0x3F;
					value |= _cd.DataAvailable() ? 0x80 : 0x00;
					value |= _cd.Busy() ? 0x40 : 0x00;
				}
				return value;
			}

			//Bit 6 is the decoder's own "room for more" line. A BIOS that is feeding a
			//phrase waits on it between nibbles.
			case 0x41AC: return _speech->IsReady() ? 0x40 : 0x00;
			case 0x41AE: return _cd.IsPresent() ? _cd.ReadByte() : _regs[0x2E];
		}

		if(addr >= 0x4180 && addr <= 0x41BF) {
			return _regs[addr & 0x3F];
		}
		return _mapperRam[addr - 0x4020];
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		MountPairedFloppy();

		if(addr >= 0x6000) {
			if(_romType == 2 && addr < 0x8000) {
				//Same window as MapKw3000SaveRam, and like it this does not follow load
				//mode. With $41B4 bit 7 closed the write is dropped, as in the reference.
				if(Kw3000SaveRamEnabled()) {
					_workRam[SramBase + Kw3000SaveRamBank() * 0x2000 + (addr & 0x1FFF)] = value;
				}
				return;
			}

			if(_loadMode) {
				if(_romType == 2) {
					//Only the 16KB RAM bank takes writes up here; the rest is ROM
					if(addr <= 0xBFFF) {
						_workRam[Kw3000RamBank() * 0x4000 + (addr & 0x3FFF)] = value;
					}
					return;
				}

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
			} else if(CartridgeMode()) {
				//A cartridge banks itself by writing into its own window
				SetGameBank(value);
			} else if(_gameChip == MachineMmc1) {
				//The game keeps its own mapper writes and the machine is the controller
				if(_mmc1.Write(addr, value, _console->GetMasterClock())) {
					ApplyGameChip(true);
				}
			} else if(_gameChip == MachineMmc3) {
				if(_mmc3.Write(addr, value)) {
					ApplyGameChip(true);
				}
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
			_mapperRam[addr - 0x4020] = value;
		}

		if(addr == 0x4016) {
			//The KW3000 reports a mouse unconditionally; the KW2000 asks for one here. A
			//game writing here is only strobing the joypad, and taking that for a request is
			//what set the machine writing mouse reports over the game's own code. True of a
			//game under either imitated controller too, hence the wider test.
			if(!GameWindow()) {
				_mouseEnabled = (_romType == 2) || (value != 0);
			}

			//This is still the controller strobe - the mapper only listens in
			NesControls()->WriteRam(addr, value);
			return;
		}

		if(addr >= 0x4180 && addr <= 0x41BF) {
			_regs[addr & 0x3F] = value;
		}

		switch(addr) {
			case 0x41A9: _placeUseB = false; break;
			case 0x41B9: _placeUseB = true; break;

			case 0x4180:
			case 0x4190:
			case 0x4191:
			case 0x41AF:
				UpdatePrgMapping();
				break;

			case 0x41AE:
				if(_cd.IsPresent()) {
					_cd.WriteByte(value);
					PlaceDiscTransfer();
				}
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

			case 0x41AC: WriteSpeech(value); break;

			case 0x4198: case 0x4199: case 0x419A: case 0x419B:
			case 0x419C: case 0x419D: case 0x419E: case 0x419F:
				UpdateChrMapping();
				break;

			case 0x41A0:
				_lineCounter = value;
				break;

			case 0x41A1:
				_irqCounter = (uint16_t)((_irqCounter & 0xFF00) | value);
				break;
			case 0x41A2:
				_irqCounter = (uint16_t)((_irqCounter & 0x00FF) | (value << 8));
				break;
			case 0x41A3:
				_irqEnabled = (value & 0x01) != 0;
				_counterIrqPending = false;
				if(!_irqEnabled) {
					_lineIrqPending = false;
				} else {
					//Arming starts the count on the next line, not part-way through this one
					_lastIrqScanline = _console->GetPpu()->GetCurrentScanline();
				}
				_console->GetCpu()->ClearIrqSource(IRQSource::External);
				break;

			case 0x41A5:
				//The KW2000 BIOS unpacks its ROM into PRG-RAM and then writes here to hand
				//over to the copy. Everything above $6000 changes meaning at this point.
				//Leaving load mode on instead is demonstrably wrong: the very next thing the
				//BIOS does is set $4190 to a bank it never filled.
				if(_romType != 2) {
					_loadMode = false;
					UpdatePrgMapping();
				}
				break;

			case 0x41B5:
				//The KW3000 hands over here instead, and unlike the KW2000 it can go back:
				//bit 5 is load mode itself rather than a one-way trigger.
				if(_romType == 2) {
					_loadMode = (value & 0x20) != 0;
					UpdatePrgMapping();
				}
				break;

			case 0x4181:
				//Bit 6 says the machine is running one of its own games, which puts it on
				//the same footing as having its operating system in the drive - and the
				//keyboard is quieter in that mode, see KbdPollKeys.
				if(value & 0x40) {
					_diskType = 1;
				}
				UpdatePrgMapping();
				break;

			case 0x4183:
			case 0x4192:
			case 0x4193:
			case 0x41B4:
			case 0x41B7:
				UpdatePrgMapping();
				break;

			case 0x42FC: case 0x42FD: case 0x42FE: case 0x42FF:
				//Enter game mode and set the mirroring $4182 can defer to
				_mirroring = (uint8_t)((addr & 1 ? 2 : 0) | (value & 0x10 ? 1 : 0));
				//A game off a disc hands over here, and from this point the machine is a
				//cartridge rather than a learning machine
				_gameLaunched = true;
				//Which shape of cartridge it is, and the window it starts in. Only a hand-over
				//that leaves the machine a cartridge starts a game: the one the disc menu makes
				//on its own way out still has the machine in system mode, and the sizes it would
				//be read with there are the reset values, not the game's.
				if(CartridgeMode()) {
					StartGameMode(value, GamePrgBankCount(), GameChrBankCount());
				} else if(_gameLaunched && !_gameReset &&
					(MachineMode() == MachineMmc1 || MachineMode() == MachineMmc3)) {
					//The other two personalities are controllers rather than window shapes: the
					//game keeps its own mapper writes and the machine imitates the chip. Started
					//once, for the same reason the window shapes are - the startup stub a game
					//brings with it writes here again while it runs.
					_gameReset = true;
					_gameChip = MachineMode();
					if(_gameChip == MachineMmc1) {
						_mmc1.Reset();
					} else {
						_mmc3.Reset();
					}
					ApplyGameChip(false);
				}
				//$4182 says how this selection is used: it can name an arrangement outright, or
				//defer here either in part - bit 4 alone, picking between the two-screen
				//arrangements - or in full, where the ADDRESS bit picks two-screen against
				//one-screen and bit 4 picks which of the pair. 97 of the games on the disc defer
				//in part and 28 in full, and one of those spends the whole game toggling a single
				//page back and forth through here - forcing horizontal or vertical instead drew it
				//as a band repeated down the screen. This has to follow EVERY write: the value that
				//matters arrives on a later one.
				MirrorSync();
				UpdatePrgMapping();
				UpdateChrMapping();
				break;
		}
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SVArray(_regs, 0x40);
		SV(_loadMode); SV(_placeUseB);
		SV(_irqCounter);
		SV(_irqEnabled);
		SV(_lineCounter); SV(_lastIrqScanline); SV(_lineIrqPending); SV(_counterIrqPending);
		SV(_irqStatus);
		SV(_kbdCtrl); SV(_kbdClock); SV(_kbdData); SV(_kbdClockCount);
		SV(_kbdLatch); SV(_kbdParity); SV(_kbdState); SV(_kbdRaiseIrq);
		SVArray(_kbdQueue, 32); SV(_kbdQueueLen); SV(_kbdPollFrame);
		SVArray(_kbdHold, Sb2kKeyboard::KeyCount);
		SV(_fdc);
		SV(_cd);
		SVArray(_exRamNt, 0x800); SV(_extNtAddr); SV(_extFetchCounter); SV(_diskType);
		SV(_ntData); SV(_logoMode); SV(_autoBank); SV(_gameLaunched); SV(_mirroring);
		SV(_gameMode); SVArray(_gamePrgBanks, 4); SV(_gameChrBank); SV(_gameReset);
		SV(_gameChip); SVArray(_gameChrPages, 8); SV(_mmc1); SV(_mmc3);
		SV(_lptData); SV(_lptCtrl); SV(_printer);
		SV(_speechByte); SV(_speechNibbleCount); SV(_speech);
		SV(_mouseEnabled); SV(_mouseFrame); SV(_cdvApuReady);
		SVArray(_mouseReport, 3); SV(_lastMouseButtons);

		if(!s.IsSaving()) {
			UpdatePrgMapping();
			UpdateChrMapping();
		}
	}
};
