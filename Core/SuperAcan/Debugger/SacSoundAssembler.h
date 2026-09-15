#pragma once
#include "pch.h"
#include "Debugger/Base6502Assembler.h"
#include "SuperAcan/Debugger/SacSoundDisUtils.h"

class LabelManager;

//The sound processor's assembler: the 65C02 with the Rockwell bit instructions, on the shared
//6502 assembler. Branch operands (and BBR/BBS's second one) are the target address.
class SacSoundAssembler final : public Base6502Assembler<SacSoundAddrMode>
{
private:
	string GetOpName(uint8_t opcode) override;
	SacSoundAddrMode GetOpMode(uint8_t opcode) override;
	bool IsOfficialOp(uint8_t opcode) override;
	AssemblerSpecialCodes ResolveOpMode(AssemblerLineData& op, uint32_t instructionAddress, bool firstPass) override;

public:
	SacSoundAssembler(LabelManager* labelManager);
	virtual ~SacSoundAssembler() = default;
};
