#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/IDebugger.h"

class Disassembler;
class Debugger;
class CallstackManager;
class MemoryAccessCounter;
class BreakpointManager;
class EmuSettings;
class Emulator;
class CodeDataLogger;
class SacConsole;
class SacCpu;
class SacMemoryManager;
class SacTraceLogger;
class SacEventManager;
class SacPpuTools;
class SacAssembler;

enum class MemoryOperationType;

//The 68000's debugger: breakpoints, stepping, the call stack, the trace log and the code/data
//log. The sound processor has its own (SacSoundDebugger).
class SacDebugger final : public IDebugger
{
	Debugger* _debugger = nullptr;
	Emulator* _emu = nullptr;
	SacConsole* _console = nullptr;
	SacCpu* _cpu = nullptr;
	SacMemoryManager* _memoryManager = nullptr;
	Disassembler* _disassembler = nullptr;
	MemoryAccessCounter* _memoryAccessCounter = nullptr;
	EmuSettings* _settings = nullptr;

	unique_ptr<SacEventManager> _eventManager;
	unique_ptr<CallstackManager> _callstackManager;
	unique_ptr<CodeDataLogger> _codeDataLogger;
	unique_ptr<BreakpointManager> _breakpointManager;
	unique_ptr<SacTraceLogger> _traceLogger;
	unique_ptr<SacPpuTools> _ppuTools;
	unique_ptr<SacAssembler> _assembler;

	uint16_t _prevOpCode = 0;
	uint32_t _prevStackPointer = 0;
	uint32_t _prevProgramCounter = 0;

	string _cdlFile;

	uint8_t GetPrevOpCodeSize();
	void ProcessCallStackUpdates(AddressInfo& destAddr, uint32_t destPc, uint32_t sp);
	AddressInfo GetAbsoluteAddress(uint32_t addr);

public:
	SacDebugger(Debugger* debugger);
	~SacDebugger();

	void Reset() override;

	void ProcessInstruction();
	template<uint8_t accessWidth> void ProcessRead(uint32_t addr, uint16_t value, MemoryOperationType type);
	template<uint8_t accessWidth> void ProcessWrite(uint32_t addr, uint16_t value, MemoryOperationType type);
	void ProcessInterrupt(uint32_t originalPc, uint32_t currentPc, bool forNmi) override;
	void ProcessPpuCycle();

	void Run() override;
	void Step(int32_t stepCount, StepType type) override;

	bool SaveRomToDisk(string filename, bool saveAsIps, CdlStripOption stripOption);

	void ProcessInputOverrides(DebugControllerState inputOverrides[8]) override;

	void SetProgramCounter(uint32_t addr, bool updateDebuggerOnly = false) override;
	uint32_t GetProgramCounter(bool getInstPc) override;
	uint64_t GetCpuCycleCount(bool forProfiler) override;
	void ResetPrevOpCode() override;

	DebuggerFeatures GetSupportedFeatures() override;

	BaseEventManager* GetEventManager() override;
	IAssembler* GetAssembler() override;
	CallstackManager* GetCallstackManager() override;
	BreakpointManager* GetBreakpointManager() override;
	ITraceLogger* GetTraceLogger() override;
	PpuTools* GetPpuTools() override;
	ISerializable* GetSerializableCpu() override;

	BaseState& GetState() override;
	void GetPpuState(BaseState& state) override;
	void ApplyState();
};
