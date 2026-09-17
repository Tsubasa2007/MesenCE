#include "pch.h"
#include "SuperAcan/Debugger/SacSoundDisUtils.h"
#include "SuperAcan/SacConsole.h"
#include "SuperAcan/SacMemoryManager.h"
#include "SuperAcan/SacTypes.h"
#include "Shared/EmuSettings.h"
#include "Shared/MemoryType.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/LabelManager.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/FastString.h"

static constexpr uint8_t _opSize[16] = {
	1, 1, 2, 2, 2, 2, 2, 2, 2, 2,
	3, 3, 3, 3, 3, 3
};

// clang-format off
static constexpr const char* _opName[256] = {
//	0      1      2      3      4      5      6      7       8      9      A      B      C      D      E      F
	"BRK", "ORA", "NOP", "NOP", "TSB", "ORA", "ASL", "RMB0", "PHP", "ORA", "ASL", "NOP", "TSB", "ORA", "ASL", "BBR0", //0
	"BPL", "ORA", "ORA", "NOP", "TRB", "ORA", "ASL", "RMB1", "CLC", "ORA", "INC", "NOP", "TRB", "ORA", "ASL", "BBR1", //1
	"JSR", "AND", "NOP", "NOP", "BIT", "AND", "ROL", "RMB2", "PLP", "AND", "ROL", "NOP", "BIT", "AND", "ROL", "BBR2", //2
	"BMI", "AND", "AND", "NOP", "BIT", "AND", "ROL", "RMB3", "SEC", "AND", "DEC", "NOP", "BIT", "AND", "ROL", "BBR3", //3
	"RTI", "EOR", "NOP", "NOP", "NOP", "EOR", "LSR", "RMB4", "PHA", "EOR", "LSR", "NOP", "JMP", "EOR", "LSR", "BBR4", //4
	"BVC", "EOR", "EOR", "NOP", "NOP", "EOR", "LSR", "RMB5", "CLI", "EOR", "PHY", "NOP", "NOP", "EOR", "LSR", "BBR5", //5
	"RTS", "ADC", "NOP", "NOP", "STZ", "ADC", "ROR", "RMB6", "PLA", "ADC", "ROR", "NOP", "JMP", "ADC", "ROR", "BBR6", //6
	"BVS", "ADC", "ADC", "NOP", "STZ", "ADC", "ROR", "RMB7", "SEI", "ADC", "PLY", "NOP", "JMP", "ADC", "ROR", "BBR7", //7
	"BRA", "STA", "NOP", "NOP", "STY", "STA", "STX", "SMB0", "DEY", "BIT", "TXA", "NOP", "STY", "STA", "STX", "BBS0", //8
	"BCC", "STA", "STA", "NOP", "STY", "STA", "STX", "SMB1", "TYA", "STA", "TXS", "NOP", "STZ", "STA", "STZ", "BBS1", //9
	"LDY", "LDA", "LDX", "NOP", "LDY", "LDA", "LDX", "SMB2", "TAY", "LDA", "TAX", "NOP", "LDY", "LDA", "LDX", "BBS2", //A
	"BCS", "LDA", "LDA", "NOP", "LDY", "LDA", "LDX", "SMB3", "CLV", "LDA", "TSX", "NOP", "LDY", "LDA", "LDX", "BBS3", //B
	"CPY", "CMP", "NOP", "NOP", "CPY", "CMP", "DEC", "SMB4", "INY", "CMP", "DEX", "WAI", "CPY", "CMP", "DEC", "BBS4", //C
	"BNE", "CMP", "CMP", "NOP", "NOP", "CMP", "DEC", "SMB5", "CLD", "CMP", "PHX", "STP", "NOP", "CMP", "DEC", "BBS5", //D
	"CPX", "SBC", "NOP", "NOP", "CPX", "SBC", "INC", "SMB6", "INX", "SBC", "NOP", "NOP", "CPX", "SBC", "INC", "BBS6", //E
	"BEQ", "SBC", "SBC", "NOP", "NOP", "SBC", "INC", "SMB7", "SED", "SBC", "PLX", "NOP", "NOP", "SBC", "INC", "BBS7"  //F
};

typedef SacSoundAddrMode M;
static constexpr SacSoundAddrMode _opMode[256] = {
//	0       1        2         3       4       5       6       7       8       9        A       B       C           D        E        F
	M::Imm, M::IndX, M::Imm,   M::Imp, M::Zp,  M::Zp,  M::Zp,  M::Zp,  M::Imp, M::Imm,  M::Acc, M::Imp, M::Abs,     M::Abs,  M::Abs,  M::ZpRel, //0
	M::Rel, M::IndY, M::ZpInd, M::Imp, M::Zp,  M::ZpX, M::ZpX, M::Zp,  M::Imp, M::AbsY, M::Acc, M::Imp, M::Abs,     M::AbsX, M::AbsX, M::ZpRel, //1
	M::Abs, M::IndX, M::Imm,   M::Imp, M::Zp,  M::Zp,  M::Zp,  M::Zp,  M::Imp, M::Imm,  M::Acc, M::Imp, M::Abs,     M::Abs,  M::Abs,  M::ZpRel, //2
	M::Rel, M::IndY, M::ZpInd, M::Imp, M::ZpX, M::ZpX, M::ZpX, M::Zp,  M::Imp, M::AbsY, M::Acc, M::Imp, M::AbsX,    M::AbsX, M::AbsX, M::ZpRel, //3
	M::Imp, M::IndX, M::Imm,   M::Imp, M::Zp,  M::Zp,  M::Zp,  M::Zp,  M::Imp, M::Imm,  M::Acc, M::Imp, M::Abs,     M::Abs,  M::Abs,  M::ZpRel, //4
	M::Rel, M::IndY, M::ZpInd, M::Imp, M::ZpX, M::ZpX, M::ZpX, M::Zp,  M::Imp, M::AbsY, M::Imp, M::Imp, M::Abs,     M::AbsX, M::AbsX, M::ZpRel, //5
	M::Imp, M::IndX, M::Imm,   M::Imp, M::Zp,  M::Zp,  M::Zp,  M::Zp,  M::Imp, M::Imm,  M::Acc, M::Imp, M::Ind,     M::Abs,  M::Abs,  M::ZpRel, //6
	M::Rel, M::IndY, M::ZpInd, M::Imp, M::ZpX, M::ZpX, M::ZpX, M::Zp,  M::Imp, M::AbsY, M::Imp, M::Imp, M::AbsXInd, M::AbsX, M::AbsX, M::ZpRel, //7
	M::Rel, M::IndX, M::Imm,   M::Imp, M::Zp,  M::Zp,  M::Zp,  M::Zp,  M::Imp, M::Imm,  M::Imp, M::Imp, M::Abs,     M::Abs,  M::Abs,  M::ZpRel, //8
	M::Rel, M::IndY, M::ZpInd, M::Imp, M::ZpX, M::ZpX, M::ZpY, M::Zp,  M::Imp, M::AbsY, M::Imp, M::Imp, M::Abs,     M::AbsX, M::AbsX, M::ZpRel, //9
	M::Imm, M::IndX, M::Imm,   M::Imp, M::Zp,  M::Zp,  M::Zp,  M::Zp,  M::Imp, M::Imm,  M::Imp, M::Imp, M::Abs,     M::Abs,  M::Abs,  M::ZpRel, //A
	M::Rel, M::IndY, M::ZpInd, M::Imp, M::ZpX, M::ZpX, M::ZpY, M::Zp,  M::Imp, M::AbsY, M::Imp, M::Imp, M::AbsX,    M::AbsX, M::AbsY, M::ZpRel, //B
	M::Imm, M::IndX, M::Imm,   M::Imp, M::Zp,  M::Zp,  M::Zp,  M::Zp,  M::Imp, M::Imm,  M::Imp, M::Imp, M::Abs,     M::Abs,  M::Abs,  M::ZpRel, //C
	M::Rel, M::IndY, M::ZpInd, M::Imp, M::ZpX, M::ZpX, M::ZpX, M::Zp,  M::Imp, M::AbsY, M::Imp, M::Imp, M::Abs,     M::AbsX, M::AbsX, M::ZpRel, //D
	M::Imm, M::IndX, M::Imm,   M::Imp, M::Zp,  M::Zp,  M::Zp,  M::Zp,  M::Imp, M::Imm,  M::Imp, M::Imp, M::Abs,     M::Abs,  M::Abs,  M::ZpRel, //E
	M::Rel, M::IndY, M::ZpInd, M::Imp, M::ZpX, M::ZpX, M::ZpX, M::Zp,  M::Imp, M::AbsY, M::Imp, M::Imp, M::Abs,     M::AbsX, M::AbsX, M::ZpRel  //F
};
// clang-format on

void SacSoundDisUtils::GetDisassembly(DisassemblyInfo& info, string& out, uint32_t memoryAddr, LabelManager* labelManager, EmuSettings* settings)
{
	FastString str(settings->GetDebugConfig().UseLowerCaseDisassembly);

	uint8_t opCode = info.GetOpCode();
	uint8_t* byteCode = info.GetByteCode();
	SacSoundAddrMode addrMode = _opMode[opCode];
	uint16_t word = (uint16_t)(byteCode[1] | (byteCode[2] << 8));

	str.Write(_opName[opCode]);
	if(addrMode != SacSoundAddrMode::Imp) {
		str.Write(' ');
	}

	auto writeLabelOrAddr = [&str, &info, labelManager](uint16_t addr) {
		AddressInfo address { addr, MemoryType::SacSoundMemory };
		string label = labelManager ? labelManager->GetLabel(address, !info.IsJump()) : "";
		if(label.empty()) {
			str.WriteAll('$', HexUtilities::ToHex(addr));
		} else {
			str.Write(label, true);
		}
	};

	auto writeZp = [&str](uint8_t zp) {
		str.WriteAll('$', HexUtilities::ToHex(zp));
	};

	switch(addrMode) {
		case SacSoundAddrMode::Imp: break;
		case SacSoundAddrMode::Acc: str.Write('A'); break;
		case SacSoundAddrMode::Imm: str.WriteAll("#$", HexUtilities::ToHex(byteCode[1])); break;

		case SacSoundAddrMode::Zp: writeZp(byteCode[1]); break;
		case SacSoundAddrMode::ZpX: writeZp(byteCode[1]); str.Write(",X"); break;
		case SacSoundAddrMode::ZpY: writeZp(byteCode[1]); str.Write(",Y"); break;

		case SacSoundAddrMode::IndX: str.Write('('); writeZp(byteCode[1]); str.Write(",X)"); break;
		case SacSoundAddrMode::IndY: str.Write('('); writeZp(byteCode[1]); str.Write("),Y"); break;
		case SacSoundAddrMode::ZpInd: str.Write('('); writeZp(byteCode[1]); str.Write(')'); break;

		case SacSoundAddrMode::Rel: writeLabelOrAddr((uint16_t)((int8_t)byteCode[1] + memoryAddr + 2)); break;

		case SacSoundAddrMode::Abs: writeLabelOrAddr(word); break;
		case SacSoundAddrMode::AbsX: writeLabelOrAddr(word); str.Write(",X"); break;
		case SacSoundAddrMode::AbsY: writeLabelOrAddr(word); str.Write(",Y"); break;
		case SacSoundAddrMode::Ind: str.Write('('); writeLabelOrAddr(word); str.Write(')'); break;
		case SacSoundAddrMode::AbsXInd: str.Write('('); writeLabelOrAddr(word); str.Write(",X)"); break;

		case SacSoundAddrMode::ZpRel:
			writeZp(byteCode[1]);
			str.Write(',');
			writeLabelOrAddr((uint16_t)((int8_t)byteCode[2] + memoryAddr + 3));
			break;
	}

	out += str.ToString();
}

//Where a data access goes, worked out from the registers and memory as they stand (pointers in
//zero page wrap within it). Only shown for the modes whose operand does not already say it.
EffectiveAddressInfo SacSoundDisUtils::GetEffectiveAddress(DisassemblyInfo& info, SacConsole* console, SacSoundCpuState& state)
{
	uint8_t opCode = info.GetOpCode();
	if(IsUnconditionalJump(opCode) || IsConditionalJump(opCode) || strcmp(_opName[opCode], "NOP") == 0) {
		return {};
	}

	SacMemoryManager* memoryManager = console->GetMemoryManager();
	auto readZpWord = [memoryManager](uint8_t ptr) {
		return (uint16_t)(memoryManager->SoundDebugRead(ptr) | (memoryManager->SoundDebugRead((uint8_t)(ptr + 1)) << 8));
	};

	uint8_t* byteCode = info.GetByteCode();
	uint8_t zp = byteCode[1];
	uint16_t word = (uint16_t)(byteCode[1] | (byteCode[2] << 8));

	switch(_opMode[opCode]) {
		case SacSoundAddrMode::Zp: return EffectiveAddressInfo(zp, 1, true);
		case SacSoundAddrMode::ZpX: return EffectiveAddressInfo((uint8_t)(zp + state.X), 1, true);
		case SacSoundAddrMode::ZpY: return EffectiveAddressInfo((uint8_t)(zp + state.Y), 1, true);
		case SacSoundAddrMode::IndX: return EffectiveAddressInfo(readZpWord((uint8_t)(zp + state.X)), 1, true);
		case SacSoundAddrMode::IndY: return EffectiveAddressInfo((uint16_t)(readZpWord(zp) + state.Y), 1, true);
		case SacSoundAddrMode::ZpInd: return EffectiveAddressInfo(readZpWord(zp), 1, true);
		case SacSoundAddrMode::Abs: return EffectiveAddressInfo(word, 1, false);
		case SacSoundAddrMode::AbsX: return EffectiveAddressInfo((uint16_t)(word + state.X), 1, true);
		case SacSoundAddrMode::AbsY: return EffectiveAddressInfo((uint16_t)(word + state.Y), 1, true);
		default: return {};
	}
}

const char* SacSoundDisUtils::GetOpName(uint8_t opCode)
{
	return _opName[opCode];
}

SacSoundAddrMode SacSoundDisUtils::GetOpMode(uint8_t opCode)
{
	return _opMode[opCode];
}

uint8_t SacSoundDisUtils::GetOpSize(uint8_t opCode)
{
	return _opSize[(int)_opMode[opCode]];
}

bool SacSoundDisUtils::IsJumpToSub(uint8_t opCode)
{
	return opCode == 0x20 || opCode == 0x00; //JSR, BRK
}

bool SacSoundDisUtils::IsReturnInstruction(uint8_t opCode)
{
	return opCode == 0x60 || opCode == 0x40; //RTS, RTI
}

bool SacSoundDisUtils::IsUnconditionalJump(uint8_t opCode)
{
	switch(opCode) {
		case 0x20: //JSR
		case 0x40: //RTI
		case 0x4C: //JMP abs
		case 0x60: //RTS
		case 0x6C: //JMP (abs)
		case 0x7C: //JMP (abs,X)
		case 0x80: //BRA
			return true;

		default:
			return false;
	}
}

//The eight flag branches, and BBR/BBS in column F
bool SacSoundDisUtils::IsConditionalJump(uint8_t opCode)
{
	return (opCode & 0x1F) == 0x10 || (opCode & 0x0F) == 0x0F;
}
