#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/BaseNesPpu.h"
#include "NES/NesConsole.h"
#include "NES/NesControlManager.h"
#include "NES/NesCpu.h"
#include "NES/NesMemoryManager.h"
#include "NES/Input/PecKeyMatrix.h"
#include "NES/Input/ZechengKeyMatrix.h"
#include "Shared/MessageManager.h"
#include "Utilities/ArchiveReader.h"
#include "Utilities/CRC32.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/Serializer.h"

//Dongda (东达) PEC-586 family - the "Pyramid" (金字塔) home computers: PEC-586, PEC-686F and
//PEC-9588, 512KB of ROM holding their programming languages and games. NES 2.0 mapper 257
//(UNIF PEC-586).
//The files in circulation call it mapper 179, which is a different board.
//
//Registers: $5000-$5FFF, decoded by A8-A10 only. $5000 is the one that matters:
// - bit 4: a 32KB bank, bits 2-0, of the lower 256KB.
// - PEC-586/686F, bit 4 clear: $8000-$FFFF shows the upper 256KB through a fixed address
//   swap - each 1KB of the window is the last 1KB of one 8KB block - which is where the
//   reset code lives. Bit 6 then puts an 8KB bank, (bits 3-0) + $20 (+ $10 for bit 5), at
//   $8000.
// - PEC-9588, bit 4 clear: a 16KB bank at $8000, bits 3-0 (+ $10 for bit 5), and the last
//   16KB at $C000 - or, with bit 6 set, the one before it, and $1C when bits 3-2 are set too.
//   The reference emulator tests the whole byte for that last case (>= $6C), but the BIOS
//   only ever writes $6C itself, while the built-in turtle graphics write $EB - $6B plus the
//   bitmap bit - and run on in $C000-$FFFF. It boots like an Apple II, from the top of the ROM.
// - bits 4-3 both set: horizontal mirroring, otherwise vertical.
// - bit 7: the bitmap mode below.
//$5300 (PEC-586/686F) and $5500 (PEC-9588) bit 2 is the cassette input, $5100 bit 0 its
//output and the printer strobe, $5200 the printer data; none of them are connected here.
//Reads of the block return a few bits picked by the last $54xx write, as FCEUX has them.
//The PEC-9588's floppy drive is an Apple II Disk II controller at $5600 + slot*16 (the BIOS uses
//slot 1, $5610-$561F). No drive is emulated, but the data latch ($56xC) reads like an empty
//drive's: while the motor runs ($56x9 on, $56x8 off) the read amplifier picks up noise, so bytes
//keep arriving and the disk code runs out its byte count looking for an address field and reports
//an error, instead of waiting forever. With the motor off the latch holds still, which is how the
//same code tells that a drive has stopped spinning.
//
//Bitmap mode: the background is 1 bit per pixel. A tile in an even nametable column takes its
//first plane, one in an odd column its second, for both colour bits; and the pattern table
//follows the nametable row instead of $2000 - the upper 16 rows use $0000, the rest $1000.
//So 8KB of CHR RAM covers the whole screen with 512 private tiles a half. Sprites are colored
//two entries further into their palette, as the reference emulator has it: the menus' blinking
//cursor is drawn in color 1 of a palette whose only visible entry is color 3.
//
//Keyboard: the PEC-586 key matrix (see PecKeyMatrix) on $4016/$4017. A key that is down reads
//as 1 in bits 1, 4 and 6 of $4017: the PEC-9388 takes bit 6, which would otherwise be open bus
//(always 1, every key held); the other machines' code shifts or masks bit 6 away.
//
//Slot cards (PEC-9388, measured on one): the menu's slot 2/3 entries leave "SELECTSLOTTWO" or
//"SELECTSLOTTHREE" in the nametable and restart, and the boot code reads it back and switches with
//$5000 = $70, $5400 = $70, $5000 = $30 (slot 2) or $50 (slot 3). With bit 4 set, bit 5 picks slot 2
//and bit 6 slot 3; what goes to $5400 made no difference on the machine, and the cards' own code
//switches banks with plain $5000 writes. The card then has $8000-$FFFF, a 32KB bank from bits 2-0;
//an empty slot reads as floating bus. A plain card (one 27C512) wires bit 0 alone, so the bank
//wraps at the card's size; a 128KB card with its own decoder (a 24-pin chip, markings ground off)
//shows $33 as $32, so a bank past the end of a file that is not a power of two reads as the last
//one. The cards are files next to the ROM, <rom>.slot2.bin and <rom>.slot3.bin, or in the ROM's
//archive, any other file there whose name starts with "slot2" or "slot3" (one archive per set of
//cards; a card's extension should not be a ROM type, or opening the archive asks which to load).
//What the boot code does next is to read a 16-byte header through the card's $FFFE vector, copy a
//page of start-up code into RAM and the title characters into the pattern table, and jump to the
//code. The only plaintext of it is the PEC-586's cracked copy, whose decoding steps are NOPs: the
//layout of the header and the copies' XORs are worked out (see BootCard), but not how the vector
//and the header themselves are decoded. So the menu's switch from the machine's own ROM to a card
//($30 or $50) does the loader's work for the two known cards, whose headers were found by search,
//and starts any other card from its reset vector, as long as that points into the card.
//
//Two 512KB learning cartridges (BIT Corp. 1989 inside) sit on this board too, with circulating
//dumps that say mapper 170 - the number the reference emulator gave this board, and a different
//one in MesenCE - so they are known by their PRG CRC32 (IsZechengCart). They use none of the
//computers' banking: $5000 bits 3-0 pick a 32KB bank of the whole ROM. The mirroring and bitmap
//bits work as above. Their keyboard is the ZeCheng type (see ZechengKeyMatrix), and $5500 reads
//a DIP setting in bits 5-4 (bit 7 is the cassette input, bit 6 unused); the reference emulator
//steps it on every reset through four values it names after brands, so 0 is used here. They run
//on Dendy timing.
//
//A third learning cartridge with the same keyboard (BIT Corp. 1989 inside as well) comes as mapper
//170, but banks as the PEC-586/686F do: its reset code is in the upper 256KB's swapped window.
class DongdaPec586 : public BaseMapper
{
public:
	static bool IsZechengCart(uint32_t prgCrc)
	{
		return prgCrc == 0x836CDDEF || prgCrc == 0x428C1C1D;
	}

	static bool HasZechengKeyboard(uint32_t prgCrc)
	{
		return IsZechengCart(prgCrc) || prgCrc == 0x44479A27;
	}

private:
	static constexpr uint32_t Crc9588 = 0xFE31765B;
	static constexpr uint8_t ZechengDip = 0x00;

	bool _zecheng = false;
	bool _zechengKeyboard = false;
	bool _kbdArmed = false;

	//What reads of $5000-$5FFF carry, by the last $54xx write's high nibble
	static constexpr uint8_t ReadBits[16] = { 0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 };

	bool _is9588 = false;
	uint8_t _regs[8] = {};

	//Noise an empty floppy drive's data latch reads back (PEC-9588), from a 16-bit LFSR, and
	//whether the drive motor is on
	uint16_t _diskNoise = 0xACE1;
	bool _diskMotorOn = false;

	//Any access to a Disk II soft switch flips it, read or write
	void AccessDiskSwitch(uint16_t addr)
	{
		switch(addr & 0x0F) {
			case 0x08: _diskMotorOn = false; break;
			case 0x09: _diskMotorOn = true; break;
		}
	}

	//The last nametable byte the picture fetched, for the bitmap mode
	uint16_t _ntAddr = 0;

	uint8_t _kbdRow = 0;
	uint8_t _kbdColumn = 0;
	uint8_t _kbdLast = 0;

	//The slot cards' ROMs, slots 2 and 3, looked for the first time a slot is picked or a state is
	//saved or loaded (the ROM's path is not known yet while the mapper initializes)
	vector<uint8_t> _cards[2];
	uint32_t _cardCrcs[2] = {};
	bool _cardsChecked = false;

	//Where each known card keeps its loader header, and the byte it is XORed with (by the CRC32 of
	//the card's first 32KB): found by searching for the fields both cards were seen to need, since
	//the code that finds them is still unknown
	struct CardHeader { uint32_t BankCrc; uint16_t Address; uint8_t Key; };
	static constexpr CardHeader CardHeaders[] = {
		{ 0x2247B8FA, 0xFD1E, 0xED }, //64KB card, one 27C512
		{ 0x165E9303, 0xB200, 0x70 }, //128KB card, 27C010 and decoder
	};

	//The card in $8000-$FFFF, if any: its slot (2 or 3, 0 = none) and bank offset
	uint8_t _cardSlot = 0;
	uint32_t _cardOffset = 0;

	NesControlManager* NesControls() { return (NesControlManager*)_console->GetControlManager(); }

	//The card for a slot from the ROM's archive: the first other file whose name starts with the slot
	static bool ReadArchiveCard(ArchiveReader& reader, const string& romName, int slot, vector<uint8_t>& data, string& name)
	{
		string tag = "slot" + std::to_string(slot);
		for(string& file : reader.GetFileList()) {
			string lcName = FolderUtilities::GetFilename(file, true).substr(0, tag.size());
			std::transform(lcName.begin(), lcName.end(), lcName.begin(), [](unsigned char c) { return (char)std::tolower(c); });
			if(file != romName && lcName == tag) {
				name = file;
				return reader.ExtractFile(file, data);
			}
		}
		return false;
	}

	void LoadCards()
	{
		_cardsChecked = true;
		VirtualFile& romFile = _emu->GetRomInfo().RomFile;
		string romPath = romFile.GetFilePath();
		unique_ptr<ArchiveReader> reader = romFile.IsArchive() ? ArchiveReader::GetReader(romPath) : nullptr;
		string folder = FolderUtilities::GetFolderName(romPath);
		string baseName = FolderUtilities::GetFilename(romPath, false);
		for(int i = 0; i < 2; i++) {
			vector<uint8_t> data;
			string path;
			if(reader) {
				if(!ReadArchiveCard(*reader, romFile.GetFileName(), i + 2, data, path)) {
					continue;
				}
			} else {
				path = FolderUtilities::CombinePath(folder, baseName + ".slot" + std::to_string(i + 2) + ".bin");
				ifstream file(path, ios::in | ios::binary);
				if(!file) {
					continue;
				}
				data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			}
			if(data.size() < 0x8000) {
				MessageManager::Log("[PEC] Slot card smaller than 32KB, ignored: " + path);
				continue;
			}
			data.resize((data.size() + 0x7FFF) & ~0x7FFF, 0xFF);
			_cards[i] = std::move(data);
			_cardCrcs[i] = CRC32::GetCRC(_cards[i]);
			MessageManager::Log("[PEC] Slot " + std::to_string(i + 2) + " card: " + path);
		}
	}

	//Gives $8000-$FFFF to a slot card when the $5000 value picks one
	bool MapCard(uint8_t reg)
	{
		uint8_t slot = (reg >> 5) & 0x03;
		if(!(reg & 0x10) || (slot != 1 && slot != 2)) {
			return false;
		}
		if(!_cardsChecked) {
			LoadCards();
		}
		if(_cards[slot - 1].empty()) {
			return false;
		}

		vector<uint8_t>& card = _cards[slot - 1];
		uint32_t banks = (uint32_t)card.size() / 0x8000;
		uint32_t bank = reg & 0x07;
		bank = (banks & (banks - 1)) == 0 ? (bank & (banks - 1)) : std::min(bank, banks - 1);
		SetCpuMemoryMapping(0x8000, 0xFFFF, card.data(), bank * 0x8000, (uint32_t)card.size(), MemoryAccessType::Read);
		_cardSlot = slot + 1;
		_cardOffset = bank * 0x8000;
		return true;
	}

	//The stand-in for the boot code's card loader (see the header), called as the menu switches to
	//the card: the header's copies, then on to its entry
	void BootCard()
	{
		vector<uint8_t>& card = _cards[_cardSlot - 2];
		uint8_t* bank = card.data() + _cardOffset;
		auto read = [bank](uint16_t addr) { return bank[addr & 0x7FFF]; };
		NesMemoryManager* memory = _console->GetMemoryManager();

		//The card's $5000 value at $04FF, as the loader leaves it: the cards OR it with 1 to reach
		//their second bank
		memory->DebugWrite(0x04FF, _regs[0]);

		uint32_t crc = CRC32::GetCRC(bank, 0x8000);
		for(const CardHeader& known : CardHeaders) {
			if(known.BankCrc != crc) {
				continue;
			}

			//16 bytes: partner block, RAM destination (= entry), source, pages, PPU address
			//(high byte first), pattern source, pages, and byte 15 = 0. Every copied byte is XORed
			//with the partner block's byte at the same distance, the pattern bytes with $AF as well
			//(the PEC-586's cracked loader has NOPs in those places, and INCs the partner pointers)
			uint8_t h[16];
			for(int i = 0; i < 16; i++) {
				h[i] = read(known.Address + i) ^ known.Key;
			}
			uint16_t partner = h[0] | (h[1] << 8);
			uint16_t dest = h[2] | (h[3] << 8);
			uint16_t src = h[4] | (h[5] << 8);
			for(uint32_t i = 0; i < h[6] * 0x100u; i++) {
				memory->DebugWrite((uint16_t)(dest + i), read(src + i) ^ read(partner + i));
			}
			uint16_t ppuAddr = (h[7] << 8) | h[8];
			uint16_t chrSrc = h[9] | (h[10] << 8);
			for(uint32_t i = 0; i < h[11] * 0x100u; i++) {
				_chrRam[(ppuAddr + i) & 0x1FFF] = read(chrSrc + i) ^ read(partner + i) ^ 0xAF;
			}

			NesCpu* cpu = _console->GetCpu();
			cpu->GetState().PS |= PSFlags::Interrupt;
			cpu->GetState().PC = dest;
			MessageManager::Log("[PEC] Slot " + std::to_string(_cardSlot) + " card loaded");
			return;
		}

		//An unknown card: straight to its reset vector, if that is in the card
		uint16_t entry = read(0xFFFC) | (read(0xFFFD) << 8);
		if(entry >= 0x8000) {
			_console->GetCpu()->GetState().PC = entry;
			MessageManager::Log("[PEC] Slot " + std::to_string(_cardSlot) + " card started at its reset vector");
		}
	}

	void MapPrg16k(uint16_t start, uint32_t bank)
	{
		SetCpuMemoryMapping(start, start + 0x3FFF, PrgMemoryType::PrgRom, (bank * 0x4000) & (_prgSize - 1), MemoryAccessType::Read);
	}

	void UpdateState()
	{
		uint8_t reg = _regs[0];
		_cardSlot = 0;
		if(_zecheng) {
			SetCpuMemoryMapping(0x8000, 0xFFFF, PrgMemoryType::PrgRom, ((reg & 0x0F) * 0x8000) & (_prgSize - 1), MemoryAccessType::Read);
		} else if(MapCard(reg)) {
			//The slot card has the whole window
		} else if(reg & 0x10) {
			SetCpuMemoryMapping(0x8000, 0xFFFF, PrgMemoryType::PrgRom, ((reg & 0x07) * 0x8000) & (_prgSize - 1), MemoryAccessType::Read);
		} else if(_is9588) {
			uint8_t bank = (reg & 0x0F) | ((reg & 0x20) >> 1);
			MapPrg16k(0x8000, bank);
			MapPrg16k(0xC000, !(reg & 0x40) ? 0x1F : ((reg & 0x0C) == 0x0C ? 0x1C : 0x1E));
		} else {
			for(uint32_t i = 0; i < 32; i++) {
				uint32_t offset = ((0x107 | (i << 3)) << 10) & (_prgSize - 1);
				SetCpuMemoryMapping(0x8000 + i * 0x400, 0x83FF + i * 0x400, PrgMemoryType::PrgRom, offset, MemoryAccessType::Read);
			}
			if(reg & 0x40) {
				uint32_t bank = (reg & 0x0F) | 0x20 | ((reg & 0x20) >> 1);
				SetCpuMemoryMapping(0x8000, 0x9FFF, PrgMemoryType::PrgRom, (bank * 0x2000) & (_prgSize - 1), MemoryAccessType::Read);
			}
		}

		SetMirroringType((reg & 0x18) == 0x18 ? MirroringType::Horizontal : MirroringType::Vertical);
		_console->GetPpu()->SetSpriteColorShift(reg & 0x80);
	}

	bool IsBackgroundFetch()
	{
		uint32_t cycle = _console->GetPpu()->GetCurrentCycle();
		return cycle <= 256 || cycle >= 321;
	}

protected:
	uint16_t GetPrgPageSize() override { return 0x2000; }
	uint16_t GetChrPageSize() override { return 0x2000; }
	uint32_t GetChrRamSize() override { return 0x2000; }
	uint32_t GetWorkRamSize() override { return 0x2000; }
	uint32_t GetSaveRamSize() override { return 0; }

	uint16_t RegisterStartAddress() override { return 0x5000; }
	uint16_t RegisterEndAddress() override { return 0x5FFF; }
	bool AllowRegisterRead() override { return true; }
	bool EnableCustomVramRead() override { return true; }

	//No 2C02 "read one dot before vblank" race, as with the other learning machines' clone PPUs.
	//One of the PEC-9588's programs polls $2002 for vblank before every character it prints,
	//with NMIs on, and keeps losing the flag to that race - its banner then takes seconds a letter.
	bool EnablePpuNmiSuppressRace() override { return false; }

	void GetMemoryRanges(MemoryRanges& ranges) override
	{
		BaseMapper::GetMemoryRanges(ranges);
		ranges.AddHandler(MemoryOperation::Write, 0x4016);
		ranges.AddHandler(MemoryOperation::Read, 0x4017);
		ranges.SetAllowOverride();
	}

	void InitMapper() override
	{
		_is9588 = _romInfo.Hash.PrgCrc32 == Crc9588;
		_zecheng = IsZechengCart(_romInfo.Hash.PrgCrc32);
		_zechengKeyboard = HasZechengKeyboard(_romInfo.Hash.PrgCrc32);

		AddRegisterRange(0x4016, 0x4016, MemoryOperation::Write);
		AddRegisterRange(0x4017, 0x4017, MemoryOperation::Read);

		SetCpuMemoryMapping(0x6000, 0x7FFF, 0, PrgMemoryType::WorkRam);
		SelectChrPage(0, 0);
		UpdateState();
	}

	void Reset(bool softReset) override
	{
		BaseMapper::Reset(softReset);
		//RESET leaves a slot card in: the cards load, show their title and wait, and the button
		//then starts them from their own reset vector (the 128KB card's points into the RAM its
		//loader filled); their way back to the machine is $5000 = 0 and JMP ($FFFC)
		uint8_t keepCard = softReset && _cardSlot != 0 && !_is9588 ? _regs[0] : 0;
		memset(_regs, 0, sizeof(_regs));
		_regs[0] = keepCard;
		_kbdRow = 0;
		_kbdColumn = 0;
		_kbdLast = 0;
		_kbdArmed = false;
		UpdateState();
	}

	uint8_t MapperReadVram(uint16_t addr, MemoryOperationType type) override
	{
		if(type == MemoryOperationType::PpuRenderingRead) {
			if(addr >= 0x2000) {
				if((addr & 0x3FF) < 0x3C0) {
					_ntAddr = addr;
				}
			} else if((_regs[0] & 0x80) && IsBackgroundFetch()) {
				addr = ((_ntAddr & 0x200) << 3) | ((_ntAddr & 0x01) << 3) | (addr & 0x0FF7);
			}
		}
		return InternalReadVram(addr);
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		if(_is9588 && (addr & 0xFF00) == 0x5600) {
			AccessDiskSwitch(addr);
			if((addr & 0x0F) == 0x0C) {
				//Disk II data latch with no disk: while the motor runs, a fresh noise byte every
				//read with bit 7 set like any byte the shift register has finished assembling
				if(_diskMotorOn) {
					for(int i = 0; i < 8; i++) {
						_diskNoise = (_diskNoise >> 1) ^ (-(int16_t)(_diskNoise & 1) & 0xB400);
					}
				}
				return (uint8_t)_diskNoise | 0x80;
			}
		}
		if(addr == 0x4017) {
			Sb2kKeyboard* kbd = NesControls()->GetControlDevice<Sb2kKeyboard>().get();
			if(_zechengKeyboard) {
				return (NesControls()->ReadRam(addr) & ~0x02) | ZechengKeyMatrix::Read(kbd, _kbdRow, _kbdColumn);
			}
			//The PEC-9388 reads its keys in bit 6 (ASL/ASL), the others in bits 1 and 4
			uint8_t key = PecKeyMatrix::Read(kbd, _kbdRow, _kbdColumn) ? 0x52 : 0;
			return (NesControls()->ReadRam(addr) & ~0x52) | key;
		}
		if(_zechengKeyboard && addr == 0x5500) {
			return ZechengDip << 4;
		}
		return (_console->GetMemoryManager()->GetOpenBus() & 0xD8) | ReadBits[_regs[4] >> 4];
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr == 0x4016) {
			NesControls()->WriteRam(addr, value);
			if(_zechengKeyboard) {
				ZechengKeyMatrix::Write(value, _kbdRow, _kbdColumn, _kbdArmed);
			} else {
				PecKeyMatrix::Write(value, _kbdLast, _kbdRow, _kbdColumn);
			}
			_kbdLast = value;
			return;
		}

		if(_is9588 && (addr & 0xFF00) == 0x5600) {
			AccessDiskSwitch(addr);
		}

		bool fromRom = _cardSlot == 0;
		_regs[(addr >> 8) & 0x07] = value;
		UpdateState();
		if(fromRom && _cardSlot != 0 && (value & 0x8F) == 0 && !_is9588) {
			//Only the menu's slot entries ($30, $50): its BASIC launchers also switch to slot 3
			//($56, $58, $D6) and then do their own copying and jumping. The PEC-9588 picks its
			//expansion slots 2/3 with the same values but needs no loader: its menu itself checks
			//for $16 at the card's $FFF9 and jumps through the card's reset vector (or restarts the
			//machine)
			BootCard();
		}
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SVArray(_regs, 8);
		SV(_ntAddr);
		SV(_diskNoise);
		SV(_diskMotorOn);
		SV(_kbdRow);
		SV(_kbdColumn);
		SV(_kbdLast);
		SV(_kbdArmed);

		//Which cards the state was made with (0 = no card, or a state from before this was kept)
		if(!_cardsChecked) {
			LoadCards();
		}
		uint32_t cardCrcs[2] = { _cardCrcs[0], _cardCrcs[1] };
		if(!s.IsSaving()) {
			cardCrcs[0] = cardCrcs[1] = 0;
		}
		SVArray(cardCrcs, 2);
		if(!s.IsSaving()) {
			for(int i = 0; i < 2; i++) {
				if(cardCrcs[i] != 0 && cardCrcs[i] != _cardCrcs[i]) {
					MessageManager::DisplayMessage("PEC", "Save state made with a different card in slot " + std::to_string(i + 2));
				}
			}
			UpdateState();
		}
	}
};
