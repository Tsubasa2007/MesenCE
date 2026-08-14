#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/BaseNesPpu.h"
#include "NES/NesConsole.h"
#include "NES/NesCpu.h"
#include "NES/Input/YuxingKeyboard.h"
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

	//A power cycle recreates the mapper, so the inserted disc is remembered here and
	//re-mounted. Scoped to the ROM path so a different machine doesn't inherit it.
	inline static string _persistedDiscRom;
	inline static string _persistedDiscPath;

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

	YuxingType _type = YuxingType::Unknown;

	//16K PRAM bank mask and 8K CRAM bank mask, both set from the machine revision
	uint8_t _pramMask = 0x3F;
	uint8_t _cramMask = 0x0F;

	//Key matrix row select - 14 bits, written as two halves through $4202/$4203
	uint16_t _keyRowMask = 0;

	//$5002: bit 1 clear = the VCD player's own screen is showing, set = normal machine
	//screen. The V9.2 models power on in VCD mode; every other revision reads it as 2.
	uint8_t _reg5002 = 2;

	uint8_t _reg4800 = 0;
	uint8_t _reg5500 = 0;
	uint8_t _reg5501 = 0;
	//Bank latch written through the $8000-$FFFF window while $5500 bit 2 is set
	uint8_t _reg8000 = 0;

	//True while $5501 bit 7 has handed banking over to the MMC3 clone
	bool _mmc3Mode = false;

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

	void DetectMachineType()
	{
		uint32_t crc = _romInfo.Hash.PrgCrc32;

		//Defaults: the largest configuration, used for any BIOS not in the table below
		_type = YuxingType::Unknown;
		_pramMask = 0x3F; //1MB PRAM
		_cramMask = 0x0F; //128KB CRAM
		_reg5002 = 2;

		switch(crc) {
			case 0xCEAC04C7: //V4.0
				_type = YuxingType::V40;
				_pramMask = 0x01; //32KB
				_cramMask = 0x03; //32KB
				break;

			case 0x3B02AF09: //V5.0
			case 0x871254E8: //V5.0 + WuBi
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
			//The MMC3 clone owns the window in this combination
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
	uint16_t GetPrgPageSize() override { return 0x2000; }
	uint16_t GetChrPageSize() override { return 0x400; }
	uint16_t GetChrRamPageSize() override { return 0x400; }
	uint32_t GetChrRamSize() override { return 0x80000; } //512KB CRAM
	uint32_t GetWorkRamSize() override { return PramSize; }
	uint32_t GetWorkRamPageSize() override { return 0x2000; }
	bool ForceWorkRamSize() override { return true; }
	uint32_t GetSaveRamSize() override { return 0; }

	uint16_t RegisterStartAddress() override { return 0x4018; }
	uint16_t RegisterEndAddress() override { return 0x5FFF; }
	bool AllowRegisterRead() override { return true; }

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
		_reg4800 = 0;
		_reg5500 = 0;
		_reg5501 = 0;
		_reg8000 = 0;
		_mmc3Mode = false;
		_vcdMode = (_type == YuxingType::V92);
		_vcdKeyboardSelected = false;

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
		UpdatePrgMapping();
		MapCram8k(0);
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		if(!_discChecked) {
			CheckForDisc();
		}

		switch(addr) {
			case 0x4016:
			case 0x4017: {
				//While the serial keyboard is selected the pad's own bits are held low
				uint8_t value = _vcdKeyboardSelected && addr == 0x4016 ? 0 : NesControls()->ReadRam(addr);
				uint8_t vcdValue = 0;
				if(IsVcdActive()) {
					_vcd.Read(addr, vcdValue);
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

			case 0x4800:
				_reg4800 = value;
				UpdatePrgMapping();
				break;

			case 0x5500:
				_reg5500 = value;
				UpdatePrgMapping();
				break;

			case 0x5501:
				_reg5501 = value;
				MapCram8k(_reg5501 & _cramMask);
				//Bit 7 hands $8000-$FFFF over to the MMC3 clone
				_mmc3Mode = (value & 0x80) != 0;
				if(!_mmc3Mode) {
					UpdatePrgMapping();
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
			//MMC3 clone banking - not yet implemented
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

		string romPath = _emu->GetRomInfo().RomFile.GetFilePath();

		if(_persistedDiscRom == romPath && !_persistedDiscPath.empty()) {
			if(_vcd.LoadDisc(_persistedDiscPath)) {
				MessageManager::Log("[YuXing] Re-inserted disc: " + _persistedDiscPath);
				return;
			}
		}

		string baseName = FolderUtilities::GetFilename(romPath, false);
		vector<string> folders = { FolderUtilities::GetFolderName(romPath) };
		string configured = GetConfiguredDiscFolder();
		if(!configured.empty() && configured != folders[0]) {
			folders.push_back(configured);
		}

		for(string& folder : folders) {
			for(string ext : { ".bin", ".BIN" }) {
				string discPath = FolderUtilities::CombinePath(folder, baseName + ext);
				if(_vcd.LoadDisc(discPath)) {
					MessageManager::Log("[YuXing] Inserted disc: " + discPath);
					return;
				}
			}
		}
	}

public:
	//Disc swapping - driven by the FDS disk shortcut keys via DiscSwapListener
	vector<string> GetDiskFileList()
	{
		string folder = GetConfiguredDiscFolder();
		if(folder.empty()) {
			folder = FolderUtilities::GetFolderName(_emu->GetRomInfo().RomFile.GetFilePath());
		}
		vector<string> files = FolderUtilities::GetFilesInFolder(folder, { ".bin" }, false);
		std::sort(files.begin(), files.end());
		return files;
	}

	uint32_t GetDiskCount() { return (uint32_t)GetDiskFileList().size(); }

	string GetCurrentDiskFilename() { return _vcd.GetDiscFilename(); }

	//Ejecting also leaves VCD mode, which is how the machine gets to its learning-machine
	//side: the BIOS's own "computer key" prompt is answered over a link the emulated
	//keyboard cannot reach, so the reference emulator escapes with a host key combination.
	void EjectDisc()
	{
		auto lock = _emu->AcquireLock();
		if(_vcd.IsDiscInserted()) {
			MessageManager::DisplayMessage("YuXing", "Disc ejected: " + FolderUtilities::GetFilename(_vcd.GetDiscFilename(), true));
			_vcd.EjectDisc();
		}
		_persistedDiscRom.clear();
		_persistedDiscPath.clear();

		if(_vcdMode) {
			//$5002 bit 1 tells the BIOS to come up as a learning machine instead. A soft
			//reset re-runs its boot code with the new value; the mapper keeps its state
			//across that, so the mode stays off.
			_vcdMode = false;
			_reg5002 = 2;
			MapRom32k(0);
			_emu->GetSystemActionManager()->Reset();
			MessageManager::DisplayMessage("YuXing", "Switched to computer mode");
		}
	}

	void InsertDisc(uint32_t index)
	{
		auto lock = _emu->AcquireLock();
		vector<string> discs = GetDiskFileList();
		if(index < discs.size() && _vcd.LoadDisc(discs[index])) {
			_discChecked = true;
			_persistedDiscRom = _emu->GetRomInfo().RomFile.GetFilePath();
			_persistedDiscPath = discs[index];
			MessageManager::DisplayMessage("YuXing", "Disc inserted: " + FolderUtilities::GetFilename(discs[index], true));
		}
	}

	void InsertNextDisc()
	{
		vector<string> discs = GetDiskFileList();
		if(discs.empty()) {
			return;
		}

		int32_t current = -1;
		for(size_t i = 0; i < discs.size(); i++) {
			if(discs[i] == _vcd.GetDiscFilename()) {
				current = (int32_t)i;
				break;
			}
		}
		InsertDisc((current + 1) % discs.size());
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SV(_keyRowMask); SV(_reg5002); SV(_reg4800); SV(_reg5500); SV(_reg5501);
		SV(_reg8000); SV(_mmc3Mode); SV(_vcdMode); SV(_vcdKeyboardSelected);
		_vcd.Serialize(s);
	}
};
