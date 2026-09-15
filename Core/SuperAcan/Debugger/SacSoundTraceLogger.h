#pragma once
#include "pch.h"
#include "Debugger/BaseTraceLogger.h"
#include "SuperAcan/SacTypes.h"

class DisassemblyInfo;
class Debugger;
class SacConsole;

class SacSoundTraceLogger : public BaseTraceLogger<SacSoundTraceLogger, SacSoundCpuState>
{
private:
	SacConsole* _console = nullptr;

protected:
	RowDataType GetFormatTagType(string& tag) override;

public:
	SacSoundTraceLogger(Debugger* debugger, IDebugger* cpuDebugger, SacConsole* console);

	void GetTraceRow(string& output, SacSoundCpuState& cpuState, TraceLogPpuState& ppuState, DisassemblyInfo& disassemblyInfo);
	void LogPpuState();

	__forceinline uint32_t GetProgramCounter(SacSoundCpuState& state) { return state.PC; }
	__forceinline uint64_t GetCycleCount(SacSoundCpuState& state) { return state.CycleCount; }
	__forceinline uint8_t GetStackPointer(SacSoundCpuState& state) { return state.SP; }
};
