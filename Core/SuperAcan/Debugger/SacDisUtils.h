#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"

class DisassemblyInfo;
class LabelManager;
class EmuSettings;
class MemoryDumper;
class SacConsole;
struct SacCpuState;
struct EffectiveAddressInfo;

//68000 instructions for the debugger. Text comes from Moira's disassembler (see SacCpu), and
//an instruction can be up to 10 bytes where the debugger keeps 8, so the rest is read from
//memory through the memory dumper the running debugger provides.
class SacDisUtils
{
private:
	static MemoryDumper* _memoryDumper;

public:
	static void SetMemoryDumper(MemoryDumper* memoryDumper) { _memoryDumper = memoryDumper; }

	static void GetDisassembly(DisassemblyInfo& info, string& out, uint32_t memoryAddr, LabelManager* labelManager, EmuSettings* settings);
	static uint8_t GetOpSize(uint32_t cpuAddress, MemoryType memType, MemoryDumper* memoryDumper);
	static EffectiveAddressInfo GetEffectiveAddress(DisassemblyInfo& info, SacConsole* console, SacCpuState& state);

	static bool IsJumpToSub(uint16_t opCode);
	static bool IsReturnInstruction(uint16_t opCode);
	static bool IsUnconditionalJump(uint16_t opCode);
	static bool IsConditionalJump(uint16_t opCode);
	static CdlFlags::CdlFlags GetOpFlags(uint16_t prevOpCode, uint32_t pc, uint32_t prevPc, uint8_t prevOpSize);
};
