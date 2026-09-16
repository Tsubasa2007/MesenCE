#include "pch.h"
#include "SuperAcan/SacMemoryManager.h"
#include "SuperAcan/SacConsole.h"
#include "SuperAcan/SacCpu.h"
#include "SuperAcan/SacControlManager.h"
#include "SuperAcan/SacApu.h"
#include "Shared/Emulator.h"
#include "Shared/CpuType.h"
#include "Shared/MemoryOperationType.h"
#include "Shared/MessageManager.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/Serializer.h"

uint8_t SacSoundBus::Read(uint16_t addr)
{
	uint8_t value = _memoryManager->SoundCpuRead(addr);
	_emu->ProcessMemoryRead<CpuType::SacSound>(addr, value, MemoryOperationType::Read);
	return value;
}

uint8_t SacSoundBus::ReadOpCode(uint16_t addr)
{
	uint8_t value = _memoryManager->SoundCpuRead(addr);
	_emu->ProcessMemoryRead<CpuType::SacSound>(addr, value, MemoryOperationType::ExecOpCode);
	return value;
}

uint8_t SacSoundBus::ReadOperand(uint16_t addr)
{
	uint8_t value = _memoryManager->SoundCpuRead(addr);
	_emu->ProcessMemoryRead<CpuType::SacSound>(addr, value, MemoryOperationType::ExecOperand);
	return value;
}

void SacSoundBus::Write(uint16_t addr, uint8_t value)
{
	if(_emu->ProcessMemoryWrite<CpuType::SacSound>(addr, value, MemoryOperationType::Write)) {
		_memoryManager->SoundCpuWrite(addr, value);
	}
}

void SacSoundBus::OnInstruction()
{
	_emu->ProcessInstruction<CpuType::SacSound>();
}

void SacSoundBus::OnInterrupt(uint16_t originalPc, uint16_t newPc, bool forNmi)
{
	_emu->ProcessInterrupt<CpuType::SacSound>(originalPc, newPc, forNmi);
}

void SacSoundBus::OnHalted()
{
	_emu->ProcessHaltedCpu<CpuType::SacSound>();
}

SacMemoryManager::SacMemoryManager(Emulator* emu)
{
	_soundBus.reset(new SacSoundBus(this, emu));
	_soundCpu.reset(new W65C02(_soundBus.get()));
}

SacMemoryManager::~SacMemoryManager()
{
}

void SacMemoryManager::Init(SacConsole* console, SacCpu* cpu, uint8_t* prgRom, uint32_t prgRomSize, uint8_t* workRam)
{
	_console = console;
	_cpu = cpu;
	_prgRom = prgRom;
	_prgRomSize = prgRomSize;
	_workRam = workRam;
	memset(_saveRam, 0xFF, sizeof(_saveRam));
	memset(_lockoutKey, 0xFF, sizeof(_lockoutKey));
}

uint8_t SacMemoryManager::DebugRead(uint32_t addr)
{
	addr &= 0xFFFFFF;
	if(addr < 0x1000 && _bootRomLow) {
		return _bootRom[addr];
	} else if(addr >= 0xF80000 && addr <= 0xF80FFF && _bootRomHigh) {
		return _bootRom[addr - 0xF80000];
	} else if(addr <= SacConstants::CartEnd) {
		return _prgRom[addr % _prgRomSize];
	} else if(addr >= 0xF80000 && addr <= 0xFBFFFF) {
		return _prgRom[(addr - 0xF80000) % _prgRomSize];
	} else if(addr >= SacConstants::WorkRamStart) {
		return _workRam[addr & (SacConstants::WorkRamSize - 1)];
	} else if(addr >= 0xF40000 && addr <= 0xF5FFFF) {
		return _videoRam[addr - 0xF40000];
	} else if(addr >= 0xF00200 && addr <= 0xF003FF) {
		return _paletteRam[addr - 0xF00200];
	} else if(addr >= 0xF00000 && addr <= 0xF001FF) {
		uint16_t word = _videoRegs[(addr - 0xF00000) >> 1];
		return (addr & 1) ? (uint8_t)word : (uint8_t)(word >> 8);
	} else if(addr >= 0xE80000 && addr <= 0xE8FFFF) {
		return _soundRam[addr & 0xFFFF];
	} else if(addr >= 0xEC0000 && addr <= 0xECFFFF) {
		return (addr & 1) ? _saveRam[(addr & 0xFFFF) >> 1] : 0xFF;
	}
	return 0;
}

uint16_t SacMemoryManager::DebugRead16(uint32_t addr)
{
	return (uint16_t)((DebugRead(addr) << 8) | DebugRead(addr + 1));
}

void SacMemoryManager::DebugWrite(uint32_t addr, uint8_t value)
{
	addr &= 0xFFFFFF;
	if(addr <= SacConstants::CartEnd && !(addr < 0x1000 && _bootRomLow)) {
		_prgRom[addr % _prgRomSize] = value;
	} else if(addr >= SacConstants::WorkRamStart) {
		_workRam[addr & (SacConstants::WorkRamSize - 1)] = value;
	} else if(addr >= 0xF40000 && addr <= 0xF5FFFF) {
		_videoRam[addr - 0xF40000] = value;
	} else if(addr >= 0xF00200 && addr <= 0xF003FF) {
		_paletteRam[addr - 0xF00200] = value;
	} else if(addr >= 0xE80000 && addr <= 0xE8FFFF) {
		_soundRam[addr & 0xFFFF] = value;
	} else if(addr >= 0xEC0000 && addr <= 0xECFFFF && (addr & 1)) {
		_saveRam[(addr & 0xFFFF) >> 1] = value;
		_saveRamUsed = true;
	}
}

//At reset the machine copies the sound processor's half of its internal ROM into the bottom
//16KB of sound RAM - not code, but a register default at $40F and wave tables from $1000
void SacMemoryManager::LoadSoundTables(const vector<uint8_t>& tables)
{
	memcpy(_soundRam, tables.data(), std::min(tables.size(), (size_t)0x4000));
}

//The 68000's half of the internal ROM, and the key ROM of the cartridge's lockout chip it
//checks. With it the machine starts as it does at power-on, from the boot ROM's vectors.
void SacMemoryManager::LoadBootRom(const vector<uint8_t>& rom, const vector<uint8_t>& key)
{
	memcpy(_bootRom, rom.data(), std::min(rom.size(), sizeof(_bootRom)));
	memcpy(_lockoutKey, key.data(), std::min(key.size(), sizeof(_lockoutKey)));
	_bootRomLow = true;
	_bootRomHigh = true;
}

//Development aid while there is no debugger: the first accesses to anything unmapped or not
//emulated yet go to the log, which is otherwise the only way to see what a cartridge wants
void SacMemoryManager::LogAccess(const string& what)
{
	if(_unmappedLogCount < 64) {
		_unmappedLogCount++;
		SacCpuState cpu = _cpu->GetState();
		MessageManager::Log("[SAC] " + what + " (PC $" + HexUtilities::ToHex24(cpu.PC) + ")");
	}
}

uint8_t SacMemoryManager::Read8(uint32_t addr)
{
	addr &= 0xFFFFFF;
	if(addr < 0x1000 && _bootRomLow) {
		return _bootRom[addr];
	} else if(addr >= 0xF80000 && addr <= 0xF80FFF && _bootRomHigh) {
		return _bootRom[addr - 0xF80000];
	} else if(addr <= SacConstants::CartEnd) {
		return _prgRom[addr % _prgRomSize];
	} else if(addr >= 0xF80000 && addr <= 0xFBFFFF) {
		return _prgRom[(addr - 0xF80000) % _prgRomSize];
	} else if(addr >= SacConstants::WorkRamStart) {
		return _workRam[addr & (SacConstants::WorkRamSize - 1)];
	} else if(addr >= 0xF40000 && addr <= 0xF5FFFF) {
		return _videoRam[addr - 0xF40000];
	} else if(addr >= 0xF00200 && addr <= 0xF003FF) {
		return _paletteRam[addr - 0xF00200];
	} else if(addr >= 0xE80000 && addr <= 0xE8FFFF && (addr & 0xFFFC) != 0x200) {
		uint16_t offset = (uint16_t)addr;
		return (offset & 0xFF00) == 0x0400 ? ReadSoundRegister(offset) : _soundRam[offset];
	}

	uint16_t word = ReadRegister(addr & ~1);
	return (addr & 1) ? (uint8_t)word : (uint8_t)(word >> 8);
}

uint16_t SacMemoryManager::Read16(uint32_t addr)
{
	addr &= 0xFFFFFE;
	if(addr <= SacConstants::CartEnd || (addr >= 0xF80000 && addr <= 0xFBFFFF) || addr >= SacConstants::WorkRamStart ||
		(addr >= 0xF40000 && addr <= 0xF5FFFF) || (addr >= 0xF00200 && addr <= 0xF003FF) ||
		(addr >= 0xE80000 && addr <= 0xE8FFFF && (addr & 0xFFFC) != 0x200)) {
		return (uint16_t)((Read8(addr) << 8) | Read8(addr + 1));
	}
	return ReadRegister(addr);
}

void SacMemoryManager::Write8(uint32_t addr, uint8_t value)
{
	addr &= 0xFFFFFF;
	if(addr <= SacConstants::CartEnd || (addr >= 0xF80000 && addr <= 0xFBFFFF)) {
		return;
	} else if(addr >= SacConstants::WorkRamStart) {
		_workRam[addr & (SacConstants::WorkRamSize - 1)] = value;
		return;
	} else if(addr >= 0xF40000 && addr <= 0xF5FFFF) {
		_videoRam[addr - 0xF40000] = value;
		return;
	} else if(addr >= 0xF00200 && addr <= 0xF003FF) {
		_paletteRam[addr - 0xF00200] = value;
		return;
	} else if(addr >= 0xE80000 && addr <= 0xE8FFFF) {
		uint16_t offset = (uint16_t)addr;
		if((offset & 0xFF00) == 0x0400) {
			WriteSoundRegister(offset, value);
		} else {
			_soundRam[offset] = value;
		}
		return;
	}

	if(addr & 1) {
		WriteRegister(addr & ~1, value, 0x00FF);
	} else {
		WriteRegister(addr, (uint16_t)(value << 8), 0xFF00);
	}
}

void SacMemoryManager::Write16(uint32_t addr, uint16_t value)
{
	addr &= 0xFFFFFE;
	if(addr <= SacConstants::CartEnd || (addr >= 0xF80000 && addr <= 0xFBFFFF) || addr >= SacConstants::WorkRamStart ||
		(addr >= 0xF40000 && addr <= 0xF5FFFF) || (addr >= 0xF00200 && addr <= 0xF003FF) || (addr >= 0xE80000 && addr <= 0xE8FFFF)) {
		Write8(addr, (uint8_t)(value >> 8));
		Write8(addr + 1, (uint8_t)value);
		return;
	}
	WriteRegister(addr, value, 0xFFFF);
}

SacControlManager* SacMemoryManager::GetControlManager()
{
	return (SacControlManager*)_console->GetControlManager();
}

uint16_t SacMemoryManager::ReadRegister(uint32_t addr)
{
	if(addr >= 0xE80000 && addr <= 0xE8FFFF) {
		//Words $200 and $202 of the sound RAM are the two pads, read directly by the 68000 with a
		//pressed button reading 1. Other games shift them in through the sound processor ($407).
		return (uint16_t)(GetControlManager()->ReadPad((addr >> 1) & 0x01) ^ 0xFFFF);
	} else if(addr >= 0xF00000 && addr <= 0xF001FF) {
		return ReadVideo((addr - 0xF00000) >> 1);
	} else if(addr >= 0xE90000 && addr <= 0xE9001F) {
		return ReadHost(addr - 0xE90000);
	} else if(addr >= 0xEB0D00 && addr <= 0xEB0D03) {
		//The lockout chip is on the low byte lane only
		return ReadLockout((addr - 0xEB0D00) >> 1);
	} else if(addr >= 0xEC0000 && addr <= 0xECFFFF) {
		return _saveRam[(addr & 0xFFFF) >> 1];
	} else if(addr == 0xE90B3C || (addr >= 0xE90020 && addr <= 0xE9003F)) {
		return 0;
	}

	LogAccess("unmapped read $" + HexUtilities::ToHex24(addr));
	return 0;
}

void SacMemoryManager::WriteRegister(uint32_t addr, uint16_t value, uint16_t mask)
{
	if(addr >= 0xF00000 && addr <= 0xF001FF) {
		WriteVideo((addr - 0xF00000) >> 1, value, mask);
	} else if(addr >= 0xE90000 && addr <= 0xE9001F) {
		WriteHost(addr - 0xE90000, value, mask);
	} else if(addr >= 0xE90020 && addr <= 0xE9003F) {
		WriteDma((addr >> 4) & 1, addr & 0x0F, value);
	} else if(addr >= 0xEB0D00 && addr <= 0xEB0D03) {
		if(mask & 0x00FF) {
			WriteLockout((addr - 0xEB0D00) >> 1, (uint8_t)value);
		}
	} else if(addr >= 0xEC0000 && addr <= 0xECFFFF) {
		if(mask & 0x00FF) {
			_saveRam[(addr & 0xFFFF) >> 1] = (uint8_t)value;
			_saveRamUsed = true;
		}
	} else if(addr != 0xE90B3C) {
		LogAccess("unmapped write $" + HexUtilities::ToHex24(addr) + " = $" + HexUtilities::ToHex(value));
	}
}

uint16_t SacMemoryManager::ReadVideo(uint32_t offset)
{
	switch(offset) {
		case 0x00 / 2: {
			//Bit 15 is vblank, bit 1 the odd frame. Reading it acknowledges the vblank interrupt.
			uint16_t flags = _console->GetScanline() >= SacConstants::VblankLine ? 0x8000 : 0;
			if(_console->GetFrameCount() & 1) {
				flags |= 0x02;
			}
			_cpu->ClearIrq(7);
			return flags;
		}

		case 0x02 / 2:
			return (uint16_t)_console->GetScanline();

		default:
			return _videoRegs[offset & 0xFF];
	}
}

//Every register reads back what was written, which is also what SacPpu draws from. A few do
//something on being written as well.
void SacMemoryManager::WriteVideo(uint32_t offset, uint16_t value, uint16_t mask)
{
	uint16_t& reg = _videoRegs[offset & 0xFF];
	reg = (uint16_t)((reg & ~mask) | (value & mask));
	uint16_t data = reg;

	switch(offset) {
		case 0x0A / 2: _lineOnTarget = (data & 0x8000) ? (data & 0xFF) : -1; break;
		case 0x0C / 2: _lineOffTarget = (data & 0x8000) ? (data & 0xFF) : -1; break;

		case 0x12 / 2: _spriteDmaDest = (_spriteDmaDest & 0x0000FFFF) | ((uint32_t)data << 16); break;
		case 0x14 / 2: _spriteDmaDest = (_spriteDmaDest & 0xFFFF0000) | data; break;
		case 0x18 / 2: _spriteDmaSource = (_spriteDmaSource & 0x0000FFFF) | ((uint32_t)data << 16); break;
		case 0x1A / 2: _spriteDmaSource = (_spriteDmaSource & 0xFFFF0000) | data; break;

		case 0x1E / 2:
			if(data & 0x8000) {
				RunSpriteDma(data);
			}
			break;
	}
}

//As MAME has it: a word count at $F00010, destination and its word step at $12-$16, source and
//its step at $18-$1C, and $1E to start. Bits 13-14 aim it at video RAM; bit 8 fills with zeros.
void SacMemoryManager::RunSpriteDma(uint16_t control)
{
	uint16_t count = _videoRegs[0x10 / 2];
	uint16_t destStep = _videoRegs[0x16 / 2];
	uint16_t sourceStep = _videoRegs[0x1C / 2];

	if(control & 0x6000) {
		_spriteDmaDest |= 0xF40000;
	}

	if(_spriteDmaLogCount < 8) {
		_spriteDmaLogCount++;
		MessageManager::Log("[SAC] sprite DMA $" + HexUtilities::ToHex32(_spriteDmaSource) + " -> $" + HexUtilities::ToHex32(_spriteDmaDest) +
			" count $" + HexUtilities::ToHex(count) + " control $" + HexUtilities::ToHex(control));
	}

	for(int i = 0; i <= count; i++) {
		if(control & 0x0100) {
			Write16(_spriteDmaDest, 0);
		} else {
			Write16(_spriteDmaDest, Read16(_spriteDmaSource));
			_spriteDmaSource += 2u * sourceStep;
		}
		_spriteDmaDest += 2u * destStep;
	}
}

//Called at the start of each line. MAME's timers are one-shot, and the one that drops the line
//also disarms the one that raises it.
void SacMemoryManager::ProcessLineIrqs(uint32_t line)
{
	if(_lineOnTarget == (int32_t)line) {
		_cpu->SetIrq(5);
		_lineOnTarget = -1;
	}
	if(_lineOffTarget == (int32_t)line) {
		_cpu->ClearIrq(5);
		_lineOffTarget = -1;
		_lineOnTarget = -1;
	}
}

uint16_t SacMemoryManager::ReadHost(uint32_t offset)
{
	switch(offset & ~1) {
		case 0x04: return (uint16_t)((_soundRam[0x40C] << 8) | _soundRam[0x40D]);
		case 0x0C: return (uint16_t)((_soundRam[0x40A] << 8) | _soundRam[0x40A]);
		case 0x10: return (uint16_t)((_irqMask << 8) | _irqMask);
		case 0x14: return _frcControl;
		case 0x16: return _frcFrequency;
		case 0x18:
			if((_frcControl & 0xFF00) == 0xA300) {
				return _frcFrameCount;
			}
			//MAME's stand-in, which one game uses as a source of random numbers
			return (uint16_t)(_soundCpu->GetState().CycleCount % 0xFFFF);
		case 0x1C: return _soundCpuCtrl;
	}
	LogAccess("UM6619 read $" + HexUtilities::ToHex24(0xE90000 + offset));
	return 0;
}

void SacMemoryManager::WriteHost(uint32_t offset, uint16_t value, uint16_t mask)
{
	switch(offset & ~1) {
		case 0x0A:
			//The 68000 knocking on the sound processor's mailbox
			SetSoundIrqSource(5);
			break;

		case 0x10:
			//Written a byte at a time by most games: either byte lane lands in the same register
			if(mask & 0xFF00) {
				_irqMask = (uint8_t)(value >> 8);
			}
			if(mask & 0x00FF) {
				_irqMask = (uint8_t)value;
			}
			break;

		case 0x14:
			_frcControl = (uint16_t)((_frcControl & ~mask) | (value & mask));
			_frcFrameCount = 0;
			UpdateFrc(_cpu->GetCycleCount());
			break;

		case 0x16:
			_frcFrequency = (uint16_t)((_frcFrequency & ~mask) | (value & mask));
			_frcFrameCount = 0;
			UpdateFrc(_cpu->GetCycleCount());
			break;

		case 0x1C: {
			//Setting bit 1 or 3 switches the boot ROM out of the low or high window, for good until
			//a reset. Bit 0 holds the sound processor in reset while clear, and lets it run from
			//its reset vector when set.
			uint16_t old = _soundCpuCtrl;
			_soundCpuCtrl = (uint16_t)((_soundCpuCtrl & ~mask) | (value & mask));
			uint16_t raised = (uint16_t)(~old & _soundCpuCtrl);
			if((raised & 0x02) && _bootRomLow) {
				_bootRomLow = false;
				LogAccess("boot ROM switched out of $000000");
			}
			if((raised & 0x08) && _bootRomHigh) {
				_bootRomHigh = false;
				LogAccess("boot ROM switched out of $F80000");
			}
			if((old ^ _soundCpuCtrl) & 0x01) {
				_soundCpuRunning = (_soundCpuCtrl & 0x01) != 0;
				if(_soundCpuRunning) {
					_soundCpu->Reset();
					LogAccess("sound CPU started at $" + HexUtilities::ToHex(_soundCpu->GetState().PC));
				} else {
					LogAccess("sound CPU halted");
				}
			}
			break;
		}

		default:
			LogAccess("UM6619 write $" + HexUtilities::ToHex24(0xE90000 + offset) + " = $" + HexUtilities::ToHex(value));
			break;
	}
}

void SacMemoryManager::WriteDma(int ch, uint32_t offset, uint16_t value)
{
	switch(offset & ~1) {
		case 0x00: _dmaSource[ch] = (_dmaSource[ch] & 0x0000FFFF) | ((uint32_t)value << 16); break;
		case 0x02: _dmaSource[ch] = (_dmaSource[ch] & 0xFFFF0000) | value; break;
		case 0x04: _dmaDest[ch] = (_dmaDest[ch] & 0x0000FFFF) | ((uint32_t)value << 16); break;
		case 0x06: _dmaDest[ch] = (_dmaDest[ch] & 0xFFFF0000) | value; break;
		case 0x08: _dmaCount[ch] = value; break;

		case 0x0A: {
			if(!(value & 0x8800)) {
				break;
			}

			if(_dmaLogCount < 16) {
				_dmaLogCount++;
				MessageManager::Log("[SAC] DMA" + std::to_string(ch) + " $" + HexUtilities::ToHex32(_dmaSource[ch]) + " -> $" + HexUtilities::ToHex32(_dmaDest[ch]) +
					" count $" + HexUtilities::ToHex(_dmaCount[ch]) + " control $" + HexUtilities::ToHex(value));
			}

			//As MAME does it, modes and all, including the ones it only knows one game for
			bool destDec = (value & 0x0400) != 0;
			bool srcDec = (value & 0x0200) != 0;
			for(int i = 0; i <= _dmaCount[ch]; i++) {
				if(value == 0xA800) {
					if((_dmaDest[ch] & 0xFE0000) == 0xF40000) {
						Write16(_dmaDest[ch], 0);
						_dmaDest[ch] += destDec ? -2 : 2;
					} else {
						uint8_t srcByte = _dmaDest[ch] & 1;
						Write8(_dmaDest[ch], Read8(_dmaSource[ch] + srcByte));
						_dmaDest[ch] += destDec ? -1 : 1;
					}
				} else if(value & 0x1000) {
					Write16(_dmaDest[ch], Read16(_dmaSource[ch]));
					_dmaDest[ch] += destDec ? -2 : 2;
					_dmaSource[ch] += srcDec ? -2 : 2;
					if((value & 0x0100) && (_dmaDest[ch] & 0x0F) == 0) {
						_dmaDest[ch] -= 0x10;
					}
				} else {
					Write8(_dmaDest[ch], Read8(_dmaSource[ch]));
					_dmaDest[ch] += destDec ? -1 : 1;
					_dmaSource[ch] += srcDec ? -1 : 1;
				}
			}
			break;
		}
	}
}

uint8_t SacMemoryManager::ReadLockout(uint32_t offset)
{
	if(offset == 1) {
		return _lockoutAddress;
	}
	if(_lockoutAddress >= 0x40 && _lockoutAddress <= 0x5F) {
		return _lockoutRam[_lockoutAddress - 0x40];
	}
	//The key the boot ROM reads, from $2F down; when it matches, the boot ROM writes $FF to
	//register $09 (thought to enable the cartridge, which is not modelled) before switching out
	if(_lockoutAddress >= 0x20 && _lockoutAddress <= 0x2F) {
		return _lockoutKey[_lockoutAddress - 0x20];
	}
	return 0xFF;
}

void SacMemoryManager::WriteLockout(uint32_t offset, uint8_t value)
{
	if(offset == 1) {
		_lockoutAddress = value & 0x7F;
	} else if(_lockoutAddress >= 0x40 && _lockoutAddress <= 0x5F) {
		_lockoutRam[_lockoutAddress - 0x40] = value;
	}
}

uint8_t SacMemoryManager::SoundCpuRead(uint16_t addr)
{
	return (addr & 0xFF00) == 0x0400 ? ReadSoundRegister(addr) : _soundRam[addr];
}

void SacMemoryManager::SoundCpuWrite(uint16_t addr, uint8_t value)
{
	if((addr & 0xFF00) == 0x0400) {
		WriteSoundRegister(addr, value);
	} else {
		_soundRam[addr] = value;
	}
}

//The registers at $400-$4FF of sound RAM, as MAME has them. Both processors come through here,
//the sound processor directly and the 68000 at $E80400-$E804FF; anything not listed is plain RAM.
uint8_t SacMemoryManager::ReadSoundRegister(uint16_t offset)
{
	switch(offset) {
		case 0x402:
		case 0x403:
			return _soundShiftRegs[offset - 0x402];

		case 0x406:
			return 0x00;

		case 0x410:
			return _soundIrqEnable;

		case 0x411: {
			//Reading the sources acknowledges them all
			uint8_t sources = _soundIrqSource;
			_soundIrqSource = 0;
			UpdateSoundIrq();
			return sources;
		}

		case 0x420:
			return _soundStatus;

		case 0x422:
			_apu->Run(GetSoundChipTime());
			return _apu->Read(_soundRegAddr);

		default:
			return _soundRam[offset];
	}
}

void SacMemoryManager::WriteSoundRegister(uint16_t offset, uint8_t value)
{
	switch(offset) {
		case 0x407: {
			//The pads, shifted in serially: a falling edge on bits 0-1 latches pad 1/2, on bits 2-3
			//shifts the next bit in, and on bits 4-5 clears the shift register. A pressed button
			//shifts in as 0.
			uint8_t lowered = (uint8_t)(_soundShiftCtrl & ~value);
			_soundShiftCtrl = value;
			for(int pad = 0; pad < 2; pad++) {
				if(lowered & (1 << pad)) {
					_latchedControls[pad] = GetControlManager()->ReadPad((uint8_t)pad);
				}
				if(lowered & (1 << (pad + 2))) {
					_soundShiftRegs[pad] = (uint8_t)((_soundShiftRegs[pad] << 1) | ((_latchedControls[pad] >> 15) & 1));
					_latchedControls[pad] <<= 1;
				}
				if(lowered & (1 << (pad + 4))) {
					_soundShiftRegs[pad] = 0;
				}
			}
			break;
		}

		case 0x40A:
			//The sound processor knocking on the 68000's mailbox (some games write it from the
			//68000 side too, and MAME raises the interrupt either way)
			_cpu->SetIrq(6);
			_soundRam[0x40A] = value;
			break;

		case 0x410:
			_soundIrqEnable = value;
			UpdateSoundIrq();
			break;

		case 0x420:
			_soundRegAddr = value;
			break;

		case 0x422:
			_apu->Run(GetSoundChipTime());
			_apu->Write(_soundRegAddr, value);
			break;

		default:
			_soundRam[offset] = value;
			break;
	}
}

void SacMemoryManager::SetSoundIrqSource(uint8_t bit)
{
	_soundIrqSource |= (uint8_t)(1 << bit);
	UpdateSoundIrq();
}

//The free-running counter's interrupt, level 3 on the 68000. How its two registers set the rate
//is not known: like MAME, it runs only with $A2 in the control register's high byte, and then by
//one of three rules taken from the settings games use, in 68000 cycles.
void SacMemoryManager::UpdateFrc(uint64_t now)
{
	_frcNextCycle = 0;
	if((_frcControl & 0xFF00) != 0xA200) {
		return;
	}

	uint64_t period = 0;
	switch(_frcControl & 0x0F) {
		case 0x0: period = SacConstants::MasterClockRate / SacConstants::CpuClockDivider; break;
		case 0x1: period = 1024ull * _frcFrequency * 6 / SacConstants::CpuClockDivider; break;
		case 0xF: period = _frcFrequency * SacConstants::FrcLongStepClocks / SacConstants::CpuClockDivider; break;
	}

	if(_frcLogCount < 8) {
		_frcLogCount++;
		MessageManager::Log("[SAC] FRC control $" + HexUtilities::ToHex(_frcControl) + " frequency $" + HexUtilities::ToHex(_frcFrequency) +
			(period ? " -> every " + std::to_string(period) + " cycles" : " -> not understood, stopped"));
	}

	if(period) {
		_frcNextCycle = now + period;
	}
}

void SacMemoryManager::ProcessFrc(uint64_t cycle)
{
	if(_frcNextCycle && cycle >= _frcNextCycle) {
		_cpu->SetIrq(3);
		UpdateFrc(_frcNextCycle);
	}
}

//With $A3 in the control register's high byte the counter counts frames instead: it interrupts
//once every frequency + 1 of them, and $E90018 reads how far it has counted. One intro crawls its
//text up by a step on each interrupt and pulls in the next page when the scroll is three steps
//along and $E90018 reads 2; Bcan's crawl, measured against ours over 8.5 seconds, has a period of
//9.15 +/- 0.16 frames for a frequency of 8.
void SacMemoryManager::ProcessFrcFrame()
{
	if((_frcControl & 0xFF00) != 0xA300) {
		return;
	}

	_frcFrameCount++;
	if(_frcFrameCount > _frcFrequency) {
		_frcFrameCount = 0;
		_cpu->SetIrq(3);
	}
}

//The sound chip's timer and streaming interrupts, which it raises and a read of one of its
//registers drops
void SacMemoryManager::SetSoundIrqLine(uint8_t bit, bool state)
{
	if(state) {
		_soundIrqSource |= (uint8_t)(1 << bit);
	} else {
		_soundIrqSource &= (uint8_t)~(1 << bit);
	}
	UpdateSoundIrq();
}

//Where on the master clock the processor touching the sound chip has got to. The 68000 runs each
//line before the sound processor does, so the two are not in step within a line.
uint64_t SacMemoryManager::GetSoundChipTime()
{
	if(_inSoundCpu) {
		return _soundCpu->GetState().CycleCount * SacConstants::SoundCpuClockDivider;
	}
	return _cpu->GetCycleCount() * SacConstants::CpuClockDivider;
}

void SacMemoryManager::UpdateSoundIrq()
{
	_soundCpu->SetIrq((_soundIrqEnable & _soundIrqSource) != 0);
}

void SacMemoryManager::RunSoundCpu(uint64_t targetCycle)
{
	W65C02::State& state = _soundCpu->GetState();
	if(!_soundCpuRunning) {
		//Held in reset, its clock still keeps pace with the machine's
		if(state.CycleCount < targetCycle) {
			state.CycleCount = targetCycle;
		}
		return;
	}

	_inSoundCpu = true;
	while(state.CycleCount < targetCycle) {
		_soundCpu->Exec();
	}
	_inSoundCpu = false;
}

void SacMemoryManager::GetState(SacSystemState& system, SacSoundCpuState& soundCpu)
{
	for(int ch = 0; ch < 2; ch++) {
		system.DmaSource[ch] = _dmaSource[ch];
		system.DmaDest[ch] = _dmaDest[ch];
		system.DmaCount[ch] = _dmaCount[ch];
		system.LatchedControls[ch] = _latchedControls[ch];
		system.SoundShiftRegs[ch] = _soundShiftRegs[ch];
	}
	system.SpriteDmaSource = _spriteDmaSource;
	system.SpriteDmaDest = _spriteDmaDest;
	system.LineOnTarget = _lineOnTarget;
	system.LineOffTarget = _lineOffTarget;
	system.SoundCpuControl = _soundCpuCtrl;
	system.FrcControl = _frcControl;
	system.FrcFrequency = _frcFrequency;
	system.IrqMask = _irqMask;
	system.IrqLines = _cpu->GetIrqLines();
	system.SoundIrqEnable = _soundIrqEnable;
	system.SoundIrqSource = _soundIrqSource;
	system.SoundShiftControl = _soundShiftCtrl;
	system.SoundStatus = _soundStatus;
	system.SoundRegAddress = _soundRegAddr;
	system.LockoutAddress = _lockoutAddress;
	system.BootRomLow = _bootRomLow;
	system.BootRomHigh = _bootRomHigh;

	GetSoundCpuState(soundCpu);
}

void SacMemoryManager::GetSoundCpuState(SacSoundCpuState& state)
{
	W65C02::State& cpu = _soundCpu->GetState();
	state.CycleCount = cpu.CycleCount;
	state.PC = cpu.PC;
	state.A = cpu.A;
	state.X = cpu.X;
	state.Y = cpu.Y;
	state.SP = cpu.SP;
	state.PS = cpu.PS;
	state.Running = _soundCpuRunning;
	state.Waiting = cpu.Waiting;
	state.Stopped = cpu.Stopped;
	state.IrqLine = cpu.IrqLine;
	state.NmiPending = cpu.NmiPending;
}

//The registers the debugger lets the user edit
void SacMemoryManager::SetSoundCpuState(SacSoundCpuState& state)
{
	W65C02::State& cpu = _soundCpu->GetState();
	cpu.PC = state.PC;
	cpu.A = state.A;
	cpu.X = state.X;
	cpu.Y = state.Y;
	cpu.SP = state.SP;
	cpu.PS = state.PS;
}

void SacMemoryManager::TriggerSoundNmi()
{
	if(_soundCpuRunning) {
		_soundCpu->TriggerNmi();
	}
}

void SacMemoryManager::Serialize(Serializer& s)
{
	SVArray(_soundRam, sizeof(_soundRam));
	SVArray(_videoRam, sizeof(_videoRam));
	SVArray(_paletteRam, sizeof(_paletteRam));
	SVArray(_saveRam, sizeof(_saveRam));
	SV(_saveRamUsed);
	SVArray(_videoRegs, 0x100);
	SV(_irqMask);
	SV(_soundCpuCtrl);
	SV(_frcControl);
	SV(_frcFrequency);
	SV(_frcNextCycle);
	SV(_frcFrameCount);
	SVArray(_dmaSource, 2);
	SVArray(_dmaDest, 2);
	SVArray(_dmaCount, 2);
	SV(_lockoutAddress);
	SVArray(_lockoutRam, 0x20);
	SV(_bootRomLow);
	SV(_bootRomHigh);
	SV(_spriteDmaSource);
	SV(_spriteDmaDest);
	SV(_lineOnTarget);
	SV(_lineOffTarget);

	W65C02::State& cpu = _soundCpu->GetState();
	SV(cpu.PC); SV(cpu.A); SV(cpu.X); SV(cpu.Y); SV(cpu.SP); SV(cpu.PS);
	SV(cpu.CycleCount); SV(cpu.Waiting); SV(cpu.Stopped); SV(cpu.IrqLine); SV(cpu.NmiPending);
	SV(_soundCpuRunning);
	SV(_soundIrqEnable);
	SV(_soundIrqSource);
	SV(_soundShiftCtrl);
	SVArray(_soundShiftRegs, 2);
	SVArray(_latchedControls, 2);
	SV(_soundStatus);
	SV(_soundRegAddr);
}
