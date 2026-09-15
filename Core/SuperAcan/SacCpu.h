#pragma once
#include "pch.h"
#include "Shared/BaseState.h"
#include "Utilities/ISerializable.h"

class Emulator;
class SacMemoryManager;
class SacCpuCore;

//The 68000's registers as the debugger and the rest of the machine see them: PC is the address of
//the instruction being executed, A[7] the active stack pointer, and USP/SSP the two stack pointers
struct SacCpuState : BaseState
{
	uint64_t CycleCount;
	uint32_t PC;
	uint32_t D[8];
	uint32_t A[8];
	uint32_t USP;
	uint32_t SSP;
	uint16_t SR;
	bool Halted;
	bool Stopped;
};

//The 68000's addressing modes, in the order Moira numbers them
enum class SacAddrMode : uint8_t
{
	Dn,       //Dn
	An,       //An
	Ind,      //(An)
	PostInc,  //(An)+
	PreDec,   //-(An)
	Disp,     //(d,An)
	Index,    //(d,An,Xi)
	AbsShort, //(xxx).w
	AbsLong,  //(xxx).l
	PcDisp,   //(d,PC)
	PcIndex,  //(d,PC,Xi)
	Imm,      //#xxx
	None      //no effective address
};

//The main processor, a 68000. The core is Moira (MIT, see SuperAcan/Moira/LICENSE), kept out of
//this header on purpose: its register file is a union of anonymous structs, which the rest of
//the core is built without language extensions to allow, so only SacCpu.cpp ever sees it.
class SacCpu final : public ISerializable
{
private:
	unique_ptr<SacCpuCore> _core;
	Emulator* _emu = nullptr;
	SacCpuState _state = {};

	//Interrupt lines held by the hardware, bit n for level n. The 68000 sees only the highest.
	uint8_t _irqLines = 0;

	void UpdateIpl();

public:
	SacCpu(Emulator* emu, SacMemoryManager* memoryManager);
	~SacCpu();

	void Reset();
	void RunUntil(uint64_t cycle);
	uint64_t GetCycleCount();

	void SetIrq(uint8_t level);
	void ClearIrq(uint8_t level);
	uint8_t GetIrqLines() { return _irqLines; }

	//The registers, brought up to date; ApplyState writes them back after the debugger edits them
	SacCpuState& GetState();
	void ApplyState();
	void SetProgramCounter(uint32_t pc);

	//One instruction at addr as text; returns its length in bytes
	int Disassemble(uint32_t addr, string& out);

	//The same from a copy of the bytes rather than the bus, for the debugger's disassembly
	static int DisassembleBytes(uint32_t addr, const uint8_t* bytes, uint32_t size, string& out);

	//What the decoder knows of an opcode: the mode of the effective address in bits 0-5 (the
	//source, for MOVE) and the operand size in bytes, 0 when it has none
	static void GetOpInfo(uint16_t opCode, SacAddrMode& mode, uint8_t& size);

	void Serialize(Serializer& s) override;
};
