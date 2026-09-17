#pragma once
#include "pch.h"
#include "Debugger/BaseTraceLogger.h"
#include "SuperAcan/SacCpu.h"

class DisassemblyInfo;
class Debugger;
class SacConsole;

class SacTraceLogger : public BaseTraceLogger<SacTraceLogger, SacCpuState>
{
private:
	SacConsole* _console = nullptr;

protected:
	RowDataType GetFormatTagType(string& tag) override;

public:
	SacTraceLogger(Debugger* debugger, IDebugger* cpuDebugger, SacConsole* console);

	void GetTraceRow(string& output, SacCpuState& cpuState, TraceLogPpuState& ppuState, DisassemblyInfo& disassemblyInfo);
	void LogPpuState();

	__forceinline uint32_t GetProgramCounter(SacCpuState& state) { return state.PC; }
	__forceinline uint64_t GetCycleCount(SacCpuState& state) { return state.CycleCount; }
	__forceinline uint8_t GetStackPointer(SacCpuState& state) { return (uint8_t)state.A[7]; }
};
