#pragma once
#include "pch.h"
#include "NES/Mappers/Unlicensed/Henggedianzi177.h"
#include "NES/Mappers/Bbk/BbkLpcAudio.h"
#include "NES/NesConsole.h"
#include "Utilities/Serializer.h"

//A mapper 177 board with an LPC speech chip on $5000-$5002: the English dictionary with speech
//that the BT-686 has as a card (see HuatongBt686), in a cartridge edition. Its program is the
//card's with the hardware accesses changed and nothing else: the bank writes go to $8000 instead
//of the BT-686's $4500, and the speech goes a whole byte at a time to $5002 instead of two
//nibbles to $4600. The phrases are the card's, with the chip and the coefficient set the card's
//too; what reaches the chip is what the BT-686's V1.0 and V2.0 system cards send to theirs, each
//phrase as stored - a header ended by $FC, the frames, then 24 bytes of $FF.
//- $5001: written $0F once at power-on; what it does is not known.
//- $5002: write, a byte for the chip; read, bit 7 set when the chip takes another. The program
//  waits for it before every byte - after the title, before it ever scans the keyboard, so on
//  the plain board the title is as far as it gets.
//The board is not named in the iNES header, so it is known by its PRG CRC32. The keyboard is the
//one the header names (Subor's).
class Henggedianzi177Lpc : public Henggedianzi177
{
private:
	unique_ptr<BbkLpcAudio> _lpcAudio;
	bool _speechInHeader = true;
	uint8_t _speechStops = 0;

public:
	static bool IsHenggedianzi177Lpc(uint32_t prgCrc)
	{
		return prgCrc == 0xBA335F4A;
	}

protected:
	bool AllowRegisterRead() override { return true; }
	bool EnableCpuClockHook() override { return true; }

	void InitMapper(RomData& romData) override
	{
		//Chinese famiclone hardware, timed like its siblings - and like the BT-686 the program was
		//written for
		romData.Info.System = GameSystem::Dendy;
	}

	void InitMapper() override
	{
		_romInfo.System = GameSystem::Dendy;
		Henggedianzi177::InitMapper();
		AddRegisterRange(0x5000, 0x5002, MemoryOperation::Any);
		RemoveRegisterRange(0x8000, 0xFFFF, MemoryOperation::Read);
		//The program writes the end of a phrase the moment the chip takes its last byte - the
		//reset waits for what it still holds, as on the BT-686
		_lpcAudio.reset(new BbkLpcAudio(_console, BbkLpcAudio::LpcVariant::Yuxing));
		_lpcAudio->DeferResetUntilSpoken();
		_lpcAudio->SetTiming(8000, 160);
		_lpcAudio->Reset();
		_speechInHeader = true;
		_speechStops = 0;
	}

	void ProcessCpuClock() override
	{
		BaseProcessCpuClock();
		_lpcAudio->Clock();
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		if(addr == 0x5002) {
			return _lpcAudio->IsReady() ? 0x80 : 0x00;
		}
		return _console->GetMemoryManager()->GetOpenBus();
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr >= 0x8000) {
			Henggedianzi177::WriteRegister(addr, value);
		} else if(addr == 0x5002) {
			WriteSpeechByte(value);
		}
	}

	//As HuatongBt686::WriteSpeechByte: the header goes up to its $FC, the frames go to the
	//decoder, and the run of $FF after them ends the phrase
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

	void Serialize(Serializer& s) override
	{
		Henggedianzi177::Serialize(s);
		SV(_lpcAudio);
		SV(_speechInHeader);
		SV(_speechStops);
	}
};
