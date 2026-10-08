#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/NesConsole.h"
#include "NES/BaseNesPpu.h"
#include "NES/Mappers/Bbk/BbkLpcAudio.h"
#include "Utilities/Serializer.h"

//Subor V7.0 / V7.1 (小霸王) learning cartridges, and the V7.1 with a 1MB add-on above it. Their headers say mapper 168, which in iNES terms
//is the Racermate board; this one is the Subor board the reference emulator also numbers 168, so
//it is picked out by PRG CRC32. Ported from the reference emulator's Mapper168.
//
//$5000 and $5200, both write-only:
// - ROM paging: with $5200 bit 2 set, $5000 picks a 32KB bank; otherwise $5000 picks the 16KB bank
//   at $8000 and the first 16KB stays at $C000. ($5000 bit 7 selects RAM paging on the board
//   variant with a disk drive; this one has no RAM there, and its software never sets it.)
// - $5200 bit 0: horizontal mirroring, otherwise vertical.
// - $5200 bits 1-0 = 2: the background's pattern table follows the nametable as well as $2000 -
//   nametables 1-3 draw from $1000. Nametable 2 draws from $1000 in every mode.
//$5300: the LPC-10 speech chip - the SB-2000's, with the same Subor coefficient set and status
//byte ($80 = can take data, $0F = end of speech).
//
//8KB of work RAM at $6000 and 8KB of CHR RAM. The keyboard and mouse are the Subor cartridges'
//(see SuborCarts).
//
//The Subor karaoke cartridge (NES 2.0 mapper 514) is the same board driven from a latch at
//$8000-$FFFF instead (the reference's Rom_Type 1): bits 0-4 pick the 32KB bank, bit 6 is horizontal
//mirroring, and either of bits 6-7 sends nametables 1-3 to the $1000 patterns.
class Subor168 : public BaseMapper
{
private:
	uint8_t _reg5000 = 0;
	uint8_t _reg5200 = 0;
	uint8_t _reg8000 = 0;
	bool _karaoke = false;

	//The last nametable byte the picture fetched, which picks the background's pattern table
	uint16_t _ntAddr = 0;

	unique_ptr<BbkLpcAudio> _lpcAudio;

	void UpdateState()
	{
		if(_karaoke) {
			SelectPrgPage2x(0, (_reg8000 & 0x1F) * 2);
			SetMirroringType((_reg8000 & 0x40) ? MirroringType::Horizontal : MirroringType::Vertical);
			return;
		}
		if(_reg5200 & 0x04) {
			SelectPrgPage2x(0, (_reg5000 & 0x7F) * 2);
		} else {
			SelectPrgPage(0, _reg5000 & 0x7F);
			SelectPrgPage(1, 0);
		}
		SetMirroringType((_reg5200 & 0x01) ? MirroringType::Horizontal : MirroringType::Vertical);
	}

	bool PatternsFollowNametable()
	{
		return _karaoke ? (_reg8000 & 0xC0) != 0 : (_reg5200 & 0x03) == 0x02;
	}

	bool IsBackgroundFetch()
	{
		uint32_t cycle = _console->GetPpu()->GetCurrentCycle();
		return cycle <= 256 || cycle >= 321;
	}

protected:
	uint16_t GetPrgPageSize() override { return 0x4000; }
	uint16_t GetChrPageSize() override { return 0x2000; }
	uint32_t GetChrRamSize() override { return 0x2000; }
	uint32_t GetWorkRamSize() override { return 0x2000; }
	uint32_t GetSaveRamSize() override { return 0; }

	uint16_t RegisterStartAddress() override { return 0x5000; }
	uint16_t RegisterEndAddress() override { return 0x5FFF; }
	bool AllowRegisterRead() override { return true; }

	//The karaoke cartridge's song start needs one more line (see SuborCarts::HasLongDendyFrame)
	int32_t GetDendyScanlineCount() override { return _karaoke ? 313 : 312; }
	int32_t GetDendyNmiScanline() override { return _karaoke ? 292 : 291; }
	bool EnableCustomVramRead() override { return true; }
	bool EnableCpuClockHook() override { return true; }

	void InitMapper(RomData& romData) override
	{
		romData.Info.System = GameSystem::Dendy;
	}

	void InitMapper() override
	{
		_romInfo.System = GameSystem::Dendy;
		_lpcAudio.reset(new BbkLpcAudio(_console, BbkLpcAudio::LpcVariant::Sb2k));
		_lpcAudio->Reset();

		_karaoke = IsKaraoke(_romInfo.Hash.PrgCrc32);
		if(_karaoke) {
			AddRegisterRange(0x8000, 0xFFFF, MemoryOperation::Write);
		}

		SetCpuMemoryMapping(0x6000, 0x7FFF, 0, PrgMemoryType::WorkRam);
		SelectChrPage(0, 0);
		UpdateState();
	}

	void Reset(bool softReset) override
	{
		BaseMapper::Reset(softReset);
		_reg5000 = 0;
		_reg5200 = 0;
		_reg8000 = 0;
		_lpcAudio->Reset();
		UpdateState();
	}

	void ProcessCpuClock() override
	{
		BaseProcessCpuClock();
		_lpcAudio->Clock();
	}

	uint8_t MapperReadVram(uint16_t addr, MemoryOperationType type) override
	{
		if(type == MemoryOperationType::PpuRenderingRead) {
			if(addr >= 0x2000) {
				if((addr & 0x3FF) < 0x3C0) {
					_ntAddr = addr;
				}
			} else if(IsBackgroundFetch()) {
				uint8_t nametable = (_ntAddr >> 10) & 0x03;
				if(nametable == 2 || (nametable != 0 && PatternsFollowNametable())) {
					addr |= 0x1000;
				}
			}
		}
		return InternalReadVram(addr);
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		if((addr & 0xFF00) == 0x5300) {
			return (_lpcAudio->IsBusy() ? 0x00 : 0x80) | (_lpcAudio->IsSpeechEnd() ? 0x0F : 0x00);
		}
		return _console->GetMemoryManager()->GetOpenBus();
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr >= 0x8000) {
			_reg8000 = value;
			UpdateState();
			return;
		}
		switch(addr & 0xFF00) {
			case 0x5000: _reg5000 = value; UpdateState(); break;
			case 0x5200: _reg5200 = value; UpdateState(); break;
			case 0x5300: _lpcAudio->WriteData(value); break;
		}
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SV(_reg5000);
		SV(_reg5200);
		SV(_reg8000);
		SV(_ntAddr);
		SV(_lpcAudio);
		if(!s.IsSaving()) {
			UpdateState();
		}
	}

public:
	static bool IsKaraoke(uint32_t prgCrc)
	{
		return prgCrc == 0x0A9808AE;
	}

	static bool IsSubor168(uint32_t prgCrc)
	{
		return prgCrc == 0x04260DBC || prgCrc == 0x79C85E71 || prgCrc == 0xD3113B3F || IsKaraoke(prgCrc);
	}
};
