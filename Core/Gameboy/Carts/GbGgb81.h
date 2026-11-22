#pragma once
#include "pch.h"
#include "Gameboy/Carts/GbCart.h"
#include "Gameboy/GbMemoryManager.h"
#include "Utilities/Serializer.h"

//GGB-81: Vast Fame's later releases (DSHGGB-81 and BC-R1616T3P boards), a variant of the
//BBD/Hitek protection on an MBC5, used by several of their games.
//A write to $2001 (A15-A12 = 2, A7-A0 = $01) picks one of 8 orders the data lines of
//$4000-$7FFF are read in; the ROM holds each bank in its own order, so the games write the
//bank's low 3 bits there on every switch. The write also reaches the MBC5 bank register, as
//any $2xxx write does. The orders are hhugboy's (ggb81DataReordering).
//Not emulated: the DSHGGB-81's logo switch - during the boot ROM it shows the cartridge's own
//logo at $0184 in place of Nintendo's, a cosmetic difference.
class GbGgb81 : public GbCart
{
private:
	static constexpr uint8_t _reorder[8][8] = {
		{ 0, 1, 2, 3, 4, 5, 6, 7 },
		{ 0, 2, 1, 3, 4, 6, 5, 7 },
		{ 0, 6, 5, 3, 4, 2, 1, 7 },
		{ 0, 1, 5, 3, 4, 6, 2, 7 },
		{ 0, 1, 6, 3, 4, 5, 2, 7 },
		{ 0, 6, 2, 3, 4, 1, 5, 7 },
		{ 0, 2, 5, 3, 4, 1, 6, 7 },
		{ 0, 6, 1, 3, 4, 2, 5, 7 },
	};

	uint8_t _lut[8][256] = {};
	uint8_t* _prgRom = nullptr;
	uint32_t _prgBankCount = 1;

	bool _ramEnabled = false;
	uint16_t _prgBank = 1;
	uint8_t _ramBank = 0;
	uint8_t _swapMode = 0;

	static uint8_t Reorder(uint8_t value, const uint8_t* order)
	{
		//Bit 7-x of the result is bit 7-order[x] of the ROM byte
		uint8_t result = 0;
		for(int x = 0; x < 8; x++) {
			result |= ((value >> (7 - order[x])) & 0x01) << (7 - x);
		}
		return result;
	}

public:
	GbGgb81()
	{
		for(int mode = 0; mode < 8; mode++) {
			for(int value = 0; value < 256; value++) {
				_lut[mode][value] = Reorder((uint8_t)value, _reorder[mode]);
			}
		}
	}

	static bool IsGgb81Cart(vector<uint8_t>& prgRom)
	{
		if(prgRom.size() < 0xC000) {
			return false;
		}

		//The second logo at $0184 these boards carry (hhugboy's "DIGI." logo hash)
		int logoSum = 0;
		for(int i = 0; i < 0x30; i++) {
			logoSum += prgRom[0x184 + i];
		}
		if(logoSum != 2692) {
			return false;
		}

		//Vast Fame's games start every bank with its own number: bank 2 reads 02 only
		//unscrambled, i.e. in a fixed or re-released copy that runs as a plain MBC5
		return prgRom[0x8000] != 0x02;
	}

	uint32_t GetCartRamSize(uint32_t headerSize) override
	{
		//The games declare 2 KB but need 8 KB; the boards seen so far carry 32 KB
		return std::max<uint32_t>(headerSize, 0x8000);
	}

	void InitCart() override
	{
		_prgRom = _gameboy->DebugGetMemory(MemoryType::GbPrgRom);
		_prgBankCount = std::max<uint32_t>(1, _gameboy->DebugGetMemorySize(MemoryType::GbPrgRom) / 0x4000);
		_memoryManager->MapRegisters(0x0000, 0x5FFF, RegisterAccess::Write);
	}

	void RefreshMappings() override
	{
		constexpr int prgBankSize = 0x4000;
		constexpr int ramBankSize = 0x2000;

		Map(0x0000, 0x3FFF, GbMemoryType::PrgRom, 0, true);
		Map(0x4000, 0x7FFF, GbMemoryType::PrgRom, _prgBank * prgBankSize, true);
		//Reordered reads go through ReadRegister, the plain order straight to the ROM
		_memoryManager->MapRegisters(0x4000, 0x5FFF, _swapMode ? RegisterAccess::ReadWrite : RegisterAccess::Write);
		_memoryManager->MapRegisters(0x6000, 0x7FFF, _swapMode ? RegisterAccess::Read : RegisterAccess::None);

		if(_ramEnabled) {
			Map(0xA000, 0xBFFF, GbMemoryType::CartRam, _ramBank * ramBankSize, false);
			_memoryManager->MapRegisters(0xA000, 0xBFFF, RegisterAccess::None);
		} else {
			Unmap(0xA000, 0xBFFF);
			_memoryManager->MapRegisters(0xA000, 0xBFFF, RegisterAccess::Read);
		}
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		if(addr >= 0x4000 && addr <= 0x7FFF) {
			uint32_t offset = (_prgBank % _prgBankCount) * 0x4000 + (addr & 0x3FFF);
			return _lut[_swapMode][_prgRom[offset]];
		}
		//Disabled RAM returns 0xFF on reads
		return 0xFF;
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if((addr & 0xF0FF) == 0x2001) {
			_swapMode = value & 0x07;
		}

		switch(addr & 0x7000) {
			case 0x0000:
			case 0x1000:
				_ramEnabled = (value == 0x0A);
				break;

			case 0x2000:
				_prgBank = (value & 0xFF) | (_prgBank & 0x100);
				break;

			case 0x3000:
				_prgBank = (_prgBank & 0xFF) | ((value & 0x01) << 8);
				break;

			case 0x4000:
			case 0x5000:
				_ramBank = value & 0x0F;
				break;
		}
		RefreshMappings();
	}

	void Serialize(Serializer& s) override
	{
		SV(_ramEnabled);
		SV(_prgBank);
		SV(_ramBank);
		SV(_swapMode);
		if(!s.IsSaving()) {
			RefreshMappings();
		}
	}
};
