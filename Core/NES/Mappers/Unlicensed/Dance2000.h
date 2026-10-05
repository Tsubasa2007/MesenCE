#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/NesConsole.h"
#include "NES/NesMemoryManager.h"
#include "NES/NesControlManager.h"
#include "NES/Mappers/Bbk/BbkPrinter.h"
#include "NES/Mappers/Bbk/BbkLpcAudio.h"
#include "NES/Mappers/Subor/SuborCarts.h"
#include "Utilities/FolderUtilities.h"

//The Subor learning cartridges on this board carry two more things:
// - $5300: the LPC-10 speech chip of the Subor V7 board (see Subor168) - status $80 = can take
//   data, $0F = end of speech. The add-on cards' programs wait on it before they read a key.
// - a parallel printer driven the way the BBK voice models drive theirs: a byte written to
//   $480F, after waiting for bit 0 of $4016 - the printer's ready line, and also the port 1
//   controller's data line, so which of the two is attached is the printer setting.
class Dance2000 : public BaseMapper
{
private:
	uint8_t _prgReg = 0;
	uint8_t _mode = 0;
	uint8_t _lastNt = 0;

	bool _isSuborCart = false;
	bool _printerAlways = false;
	bool _printerNamed = false;
	BbkPrinter _printer;
	unique_ptr<BbkLpcAudio> _lpcAudio;

	//Named lazily: the rom path is not available yet when the mapper is constructed
	BbkPrinter& Printer()
	{
		if(!_printerNamed) {
			_printerNamed = true;
			_printer.SetRomName(FolderUtilities::GetFilename(_emu->GetRomInfo().RomFile.GetFilePath(), false));
		}
		return _printer;
	}

protected:
	bool AllowRegisterRead() override { return true; }
	uint16_t GetPrgPageSize() override { return 0x4000; }
	uint16_t GetChrPageSize() override { return 0x1000; }
	bool EnableVramAddressHook() override { return true; }
	bool EnableCpuClockHook() override { return true; }

	void ProcessCpuClock() override
	{
		BaseProcessCpuClock();
		if(_isSuborCart) {
			_printer.Clock();
			_lpcAudio->Clock();
		}
	}

	//$4016 is below the range the base mapper claims, so it has to be asked for to be seen
	void GetMemoryRanges(MemoryRanges& ranges) override
	{
		BaseMapper::GetMemoryRanges(ranges);
		if(_isSuborCart) {
			ranges.AddHandler(MemoryOperation::Read, 0x4016);
			ranges.SetAllowOverride();
		}
	}

	void InitMapper() override
	{
		_prgReg = _mode = _lastNt = 0;
		AddRegisterRange(0x5000, 0x5FFF, MemoryOperation::Write);
		RemoveRegisterRange(0x8000, 0xFFFF, MemoryOperation::Write);

		_isSuborCart = SuborCarts::IsSuborCart(_romInfo.Hash.PrgCrc32);
		_printerAlways = SuborCarts::HasPrinterAlways(_romInfo.Hash.PrgCrc32);
		if(_isSuborCart) {
			AddRegisterRange(0x480F, 0x480F, MemoryOperation::Write);
			AddRegisterRange(0x4016, 0x4016, MemoryOperation::Read);
			AddRegisterRange(0x5300, 0x53FF, MemoryOperation::Read);
			_printerNamed = false;
			_printer.Reset();
			_lpcAudio.reset(new BbkLpcAudio(_console, BbkLpcAudio::LpcVariant::Sb2k));
			_lpcAudio->Reset();
		}
		UpdateState();
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SV(_mode);
		SV(_prgReg);
		SV(_lastNt);
		if(_isSuborCart) {
			SV(_printer);
			SV(_lpcAudio);
		}
	}

	void NotifyVramAddressChange(uint16_t addr) override
	{
		if(_mode & 0x02) {
			if((addr & 0x3000) == 0x2000) {
				//The page follows the nametable the fetch lands in, so it is the mirrored address
				//line that picks it: A11 with horizontal mirroring, A10 with vertical. Taking A11
				//always left a left/right split on one page - the Subor cartridges' Internet page
				//puts its text panel in the right-hand nametable under vertical mirroring.
				uint32_t currentNametable = (addr >> (GetMirroringType() == MirroringType::Vertical ? 10 : 11)) & 0x01;
				if(currentNametable != _lastNt) {
					_lastNt = currentNametable;
					SelectChrPage(0, _lastNt);
				}
			}
		} else {
			if(_lastNt != 0) {
				_lastNt = 0;
				SelectChrPage(0, _lastNt);
			}
		}
	}

	void UpdateState()
	{
		SelectChrPage(0, _lastNt);
		SelectChrPage(1, 1);
		//The Subor learning cartridges on this board are up to 2MB and use every bank bit up to bit 6:
		//a 512KB one keeps its font and half its programs in banks $10-$1F, a 1MB one starts its
		//menu from bank $2F or $3F, and a 2MB one is a 1MB cartridge with an add-on card in banks
		//$40-$7F. Smaller boards simply wrap.
		if(_mode & 0x04) {
			SelectPrgPage2x(0, (_prgReg & 0x3F) << 1);
		} else {
			SelectPrgPage(0, _prgReg & 0x7F);
			SelectPrgPage(1, 0);
		}
		SetMirroringType(_mode & 0x01 ? MirroringType::Horizontal : MirroringType::Vertical);
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		if(addr == 0x4016) {
			//With the printer on the line it reads ready; with a pad on it, the pad's data - which
			//the print routine takes for "not ready" and reports as a printer error
			uint8_t value = ((NesControlManager*)_console->GetControlManager())->ReadRam(addr);
			bool printer = _printerAlways || _console->GetNesConfig().Yuyin2Printer;
			return printer ? ((value & 0xFE) | 0x01) : value;
		}
		if(_isSuborCart && (addr & 0xFF00) == 0x5300) {
			return (_lpcAudio->IsBusy() ? 0x00 : 0x80) | (_lpcAudio->IsSpeechEnd() ? 0x0F : 0x00);
		}
		//On the 256KB multicart bit 6 takes the ROM off the bus. The bigger Subor cartridges use it
		//as a bank bit - a 1MB one reads its font with it set, and a 2MB one's add-on card is there.
		bool romOff = (_prgReg & 0x40) && GetPrgPageCount() <= 16;
		return romOff ? _console->GetMemoryManager()->GetOpenBus() : InternalReadRam(addr);
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr == 0x480F) {
			Printer().WriteData(value);
			return;
		}
		if(_isSuborCart && (addr & 0xFF00) == 0x5300) {
			_lpcAudio->WriteData(value);
			return;
		}
		if(addr == 0x5000) {
			_prgReg = value;
			UpdateState();
		} else if(addr == 0x5200) {
			_mode = value;
			if(_mode & 0x04) {
				UpdateState();
			}
		}
	}
};