#include "pch.h"
#include "SuperAcan/Debugger/SacSoundAssembler.h"
#include "SuperAcan/Debugger/SacSoundDisUtils.h"
#include "Debugger/LabelManager.h"

SacSoundAssembler::SacSoundAssembler(LabelManager* labelManager) : Base6502Assembler<SacSoundAddrMode>(labelManager, CpuType::SacSound)
{
}

string SacSoundAssembler::GetOpName(uint8_t opcode)
{
	return SacSoundDisUtils::GetOpName(opcode);
}

SacSoundAddrMode SacSoundAssembler::GetOpMode(uint8_t opcode)
{
	return SacSoundDisUtils::GetOpMode(opcode);
}

//The undefined opcodes are all NOPs; $EA is the one to write for a plain NOP
bool SacSoundAssembler::IsOfficialOp(uint8_t opcode)
{
	return opcode == 0xEA || strcmp(SacSoundDisUtils::GetOpName(opcode), "NOP") != 0;
}

AssemblerSpecialCodes SacSoundAssembler::ResolveOpMode(AssemblerLineData& op, uint32_t instructionAddress, bool firstPass)
{
	AssemblerOperand& operand = op.Operands[0];
	AssemblerOperand& operand2 = op.Operands[1];
	if(operand.ByteCount > 2 || operand2.ByteCount > 2 || op.OperandCount > 2) {
		return AssemblerSpecialCodes::InvalidOperands;
	}

	if(operand2.Type == OperandType::Custom) {
		//BBR/BBS: a zero page address, then the branch target
		if(operand.IsImmediate || operand.HasParenOrBracket() || operand.ByteCount != 1 || !IsOpModeAvailable(op.OpCode, SacSoundAddrMode::ZpRel)) {
			return AssemblerSpecialCodes::InvalidOperands;
		}

		int32_t addressGap = (int32_t)operand2.Value - (int32_t)(instructionAddress + 3);
		if((addressGap > 127 || addressGap < -128) && !firstPass) {
			return AssemblerSpecialCodes::OutOfRangeJump;
		}
		operand2.ByteCount = 1;
		operand2.Value = (uint8_t)addressGap;
		op.AddrMode = SacSoundAddrMode::ZpRel;
	} else if(operand.IsImmediate) {
		if(operand.HasOpeningParenthesis || operand.ByteCount == 0 || op.OperandCount > 1) {
			return AssemblerSpecialCodes::InvalidOperands;
		} else if(operand.ByteCount > 1) {
			return AssemblerSpecialCodes::OperandOutOfRange;
		}
		op.AddrMode = SacSoundAddrMode::Imm;
	} else if(operand.HasOpeningParenthesis) {
		//(zp,X), (zp),Y, (zp), (abs) and (abs,X)
		if(operand2.Type == OperandType::X && operand2.HasClosingParenthesis) {
			if(operand.ByteCount == 2) {
				op.AddrMode = SacSoundAddrMode::AbsXInd;
			} else if(operand.ByteCount == 1) {
				op.AddrMode = SacSoundAddrMode::IndX;
			} else {
				return AssemblerSpecialCodes::InvalidOperands;
			}
		} else if(operand.HasClosingParenthesis && operand2.Type == OperandType::Y) {
			op.AddrMode = SacSoundAddrMode::IndY;
		} else if(operand.HasClosingParenthesis) {
			if(operand.ByteCount == 2) {
				op.AddrMode = SacSoundAddrMode::Ind;
			} else if(operand.ByteCount == 1) {
				op.AddrMode = SacSoundAddrMode::ZpInd;
			} else {
				return AssemblerSpecialCodes::InvalidOperands;
			}
		} else {
			return AssemblerSpecialCodes::InvalidOperands;
		}
	} else if(operand.HasParenOrBracket() || operand2.HasParenOrBracket()) {
		return AssemblerSpecialCodes::ParsingError;
	} else if(operand2.Type == OperandType::X) {
		if(operand.ByteCount == 2) {
			op.AddrMode = SacSoundAddrMode::AbsX;
		} else if(operand.ByteCount == 1) {
			AdjustOperandSize(op, operand, SacSoundAddrMode::ZpX, SacSoundAddrMode::AbsX);
		} else {
			return AssemblerSpecialCodes::InvalidOperands;
		}
	} else if(operand2.Type == OperandType::Y) {
		if(operand.ByteCount == 2) {
			op.AddrMode = SacSoundAddrMode::AbsY;
		} else if(operand.ByteCount == 1) {
			AdjustOperandSize(op, operand, SacSoundAddrMode::ZpY, SacSoundAddrMode::AbsY);
		} else {
			return AssemblerSpecialCodes::InvalidOperands;
		}
	} else if(operand.Type == OperandType::A) {
		op.AddrMode = SacSoundAddrMode::Acc;
	} else if(op.OperandCount == 0) {
		if(IsOpModeAvailable(op.OpCode, SacSoundAddrMode::Acc)) {
			op.AddrMode = SacSoundAddrMode::Acc;
		} else if(IsOpModeAvailable(op.OpCode, SacSoundAddrMode::Imp)) {
			op.AddrMode = SacSoundAddrMode::Imp;
		} else if(IsOpModeAvailable(op.OpCode, SacSoundAddrMode::Imm)) {
			//BRK is followed by a signature byte, 0 when none is given
			op.AddrMode = SacSoundAddrMode::Imm;
			operand.Type = OperandType::Custom;
			operand.Value = 0;
			operand.ByteCount = 1;
			op.OperandCount = 1;
		} else {
			return AssemblerSpecialCodes::InvalidOperands;
		}
	} else if(op.OperandCount == 1) {
		if(IsOpModeAvailable(op.OpCode, SacSoundAddrMode::Rel)) {
			//The operand is the target; the instruction holds the distance to it
			int32_t addressGap = (int32_t)operand.Value - (int32_t)(instructionAddress + 2);
			if((addressGap > 127 || addressGap < -128) && !firstPass) {
				return AssemblerSpecialCodes::OutOfRangeJump;
			}
			op.AddrMode = SacSoundAddrMode::Rel;
			operand.ByteCount = 1;
			operand.Value = (uint8_t)addressGap;
		} else if(operand.ByteCount == 2) {
			op.AddrMode = SacSoundAddrMode::Abs;
		} else if(operand.ByteCount == 1) {
			AdjustOperandSize(op, operand, SacSoundAddrMode::Zp, SacSoundAddrMode::Abs);
		} else {
			return AssemblerSpecialCodes::InvalidOperands;
		}
	} else {
		return AssemblerSpecialCodes::InvalidOperands;
	}

	return AssemblerSpecialCodes::OK;
}
