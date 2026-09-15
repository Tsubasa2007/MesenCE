#pragma once
#include "pch.h"
#include "Debugger/IAssembler.h"

class LabelManager;

//The 68000's assembler. It reads the syntax the disassembler writes (Moira's) and finds each
//instruction's encoding by working back from the disassembler: every opcode word is disassembled
//once and indexed by its mnemonic and the shape of its operands (modes and registers, not
//values), and a line's candidates are completed with extension words and kept only if their
//disassembly gives back the same operands. What it writes therefore always disassembles to what
//was typed.
class SacAssembler final : public IAssembler
{
private:
	LabelManager* _labelManager = nullptr;

public:
	SacAssembler(LabelManager* labelManager) : _labelManager(labelManager) {}

	uint32_t AssembleCode(string code, uint32_t startAddress, int16_t* assembledCode) override;
};
