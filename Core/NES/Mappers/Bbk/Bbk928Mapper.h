#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "Utilities/Serializer.h"

//BBK (步步高) 928 learning machine - mapper 171, variant 3 in the header byte 9 convention
//this branch uses to tell the learning machines apart (see MapperFactory).
//
//Dumps of this machine in circulation are tagged mapper 186, which is Study Box; they are
//retagged rather than squatting on that number. Do not identify one of these by its header -
//follow the reset vector at $FFFC and look for stores to $FFE0-$FFE3.
//
//The memory controller is the whole of the board as far as software can see it: four 8KB
//windows with one write-only bank register each, sitting at the very top of the address space.
//
// - $FFE0 -> $8000-$9FFF
// - $FFE1 -> $A000-$BFFF
// - $FFE2 -> $C000-$DFFF
// - $FFE3 -> $E000-$FFFF
//
//Putting the registers inside the last bank is not as awkward as it looks: the ROM still reads
//through at those addresses, so the bytes there are ordinary code. The reset vector points at
//$FFEA, which is "LDA #$7E / STA $FFE2" - page the kernel into $C000 and jump into it, the same
//shape the other machines in this family boot with.
//
//$FFE4 is written by the BIOS; its meaning is not known, and it is ignored here.
//
//No CHR ROM, so the machine has CHR-RAM and blits into it. Mirroring comes from the header.
class Bbk928Mapper : public BaseMapper
{
private:
	uint8_t _banks[4] = {};

	void UpdatePrgMapping()
	{
		for(int i = 0; i < 4; i++) {
			SelectPrgPage(i, _banks[i]);
		}
	}

protected:
	uint16_t GetPrgPageSize() override { return 0x2000; }
	uint16_t GetChrPageSize() override { return 0x2000; }
	uint16_t GetChrRamPageSize() override { return 0x2000; }
	uint32_t GetChrRamSize() override { return 0x2000; }
	uint32_t GetWorkRamSize() override { return 0x2000; }
	uint32_t GetSaveRamSize() override { return 0; }

	uint16_t RegisterStartAddress() override { return 0xFFE0; }
	uint16_t RegisterEndAddress() override { return 0xFFE4; }

	void InitMapper(RomData& romData) override
	{
		//Chinese famiclone hardware, timed like its siblings
		romData.Info.System = GameSystem::Dendy;
	}

	void InitMapper() override
	{
		_romInfo.System = GameSystem::Dendy;

		//The last bank has to be in the $E000 window before any code runs - that is where the
		//CPU fetches its reset vector from. The rest are set by that bank's own startup path.
		_banks[0] = 0;
		_banks[1] = 0;
		_banks[2] = 0;
		_banks[3] = (uint8_t)(GetPrgPageCount() - 1);
		UpdatePrgMapping();

		SelectChrPage(0, 0);

		memset(_chrRam, 0, _chrRamSize);
		memset(_workRam, 0, _workRamSize);
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr <= 0xFFE3) {
			_banks[addr & 0x03] = value;
			SelectPrgPage(addr & 0x03, value);
		}
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SVArray(_banks, 4);

		if(!s.IsSaving()) {
			UpdatePrgMapping();
		}
	}
};
