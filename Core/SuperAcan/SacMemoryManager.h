#pragma once
#include "pch.h"
#include "SuperAcan/SacTypes.h"
#include "SuperAcan/W65C02.h"
#include "Utilities/ISerializable.h"

class SacApu;
class SacConsole;
class SacControlManager;
class SacCpu;
class SacMemoryManager;

//The sound processor's view of the machine: all 64KB of it is the sound RAM the 68000 shares,
//with the register window at $400-$4FF laid over it
class SacSoundBus final : public W65C02::Bus
{
private:
	SacMemoryManager* _memoryManager = nullptr;

public:
	SacSoundBus(SacMemoryManager* memoryManager) : _memoryManager(memoryManager) {}

	uint8_t Read(uint16_t addr) override;
	void Write(uint16_t addr, uint8_t value) override;
};

//The 68000's view of the machine, after MAME's supracan main_map, and for now the sound
//processor with it. At power-on the internal boot ROM overlays $000000-$000FFF and
//$F80000-$F80FFF, checks the cartridge's lockout chip and switches itself out. Without it every
//cartridge still carries reset vectors of its own, and the machine starts as the boot ROM would
//leave it.
class SacMemoryManager final : public ISerializable
{
private:
	SacConsole* _console = nullptr;
	SacCpu* _cpu = nullptr;

	uint8_t* _prgRom = nullptr;
	uint32_t _prgRomSize = 0;
	uint8_t* _workRam = nullptr;

	uint8_t _soundRam[0x10000] = {};
	uint8_t _videoRam[0x20000] = {};
	uint8_t _paletteRam[0x200] = {};

	//The cartridge's save RAM sits on the low byte lane only, so $EC0000-$ECFFFF is 32KB of it
	uint8_t _saveRam[0x8000] = {};

	uint16_t _videoRegs[0x100] = {};

	//UM6619 host registers
	uint8_t _irqMask = 0;
	uint16_t _soundCpuCtrl = 0;
	uint16_t _frcControl = 0;
	uint16_t _frcFrequency = 0;
	uint64_t _frcNextCycle = 0;
	uint32_t _frcLogCount = 0;

	uint32_t _dmaSource[2] = {};
	uint32_t _dmaDest[2] = {};
	uint16_t _dmaCount[2] = {};

	//The video chip's own DMA, used to fill and copy sprite tables; its addresses move on as it runs
	uint32_t _spriteDmaSource = 0;
	uint32_t _spriteDmaDest = 0;
	uint32_t _spriteDmaLogCount = 0;

	//The raster interrupt: level 5 raised when one line is reached and dropped at another
	int32_t _lineOnTarget = -1;
	int32_t _lineOffTarget = -1;

	//UM6650 lockout chip, one per cartridge: an address register, a data port, 32 bytes of RAM
	//and a 16-byte key ROM. Only the machine's boot ROM talks to it; no cartridge does.
	uint8_t _lockoutAddress = 0x7F;
	uint8_t _lockoutRam[0x20] = {};
	uint8_t _lockoutKey[0x10] = {};

	//The machine's internal boot ROM, over the first 4KB of both cartridge windows until it
	//switches itself out
	uint8_t _bootRom[0x1000] = {};
	bool _bootRomLow = false;
	bool _bootRomHigh = false;

	//The sound processor and the registers at $400-$4FF of sound RAM, which both processors see
	unique_ptr<SacSoundBus> _soundBus;
	unique_ptr<W65C02> _soundCpu;
	bool _soundCpuRunning = false;
	uint8_t _soundIrqEnable = 0;
	uint8_t _soundIrqSource = 0;
	uint8_t _soundShiftCtrl = 0;
	uint8_t _soundShiftRegs[2] = {};
	uint16_t _latchedControls[2] = {};
	uint8_t _soundStatus = 0;
	uint8_t _soundRegAddr = 0;

	//The sound chip behind $420/$422, and which processor is running, to know how far to run it
	SacApu* _apu = nullptr;
	bool _inSoundCpu = false;

	uint32_t _unmappedLogCount = 0;
	uint32_t _dmaLogCount = 0;

	uint16_t ReadRegister(uint32_t addr);
	void WriteRegister(uint32_t addr, uint16_t value, uint16_t mask);

	uint16_t ReadVideo(uint32_t offset);
	void WriteVideo(uint32_t offset, uint16_t value, uint16_t mask);
	void RunSpriteDma(uint16_t control);
	SacControlManager* GetControlManager();
	uint16_t ReadHost(uint32_t offset);
	void WriteHost(uint32_t offset, uint16_t value, uint16_t mask);
	void WriteDma(int ch, uint32_t offset, uint16_t value);
	uint8_t ReadLockout(uint32_t offset);
	void WriteLockout(uint32_t offset, uint8_t value);

	uint8_t ReadSoundRegister(uint16_t offset);
	void WriteSoundRegister(uint16_t offset, uint8_t value);
	void SetSoundIrqSource(uint8_t bit);
	void UpdateSoundIrq();
	uint64_t GetSoundChipTime();
	void UpdateFrc(uint64_t now);

	void LogAccess(const string& what);

public:
	SacMemoryManager();
	~SacMemoryManager();

	void Init(SacConsole* console, SacCpu* cpu, uint8_t* prgRom, uint32_t prgRomSize, uint8_t* workRam);
	void LoadSoundTables(const vector<uint8_t>& tables);
	void LoadBootRom(const vector<uint8_t>& rom, const vector<uint8_t>& key);

	uint8_t Read8(uint32_t addr);
	uint16_t Read16(uint32_t addr);
	void Write8(uint32_t addr, uint8_t value);
	void Write16(uint32_t addr, uint16_t value);

	uint8_t SoundCpuRead(uint16_t addr);
	void SoundCpuWrite(uint16_t addr, uint8_t value);
	void RunSoundCpu(uint64_t targetCycle);
	void TriggerSoundNmi();
	void SetApu(SacApu* apu) { _apu = apu; }
	uint64_t GetFrcTarget() { return _frcNextCycle; }
	void ProcessFrc(uint64_t cycle);
	void SetSoundIrqLine(uint8_t bit, bool state);
	uint8_t* GetSoundRam() { return _soundRam; }
	bool IsSoundCpuRunning() { return _soundCpuRunning; }
	W65C02::State GetSoundCpuState() { return _soundCpu->GetState(); }

	uint8_t GetIrqMask() { return _irqMask; }
	void ProcessLineIrqs(uint32_t line);

	uint8_t* GetVideoRam() { return _videoRam; }
	uint8_t* GetPaletteRam() { return _paletteRam; }
	uint16_t* GetVideoRegs() { return _videoRegs; }

	void Serialize(Serializer& s) override;
};
