#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/NesConsole.h"
#include "NES/BaseNesPpu.h"
#include "NES/NesControlManager.h"
#include "NES/Input/Sb2kKeyboard.h"
#include "NES/Input/ZechengKeyMatrix.h"
#include "Utilities/Serializer.h"

//NES 2.0 mapper 560, submapper 1: the Subor V1.0 (小霸王, SB-218) learning cartridge - a C/E BASIC
//board with a character generator. Ported from the reference emulator's Mapper560.
//
//- 64KB of PRG in two 32KB halves; any write to $8000-$FFFF switches to the other one. It starts
//  in the second.
//- 8KB of CHR RAM for ordinary tiles, which comes up holding the first 8KB of the CHR ROM, and the
//  128KB CHR ROM itself as a font: 8192 tiles' worth of bit planes.
//- While the second half is in, each background tile may come from the font instead. The byte at
//  the same position in nametable 1 picks it: 1 means the ordinary tile, anything else is a font
//  page - its low five bits are bits 12-8 of a font tile whose low bits are the nametable byte,
//  and bit 5 picks that tile's first or second plane. A font tile is drawn 1 bit per pixel in
//  colour 1. The board has four nametables, so nametable 1 is its own memory.
//
//The plain C/E BASIC cartridges (submapper 0) are wired as NESdev's mapper 560 page has them,
//not as the reference emulator draws them (it copies the CHR ROM into RAM and patches particular
//screens by what their nametables hold). There is no CHR RAM: the 128KB CHR ROM holds 16384
//one-plane tiles of 8 bytes. The page has $1000-$1FFF mirror $0000-$0FFF, but these programs
//draw their mode 0 screens from tiles 256-511 there, so PPU A12 drives CHR A11 here.
//- First half (mode 0, the programs taken from Family BASIC): an ordinary 2-plane picture whose
//  tile N has its first plane at N*8 and its second at $10000 + N*8 - PPU A3 drives CHR A16.
//- Second half (mode 1): every background tile number is 14 bits - the nametable byte for the low
//  eight, and the low six bits of the byte at the same place in nametable 1 (A10 set) for the high
//  ones. PPU A3 is ignored, so
//  both planes read the same byte and the tile is drawn in colours 0 and 3. Sprites stay mode 0.
//
//Most of these take the Family BASIC keyboard. One's header asks for the Keda keyboard instead,
//which its program scans the way the ZeCheng-type keyboard is scanned (see ZechengKeyMatrix) -
//$4016 = 5, then per row 7 (rewind the column), 6, eight reads of $4017 bit 1 (set while the key
//is down) and 3 (next row), for ten rows - and whose Shift and Ctrl it finds where that matrix
//has them. The scan is run here.
class Subor560 : public BaseMapper
{
private:
	bool _secondHalf = true;
	bool _ceBasic = false;

	bool _kedaKeyboard = false;
	uint8_t _kbdRow = 0;
	uint8_t _kbdColumn = 0;
	bool _kbdArmed = false;

	NesControlManager* NesControls() { return (NesControlManager*)_console->GetControlManager(); }

	//The last nametable byte the picture fetched, whose position picks the font page
	uint16_t _ntAddr = 0;

	void UpdateState()
	{
		SelectPrgPage(0, _secondHalf ? 1 : 0);
		if(_ceBasic) {
			SelectChrPage(0, 0, ChrMemoryType::ChrRom);
			SelectChrPage(1, 0, ChrMemoryType::ChrRom);
		} else {
			SelectChrPage(0, 0, ChrMemoryType::ChrRam);
			SelectChrPage(1, 1, ChrMemoryType::ChrRam);
		}
	}

	uint8_t ReadChrRom(uint32_t offset)
	{
		return _chrRomSize ? _chrRom[offset % _chrRomSize] : 0;
	}

	bool IsBackgroundFetch()
	{
		uint32_t cycle = _console->GetPpu()->GetCurrentCycle();
		return cycle <= 256 || cycle >= 321;
	}

protected:
	uint16_t GetPrgPageSize() override { return 0x8000; }
	uint16_t GetChrPageSize() override { return 0x1000; }
	uint32_t GetChrRamSize() override { return 0x2000; }
	bool EnableCustomVramRead() override { return true; }
	bool AllowRegisterRead() override { return _romInfo.InputType == GameInputType::KedaKeyboard; }

	void GetMemoryRanges(MemoryRanges& ranges) override
	{
		BaseMapper::GetMemoryRanges(ranges);
		if(_romInfo.InputType == GameInputType::KedaKeyboard) {
			ranges.AddHandler(MemoryOperation::Write, 0x4016);
			ranges.AddHandler(MemoryOperation::Read, 0x4017);
			ranges.SetAllowOverride();
		}
	}

	void InitMapper(RomData& romData) override
	{
		romData.Info.System = GameSystem::Dendy;
	}

	void InitMapper() override
	{
		_romInfo.System = GameSystem::Dendy;
		_ceBasic = !IsSuborV1(_romInfo);
		_kedaKeyboard = _romInfo.InputType == GameInputType::KedaKeyboard;
		if(_kedaKeyboard) {
			RemoveRegisterRange(0x8000, 0xFFFF, MemoryOperation::Read);
			AddRegisterRange(0x4016, 0x4016, MemoryOperation::Write);
			AddRegisterRange(0x4017, 0x4017, MemoryOperation::Read);
		}
		if(!_ceBasic && _chrRam && _chrRom) {
			memcpy(_chrRam, _chrRom, std::min(_chrRamSize, _chrRomSize));
		}
		UpdateState();
	}

	void Reset(bool softReset) override
	{
		BaseMapper::Reset(softReset);
		_secondHalf = true;
		_kbdRow = 0;
		_kbdColumn = 0;
		_kbdArmed = false;
		UpdateState();
	}

	uint8_t MapperReadVram(uint16_t addr, MemoryOperationType type) override
	{
		if(_ceBasic) {
			if(addr >= 0x2000) {
				if(type == MemoryOperationType::PpuRenderingRead && (addr & 0x3FF) < 0x3C0) {
					_ntAddr = addr;
				}
				return InternalReadVram(addr);
			}
			if(_secondHalf && type == MemoryOperationType::PpuRenderingRead && IsBackgroundFetch()) {
				uint8_t high = InternalReadVram(0x2400 | (_ntAddr & 0x3FF)) & 0x3F;
				return ReadChrRom((high << 11) | ((addr & 0xFF0) >> 1) | (addr & 0x07));
			}
			return ReadChrRom(((addr & 0x08) << 13) | ((addr & 0x1FF0) >> 1) | (addr & 0x07));
		}

		if(type == MemoryOperationType::PpuRenderingRead) {
			if(addr >= 0x2000) {
				if((addr & 0x3FF) < 0x3C0) {
					_ntAddr = addr;
				}
			} else if(_secondHalf && IsBackgroundFetch()) {
				uint8_t page = InternalReadVram(0x2400 | (_ntAddr & 0x3FF));
				if(page != 1) {
					if(addr & 0x08) {
						//Font tiles are 1bpp: their second plane is empty
						return 0;
					}
					uint32_t tile = ((page & 0x1F) << 8) | ((addr >> 4) & 0xFF);
					uint32_t offset = (tile << 4) | ((page & 0x20) ? 0x08 : 0) | (addr & 0x07);
					return _chrRomSize ? _chrRom[offset % _chrRomSize] : 0;
				}
			}
		}
		return InternalReadVram(addr);
	}

public:
	static bool IsSuborV1(const NesRomInfo& info)
	{
		return info.SubMapperID == 1 || info.Hash.PrgCrc32 == 0xC07ADC88;
	}

protected:
	uint8_t ReadRegister(uint16_t addr) override
	{
		Sb2kKeyboard* kbd = NesControls()->GetControlDevice<Sb2kKeyboard>().get();
		return (NesControls()->ReadRam(addr) & ~0x02) | ZechengKeyMatrix::Read(kbd, _kbdRow, _kbdColumn);
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr == 0x4016) {
			NesControls()->WriteRam(addr, value);
			ZechengKeyMatrix::Write(value, _kbdRow, _kbdColumn, _kbdArmed);
			return;
		}
		_secondHalf = !_secondHalf;
		UpdateState();
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SV(_secondHalf);
		SV(_ntAddr);
		if(_kedaKeyboard) {
			SV(_kbdRow);
			SV(_kbdColumn);
			SV(_kbdArmed);
		}
		if(!s.IsSaving()) {
			UpdateState();
		}
	}
};
