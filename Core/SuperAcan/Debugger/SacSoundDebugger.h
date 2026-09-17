#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/IDebugger.h"
#include "SuperAcan/SacTypes.h"

class Disassembler;
class Debugger;
class CallstackManager;
class MemoryAccessCounter;
class BreakpointManager;
class SacConsole;
class SacMemoryManager;
class SacSoundTraceLogger;
class SacSoundAssembler;

enum class MemoryOperationType;

//The sound processor's debugger: breakpoints, stepping, the call stack and the trace log. Its
//registers live in the W65C02 core; the debugger works on a copy of them, refreshed whenever it
//is asked for and written back by ApplyState when they are edited.
class SacSoundDebugger final : public IDebugger
{
	Debugger* _debugger = nullptr;
	Disassembler* _disassembler = nullptr;
	MemoryAccessCounter* _memoryAccessCounter = nullptr;
	SacConsole* _console = nullptr;
	SacMemoryManager* _memoryManager = nullptr;

	unique_ptr<CallstackManager> _callstackManager;
	unique_ptr<BreakpointManager> _breakpointManager;
	unique_ptr<SacSoundTraceLogger> _traceLogger;
	unique_ptr<SacSoundAssembler> _assembler;

	SacSoundCpuState _state = {};

	uint8_t _prevOpCode = 0xEA;
	uint8_t _prevStackPointer = 0;
	uint16_t _prevProgramCounter = 0;

	SacSoundCpuState& UpdateState();
	void ProcessCallStackUpdates(AddressInfo& destAddr, uint16_t destPc, uint8_t sp);

public:
	SacSoundDebugger(Debugger* debugger);
	~SacSoundDebugger();

	void Reset() override;

	void ProcessInstruction();
	void ProcessRead(uint32_t addr, uint8_t value, MemoryOperationType type);
	void ProcessWrite(uint32_t addr, uint8_t value, MemoryOperationType type);
	void ProcessInterrupt(uint32_t originalPc, uint32_t currentPc, bool forNmi) override;

	void Run() override;
	void Step(int32_t stepCount, StepType type) override;

	DebuggerFeatures GetSupportedFeatures() override;
	void SetProgramCounter(uint32_t addr, bool updateDebuggerOnly = false) override;
	uint32_t GetProgramCounter(bool getInstPc) override;
	uint64_t GetCpuCycleCount(bool forProfiler) override;
	void ResetPrevOpCode() override;

	CallstackManager* GetCallstackManager() override;
	BreakpointManager* GetBreakpointManager() override;
	IAssembler* GetAssembler() override;
	BaseEventManager* GetEventManager() override;
	ITraceLogger* GetTraceLogger() override;

	BaseState& GetState() override;
	void ApplyState();
};
