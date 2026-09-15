#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"

class DisassemblyInfo;
class LabelManager;
class EmuSettings;
class SacConsole;
struct SacSoundCpuState;

enum class SacSoundAddrMode : uint8_t
{
	Imp, Acc, Imm, Zp, ZpX, ZpY, IndX, IndY, ZpInd, Rel,
	Abs, AbsX, AbsY, Ind, AbsXInd, ZpRel
};

//The sound processor's instructions for the debugger: the 65C02 set with the Rockwell bit
//instructions and WAI/STP, and every undefined opcode shown as the NOP W65C02 runs it as
class SacSoundDisUtils
{
public:
	static void GetDisassembly(DisassemblyInfo& info, string& out, uint32_t memoryAddr, LabelManager* labelManager, EmuSettings* settings);
	static const char* GetOpName(uint8_t opCode);
	static SacSoundAddrMode GetOpMode(uint8_t opCode);
	static EffectiveAddressInfo GetEffectiveAddress(DisassemblyInfo& info, SacConsole* console, SacSoundCpuState& state);

	static uint8_t GetOpSize(uint8_t opCode);
	static bool IsJumpToSub(uint8_t opCode);
	static bool IsReturnInstruction(uint8_t opCode);
	static bool IsUnconditionalJump(uint8_t opCode);
	static bool IsConditionalJump(uint8_t opCode);
};
