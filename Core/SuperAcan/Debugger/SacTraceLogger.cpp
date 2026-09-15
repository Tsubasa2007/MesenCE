#include "pch.h"
#include "SuperAcan/Debugger/SacTraceLogger.h"
#include "SuperAcan/SacConsole.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/Debugger.h"
#include "Debugger/DebugTypes.h"
#include "Utilities/HexUtilities.h"

SacTraceLogger::SacTraceLogger(Debugger* debugger, IDebugger* cpuDebugger, SacConsole* console) : BaseTraceLogger(debugger, cpuDebugger, CpuType::Sac)
{
	_console = console;
}

//D0-D7 and A0-A7 use the generic register slots R0-R15
RowDataType SacTraceLogger::GetFormatTagType(string& tag)
{
	if(tag.size() == 2 && (tag[0] == 'D' || tag[0] == 'A') && tag[1] >= '0' && tag[1] <= '7') {
		return (RowDataType)((int)RowDataType::R0 + (tag[0] == 'A' ? 8 : 0) + (tag[1] - '0'));
	} else if(tag == "SR") {
		return RowDataType::SR;
	} else if(tag == "SP") {
		return RowDataType::SP;
	} else if(tag == "PS") {
		return RowDataType::PS;
	} else {
		return RowDataType::Text;
	}
}

void SacTraceLogger::GetTraceRow(string& output, SacCpuState& cpuState, TraceLogPpuState& ppuState, DisassemblyInfo& disassemblyInfo)
{
	//The condition codes: extend, negative, zero, overflow, carry
	constexpr char activeStatusLetters[5] = { 'X', 'N', 'Z', 'V', 'C' };
	constexpr char inactiveStatusLetters[5] = { 'x', 'n', 'z', 'v', 'c' };

	for(RowPart& rowPart : _rowParts) {
		int reg = (int)rowPart.DataType - (int)RowDataType::R0;
		if(reg >= 0 && reg < 16) {
			WriteIntValue(output, reg < 8 ? cpuState.D[reg] : cpuState.A[reg - 8], rowPart);
			continue;
		}

		switch(rowPart.DataType) {
			case RowDataType::SR: WriteIntValue(output, cpuState.SR, rowPart); break;
			case RowDataType::SP: WriteIntValue(output, cpuState.A[7], rowPart); break;
			case RowDataType::PS: GetStatusFlag(activeStatusLetters, inactiveStatusLetters, output, cpuState.SR & 0x1F, rowPart, 5); break;
			default: ProcessSharedTag(rowPart, output, cpuState, ppuState, disassemblyInfo); break;
		}
	}
}

void SacTraceLogger::LogPpuState()
{
	_ppuState[_currentPos] = {
		0,
		0,
		(int32_t)_console->GetScanline(),
		_console->GetFrameCount()
	};
}
