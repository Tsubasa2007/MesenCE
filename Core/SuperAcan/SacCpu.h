#pragma once
#include "pch.h"
#include "Utilities/ISerializable.h"

class SacMemoryManager;
class SacCpuCore;

struct SacCpuState
{
	uint32_t PC;
	uint16_t SR;
	uint32_t D[8];
	uint32_t A[8];
	uint64_t CycleCount;
	bool Halted;
};

//The main processor, a 68000. The core is Moira (MIT, see SuperAcan/Moira/LICENSE), kept out of
//this header on purpose: its register file is a union of anonymous structs, which the rest of
//the core is built without language extensions to allow, so only SacCpu.cpp ever sees it.
class SacCpu final : public ISerializable
{
private:
	unique_ptr<SacCpuCore> _core;

	//Interrupt lines held by the hardware, bit n for level n. The 68000 sees only the highest.
	uint8_t _irqLines = 0;

	void UpdateIpl();

public:
	SacCpu(SacMemoryManager* memoryManager);
	~SacCpu();

	void Reset();
	void RunUntil(uint64_t cycle);
	uint64_t GetCycleCount();

	void SetIrq(uint8_t level);
	void ClearIrq(uint8_t level);

	SacCpuState GetState();

	//One instruction at addr as text; returns its length in bytes
	int Disassemble(uint32_t addr, string& out);

	void Serialize(Serializer& s) override;
};
