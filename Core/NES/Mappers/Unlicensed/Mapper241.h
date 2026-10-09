#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/Mappers/Subor/SuborCarts.h"
#include "NES/Mappers/Bbk/BbkLpcAudio.h"

class Mapper241 : public BaseMapper
{
private:
	//The LPC speech chip some learning cartridges carry (see SuborCarts::HasLpcSpeech), at
	//$5000-$5FFF: a write of $5x hands it the four bits x, low bit first; any other write resets
	//it, and a read has bit 6 set while it can take more. The stream is the plain "PE" one the
	//YuXing machines send, so the bits are assembled into bytes for BbkLpcAudio in that mode.
	unique_ptr<BbkLpcAudio> _lpcAudio;
	uint8_t _lpcByte = 0;
	bool _lpcHalf = false;

	bool HasSpeech() { return SuborCarts::HasLpcSpeech(_romInfo.Hash.PrgCrc32); }

	void ResetSpeech()
	{
		_lpcByte = 0;
		_lpcHalf = false;
		_lpcAudio->WriteControl(0);
		_lpcAudio->WriteControl(1);
	}

protected:
	//See SuborCarts::HasLongDendyFrame
	int32_t GetDendyScanlineCount() override { return SuborCarts::HasLongDendyFrame(_romInfo.Hash.PrgCrc32) ? 313 : 312; }
	int32_t GetDendyNmiScanline() override { return SuborCarts::HasLongDendyFrame(_romInfo.Hash.PrgCrc32) ? 292 : 291; }

	uint16_t GetPrgPageSize() override { return 0x8000; }
	uint16_t GetChrPageSize() override { return 0x2000; }
	bool AllowRegisterRead() override { return HasSpeech(); }
	bool EnableCpuClockHook() override { return HasSpeech(); }

	void InitMapper() override
	{
		if(HasSpeech()) {
			_lpcAudio.reset(new BbkLpcAudio(_console, BbkLpcAudio::LpcVariant::Yuxing));
			_lpcAudio->Reset();
			//Reads of $8000-$FFFF stay plain ROM reads
			RemoveRegisterRange(0x8000, 0xFFFF, MemoryOperation::Read);
			AddRegisterRange(0x5000, 0x5FFF, MemoryOperation::Any);
		}
		SelectPrgPage(0, 0);
		SelectChrPage(0, 0);
	}

	//The learning cartridges keep their reset code in the first 32KB only - the other banks are
	//their data, which they read with code copied to RAM - so the machine's reset button has to
	//bring the first bank back, or a reset taken while they read their data starts from garbage
	void Reset(bool softReset) override
	{
		BaseMapper::Reset(softReset);
		if(softReset && SuborCarts::IsSuborCart(_romInfo.Hash.PrgCrc32)) {
			SelectPrgPage(0, 0);
		}
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

	uint8_t ReadRegister(uint16_t addr) override
	{
		return _lpcAudio->IsReady() ? 0x40 : 0x00;
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr >= 0x8000) {
			SelectPrgPage(0, value);
		} else if((value & 0xF0) == 0x50) {
			if(_lpcHalf) {
				_lpcAudio->WriteData(_lpcByte | (uint8_t)((value & 0x0F) << 4));
			} else {
				_lpcByte = value & 0x0F;
			}
			_lpcHalf = !_lpcHalf;
		} else {
			ResetSpeech();
		}
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		if(_lpcAudio) {
			SV(_lpcAudio);
			SV(_lpcByte);
			SV(_lpcHalf);
		}
	}
};
