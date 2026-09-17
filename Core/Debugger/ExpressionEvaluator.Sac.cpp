#include "pch.h"
#include "Debugger/ExpressionEvaluator.h"
#include "Debugger/Debugger.h"
#include "SuperAcan/Debugger/SacDebugger.h"
#include "SuperAcan/Debugger/SacSoundDebugger.h"
#include "SuperAcan/SacTypes.h"
#include "SuperAcan/SacConsole.h"
#include "SuperAcan/SacCpu.h"

//D0-D7 and A0-A7 use the generic register slots R0-R15
unordered_map<string, int64_t>& ExpressionEvaluator::GetSacTokens()
{
	static unordered_map<string, int64_t> supportedTokens = {
		{ "d0", EvalValues::R0 },
		{ "d1", EvalValues::R1 },
		{ "d2", EvalValues::R2 },
		{ "d3", EvalValues::R3 },
		{ "d4", EvalValues::R4 },
		{ "d5", EvalValues::R5 },
		{ "d6", EvalValues::R6 },
		{ "d7", EvalValues::R7 },
		{ "a0", EvalValues::R8 },
		{ "a1", EvalValues::R9 },
		{ "a2", EvalValues::R10 },
		{ "a3", EvalValues::R11 },
		{ "a4", EvalValues::R12 },
		{ "a5", EvalValues::R13 },
		{ "a6", EvalValues::R14 },
		{ "a7", EvalValues::R15 },

		{ "sp", EvalValues::RegSP },
		{ "sr", EvalValues::RegSR },
		{ "pc", EvalValues::RegPC },

		{ "frame", EvalValues::PpuFrameCount },
		{ "scanline", EvalValues::PpuScanline },
	};

	return supportedTokens;
}

int64_t ExpressionEvaluator::GetSacTokenValue(int64_t token, EvalResultType& resultType)
{
	SacCpuState& s = (SacCpuState&)((SacDebugger*)_cpuDebugger)->GetState();
	SacConsole* console = (SacConsole*)_debugger->GetConsole();

	if(token >= EvalValues::R0 && token <= EvalValues::R7) {
		return s.D[token - EvalValues::R0];
	} else if(token >= EvalValues::R8 && token <= EvalValues::R15) {
		return s.A[token - EvalValues::R8];
	}

	switch(token) {
		case EvalValues::RegSP: return s.A[7];
		case EvalValues::RegSR: return s.SR;
		case EvalValues::RegPC: return s.PC;

		case EvalValues::PpuFrameCount: return console->GetFrameCount();
		case EvalValues::PpuScanline: return console->GetScanline();

		default: return 0;
	}
}

//The sound processor, a 65C02
unordered_map<string, int64_t>& ExpressionEvaluator::GetSacSoundTokens()
{
	static unordered_map<string, int64_t> supportedTokens = {
		{ "a", EvalValues::RegA },
		{ "x", EvalValues::RegX },
		{ "y", EvalValues::RegY },
		{ "ps", EvalValues::RegPS },
		{ "sp", EvalValues::RegSP },
		{ "pc", EvalValues::RegPC },

		{ "frame", EvalValues::PpuFrameCount },
		{ "scanline", EvalValues::PpuScanline },
	};

	return supportedTokens;
}

int64_t ExpressionEvaluator::GetSacSoundTokenValue(int64_t token, EvalResultType& resultType)
{
	SacSoundCpuState& s = (SacSoundCpuState&)((SacSoundDebugger*)_cpuDebugger)->GetState();
	SacConsole* console = (SacConsole*)_debugger->GetConsole();

	switch(token) {
		case EvalValues::RegA: return s.A;
		case EvalValues::RegX: return s.X;
		case EvalValues::RegY: return s.Y;
		case EvalValues::RegPS: return s.PS;
		case EvalValues::RegSP: return s.SP;
		case EvalValues::RegPC: return s.PC;

		case EvalValues::PpuFrameCount: return console->GetFrameCount();
		case EvalValues::PpuScanline: return console->GetScanline();

		default: return 0;
	}
}
