#pragma once
#include "NES/BaseMapper.h"
#include "NES/NesConsole.h"
#include "NES/NesMemoryManager.h"
#include "NES/Mappers/Bbk/BbkLpcAudio.h"
#include "NES/Mappers/Subor/SuborCarts.h"

class Waixing178 : public BaseMapper
{
private:
	uint8_t _regs[4] = {};

	//The multimedia cards' LPC speech chip (see SuborCarts::HasLpcSpeech), at $5800-$5FFF: the
	//same chip and protocol as on Mapper241 - a write of $5x hands it the four bits x, low bit
	//first, any other write resets it, and a read has bit 6 set while it can take more. The cards
	//write the reset as soon as the last byte of a phrase is queued, so it waits for the queued
	//bytes to be spoken, or each phrase would lose its last word's ending.
	unique_ptr<BbkLpcAudio> _lpcAudio;
	uint8_t _lpcByte = 0;
	bool _lpcHalf = false;

	bool HasSpeech() { return SuborCarts::HasLpcSpeech(_romInfo.Hash.PrgCrc32); }

protected:
	uint16_t GetPrgPageSize() override { return 0x4000; }
	uint16_t GetChrPageSize() override { return 0x2000; }
	uint16_t RegisterStartAddress() override { return 0x4800; }
	//Only $4800-$4803: a learning cartridge on this board writes $480D as one of its programs
	//starts, which as a mirror of $4801 would switch that program's code out from under it
	uint16_t RegisterEndAddress() override { return 0x4803; }
	uint32_t GetWorkRamSize() override { return 0x8000; }
	bool AllowRegisterRead() override { return HasSpeech(); }
	//See SuborCarts::HasLongDendyFrame
	int32_t GetDendyScanlineCount() override { return SuborCarts::HasLongDendyFrame(_romInfo.Hash.PrgCrc32) ? 313 : 312; }
	int32_t GetDendyNmiScanline() override { return SuborCarts::HasLongDendyFrame(_romInfo.Hash.PrgCrc32) ? 292 : 291; }
	bool EnableCpuClockHook() override { return HasSpeech(); }

	void InitMapper() override
	{
		memset(_regs, 0, sizeof(_regs));
		if(HasSpeech()) {
			_lpcAudio.reset(new BbkLpcAudio(_console, BbkLpcAudio::LpcVariant::Yuxing));
			_lpcAudio->DeferResetUntilSpoken();
			_lpcAudio->Reset();
			AddRegisterRange(0x5800, 0x5FFF, MemoryOperation::Any);
		}
		UpdateState();
		SelectChrPage(0, 0);
	}

	void Reset(bool softReset) override
	{
		BaseMapper::Reset(softReset);
		if(_lpcAudio) {
			_lpcAudio->Reset();
			_lpcByte = 0;
			_lpcHalf = false;
		}
	}

	void ProcessCpuClock() override
	{
		BaseProcessCpuClock();
		if(_lpcAudio) {
			_lpcAudio->Clock();
		}
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SVArray(_regs, 4);
		if(_lpcAudio) {
			SV(_lpcAudio);
			SV(_lpcByte);
			SV(_lpcHalf);
		}
		if(!s.IsSaving()) {
			UpdateState();
		}
	}

	void UpdateState()
	{
		uint16_t sbank = _regs[1] & 0x07;
		uint16_t bbank = _regs[2];
		if(_regs[0] & 0x02) {
			SelectPrgPage(0, (bbank << 3) | sbank);
			if(_regs[0] & 0x04) {
				SelectPrgPage(1, (bbank << 3) | 0x06 | (_regs[1] & 0x01));
			} else {
				SelectPrgPage(1, (bbank << 3) | 0x07);
			}
		} else {
			uint16_t bank = (bbank << 3) | sbank;
			if(_regs[0] & 0x04) {
				SelectPrgPage(0, bank);
				SelectPrgPage(1, bank);
			} else {
				SelectPrgPage2x(0, bank);
			}
		}

		SetCpuMemoryMapping(0x6000, 0x7FFF, _regs[3] & 0x03, PrgMemoryType::WorkRam, MemoryAccessType::ReadWrite);
		SetMirroringType(_regs[0] & 0x01 ? MirroringType::Horizontal : MirroringType::Vertical);
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		if(addr >= 0x5800) {
			return _lpcAudio->IsReady() ? 0x40 : 0x00;
		}
		return _console->GetMemoryManager()->GetOpenBus();
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr < 0x5800) {
			_regs[addr & 0x03] = value;
			UpdateState();
		} else if((value & 0xF0) == 0x50) {
			if(_lpcHalf) {
				_lpcAudio->WriteData(_lpcByte | (uint8_t)((value & 0x0F) << 4));
			} else {
				_lpcByte = value & 0x0F;
			}
			_lpcHalf = !_lpcHalf;
		} else {
			_lpcByte = 0;
			_lpcHalf = false;
			_lpcAudio->WriteControl(0);
			_lpcAudio->WriteControl(1);
		}
	}
};