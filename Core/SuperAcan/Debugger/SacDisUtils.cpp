#include "pch.h"
#include "SuperAcan/Debugger/SacDisUtils.h"
#include "SuperAcan/SacCpu.h"
#include "SuperAcan/SacConsole.h"
#include "SuperAcan/SacMemoryManager.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/MemoryDumper.h"

MemoryDumper* SacDisUtils::_memoryDumper = nullptr;

void SacDisUtils::GetDisassembly(DisassemblyInfo& info, string& out, uint32_t memoryAddr, LabelManager* labelManager, EmuSettings* settings)
{
	uint8_t bytes[16] = {};
	uint8_t size = info.GetOpSize();
	memcpy(bytes, info.GetByteCode(), std::min<int>(size, 8));
	if(size > 8 && _memoryDumper) {
		for(int i = 8; i < size; i++) {
			bytes[i] = _memoryDumper->GetMemoryValue(MemoryType::SacMemory, memoryAddr + i);
		}
	}
	SacCpu::DisassembleBytes(memoryAddr, bytes, sizeof(bytes), out);
}

uint8_t SacDisUtils::GetOpSize(uint32_t cpuAddress, MemoryType memType, MemoryDumper* memoryDumper)
{
	uint8_t bytes[16];
	for(int i = 0; i < 16; i++) {
		bytes[i] = memoryDumper->GetMemoryValue(memType, cpuAddress + i);
	}
	string text;
	int size = SacCpu::DisassembleBytes(cpuAddress, bytes, sizeof(bytes), text);
	return (uint8_t)std::clamp(size, 2, 10);
}

//Bytes of extension words that follow the opcode for an effective address
static uint32_t GetExtensionSize(SacAddrMode mode, uint8_t size)
{
	switch(mode) {
		case SacAddrMode::Disp: case SacAddrMode::Index: case SacAddrMode::AbsShort:
		case SacAddrMode::PcDisp: case SacAddrMode::PcIndex:
			return 2;

		case SacAddrMode::AbsLong: return 4;
		case SacAddrMode::Imm: return size == 4 ? 4 : 2;
		default: return 0;
	}
}

//Where an instruction's data access goes, worked out from the registers and memory as they
//stand (addresses are 24 bits). MOVE has two effective addresses: the source is shown unless it
//is a register or an immediate, then the destination is. Branches are left out since their
//operand is the target, and LEA, PEA, JMP and JSR show the address they compute with no value,
//as they read nothing there. Absolute addresses are in the operand already, so only their value
//is shown.
EffectiveAddressInfo SacDisUtils::GetEffectiveAddress(DisassemblyInfo& info, SacConsole* console, SacCpuState& state)
{
	//The instruction is read as the 68000 sees it, which also covers the part of a 10-byte
	//instruction the debugger does not keep
	SacMemoryManager* memoryManager = console->GetMemoryManager();
	uint32_t pc = state.PC & 0xFFFFFF;
	auto readWord = [=](uint32_t offset) { return memoryManager->DebugRead16(pc + offset); };

	uint16_t opCode = readWord(0);
	if((opCode & 0xF000) == 0x6000 || (opCode & 0xF0F8) == 0x50C8) {
		//Bcc, BRA, BSR and DBcc
		return {};
	}

	SacAddrMode mode;
	uint8_t size;
	SacCpu::GetOpInfo(opCode, mode, size);
	uint8_t reg = opCode & 0x07;

	//Operands that sit between the opcode and the effective address's extension words
	uint32_t extOffset = 2;
	if((opCode & 0xF100) == 0x0000) {
		if((opCode & 0x0F00) == 0x0800) {
			extOffset += 2; //BTST, BCHG, BCLR and BSET with a bit number
		} else {
			extOffset += size == 4 ? 4 : 2; //ORI, ANDI, SUBI, ADDI, EORI and CMPI
		}
	} else if((opCode & 0xFB80) == 0x4880) {
		extOffset += 2; //MOVEM's register mask
	}

	bool isMove = (opCode & 0xC000) == 0 && (opCode & 0x3000) != 0;
	if(isMove && (mode <= SacAddrMode::An || mode == SacAddrMode::Imm)) {
		extOffset += GetExtensionSize(mode, size);
		uint8_t dstMode = (opCode >> 6) & 0x07;
		reg = (opCode >> 9) & 0x07;
		if(dstMode < 7) {
			mode = (SacAddrMode)dstMode;
		} else {
			mode = reg == 0 ? SacAddrMode::AbsShort : (reg == 1 ? SacAddrMode::AbsLong : SacAddrMode::None);
		}
	}

	auto indexed = [&state](uint32_t base, uint16_t ext) {
		uint8_t xn = (ext >> 12) & 0x07;
		uint32_t index = (ext & 0x8000) ? state.A[xn] : state.D[xn];
		if(!(ext & 0x0800)) {
			index = (uint32_t)(int16_t)index;
		}
		return base + (int8_t)(ext & 0xFF) + index;
	};

	uint32_t addr;
	bool showAddress = true;
	switch(mode) {
		case SacAddrMode::Ind: case SacAddrMode::PostInc: addr = state.A[reg]; break;
		case SacAddrMode::PreDec: addr = state.A[reg] - (size == 1 && reg == 7 ? 2 : size); break;
		case SacAddrMode::Disp: addr = state.A[reg] + (int16_t)readWord(extOffset); break;
		case SacAddrMode::Index: addr = indexed(state.A[reg], readWord(extOffset)); break;
		case SacAddrMode::PcDisp: addr = pc + extOffset + (int16_t)readWord(extOffset); break;
		case SacAddrMode::PcIndex: addr = indexed(pc + extOffset, readWord(extOffset)); break;
		case SacAddrMode::AbsShort: addr = (uint32_t)(int16_t)readWord(extOffset); showAddress = false; break;
		case SacAddrMode::AbsLong: addr = ((uint32_t)readWord(extOffset) << 16) | readWord(extOffset + 2); showAddress = false; break;
		default: return {};
	}

	bool readsNothing = (opCode & 0xF1C0) == 0x41C0 || (opCode & 0xFF80) == 0x4E80 || (opCode & 0xFFC0) == 0x4840;
	if(readsNothing) {
		//LEA, JSR, JMP and PEA
		return EffectiveAddressInfo(addr & 0xFFFFFF, 0, showAddress, MemoryType::SacMemory);
	}
	return EffectiveAddressInfo(addr & 0xFFFFFF, size, showAddress, MemoryType::SacMemory);
}

//JSR and BSR
bool SacDisUtils::IsJumpToSub(uint16_t opCode)
{
	return (opCode & 0xFFC0) == 0x4E80 || (opCode & 0xFF00) == 0x6100;
}

//RTS, RTE and RTR
bool SacDisUtils::IsReturnInstruction(uint16_t opCode)
{
	return opCode == 0x4E75 || opCode == 0x4E73 || opCode == 0x4E77;
}

//JMP, BRA, and the returns
bool SacDisUtils::IsUnconditionalJump(uint16_t opCode)
{
	return (opCode & 0xFFC0) == 0x4EC0 || (opCode & 0xFF00) == 0x6000 || IsReturnInstruction(opCode);
}

//Bcc and DBcc
bool SacDisUtils::IsConditionalJump(uint16_t opCode)
{
	return ((opCode & 0xF000) == 0x6000 && (opCode & 0x0F00) >= 0x0200) || (opCode & 0xF0F8) == 0x50C8;
}

CdlFlags::CdlFlags SacDisUtils::GetOpFlags(uint16_t prevOpCode, uint32_t pc, uint32_t prevPc, uint8_t prevOpSize)
{
	if(pc != prevPc + prevOpSize) {
		if(IsJumpToSub(prevOpCode)) {
			return CdlFlags::SubEntryPoint;
		} else if((IsUnconditionalJump(prevOpCode) || IsConditionalJump(prevOpCode)) && !IsReturnInstruction(prevOpCode)) {
			return CdlFlags::JumpTarget;
		}
	}
	return (CdlFlags::CdlFlags)0;
}
