#pragma once
#include "pch.h"
#include "Shared/Interfaces/IConsole.h"
#include "SuperAcan/SacTypes.h"

class Emulator;
class SacControlManager;
class SacCpu;
class SacMemoryManager;
class SacPpu;
class SacApu;

class SacConsole final : public IConsole
{
private:
	Emulator* _emu = nullptr;
	unique_ptr<SacControlManager> _controlManager;
	unique_ptr<SacMemoryManager> _memoryManager;
	unique_ptr<SacCpu> _cpu;
	unique_ptr<SacPpu> _ppu;
	unique_ptr<SacApu> _apu;

	uint8_t* _prgRom = nullptr;
	uint32_t _prgRomSize = 0;

	uint8_t* _workRam = nullptr;
	uint32_t _workRamSize = 0;

	uint16_t* _frameBuffer = nullptr;
	uint32_t _frameCount = 0;
	uint32_t _scanline = 0;
	uint64_t _cpuTargetCycle = 0;
	uint64_t _soundCpuTargetCycle = 0;

	void LoadSoundTables();
	void LoadBootRom();
	void SendFrame();
	void DumpFrame();
	void LogProgress();
	void RunCpu(uint64_t targetCycle);

public:
	SacConsole(Emulator* emu);
	~SacConsole();

	static vector<string> GetSupportedExtensions() { return { ".bin" }; }
	static vector<string> GetSupportedSignatures() { return {}; }

	LoadRomResult LoadRom(VirtualFile& romFile) override;
	void RunFrame() override;

	void Reset() override;
	void SaveBattery() override;

	uint32_t GetScanline() { return _scanline; }
	uint16_t GetLineCycle();
	SacCpu* GetCpu() { return _cpu.get(); }
	SacPpu* GetPpu() { return _ppu.get(); }
	SacMemoryManager* GetMemoryManager() { return _memoryManager.get(); }
	uint16_t* GetFrameBuffer() { return _frameBuffer; }

	BaseControlManager* GetControlManager() override;
	ConsoleRegion GetRegion() override;
	ConsoleType GetConsoleType() override;
	vector<CpuType> GetCpuTypes() override;
	uint64_t GetMasterClock() override;
	uint32_t GetMasterClockRate() override;
	double GetFps() override;
	BaseVideoFilter* GetVideoFilter(bool getDefaultFilter) override;
	PpuFrameInfo GetPpuFrame() override;
	uint32_t GetFrameCount() override;
	RomFormat GetRomFormat() override;
	AudioTrackInfo GetAudioTrackInfo() override;
	void ProcessAudioPlayerAction(AudioPlayerActionParams p) override;
	AddressInfo GetAbsoluteAddress(AddressInfo& relAddress) override;
	AddressInfo GetRelativeAddress(AddressInfo& absAddress, CpuType cpuType) override;
	void GetConsoleState(BaseState& state, ConsoleType consoleType) override;

	void Serialize(Serializer& s) override;
};
