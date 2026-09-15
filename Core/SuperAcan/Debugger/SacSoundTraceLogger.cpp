#include "pch.h"
#include "SuperAcan/Debugger/SacSoundTraceLogger.h"
#include "SuperAcan/SacConsole.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/Debugger.h"
#include "Debugger/DebugTypes.h"

SacSoundTraceLogger::SacSoundTraceLogger(Debugger* debugger, IDebugger* cpuDebugger, SacConsole* console) : BaseTraceLogger(debugger, cpuDebugger, CpuType::SacSound)
{
	_console = console;
}

RowDataType SacSoundTraceLogger::GetFormatTagType(string& tag)
{
	if(tag == "A") {
		return RowDataType::A;
	} else if(tag == "X") {
		return RowDataType::X;
	} else if(tag == "Y") {
		return RowDataType::Y;
	} else if(tag == "P") {
		return RowDataType::PS;
	} else if(tag == "SP") {
		return RowDataType::SP;
	} else {
		return RowDataType::Text;
	}
}

void SacSoundTraceLogger::GetTraceRow(string& output, SacSoundCpuState& cpuState, TraceLogPpuState& ppuState, DisassemblyInfo& disassemblyInfo)
{
	constexpr char activeStatusLetters[8] = { 'N', 'V', '-', 'B', 'D', 'I', 'Z', 'C' };
	constexpr char inactiveStatusLetters[8] = { 'n', 'v', '-', 'b', 'd', 'i', 'z', 'c' };

	for(RowPart& rowPart : _rowParts) {
		switch(rowPart.DataType) {
			case RowDataType::A: WriteIntValue(output, cpuState.A, rowPart); break;
			case RowDataType::X: WriteIntValue(output, cpuState.X, rowPart); break;
			case RowDataType::Y: WriteIntValue(output, cpuState.Y, rowPart); break;
			case RowDataType::SP: WriteIntValue(output, cpuState.SP, rowPart); break;
			case RowDataType::PS: GetStatusFlag(activeStatusLetters, inactiveStatusLetters, output, cpuState.PS, rowPart); break;
			default: ProcessSharedTag(rowPart, output, cpuState, ppuState, disassemblyInfo); break;
		}
	}
}

//The sound processor runs each line after the 68000, so only the line itself is meaningful
void SacSoundTraceLogger::LogPpuState()
{
	_ppuState[_currentPos] = {
		0,
		0,
		(int32_t)_console->GetScanline(),
		_console->GetFrameCount()
	};
}
