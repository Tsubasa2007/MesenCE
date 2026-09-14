#include "pch.h"
#include "SuperAcan/SacCpu.h"
#include "SuperAcan/SacMemoryManager.h"
#include "Shared/MessageManager.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/Serializer.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "SuperAcan/Moira/Moira.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

class SacCpuCore final : public moira::Moira
{
private:
	SacCpu* _owner = nullptr;
	SacMemoryManager* _memoryManager = nullptr;

protected:
	//The 68000 has a 16-bit bus, so a long is two word accesses, high word first
	moira::u8 read8(moira::u32 addr) const override { return _memoryManager->Read8(addr); }
	moira::u16 read16(moira::u32 addr) const override { return _memoryManager->Read16(addr); }
	moira::u32 read32(moira::u32 addr) const override
	{
		return ((moira::u32)_memoryManager->Read16(addr) << 16) | _memoryManager->Read16(addr + 2);
	}

	void write8(moira::u32 addr, moira::u8 val) const override { _memoryManager->Write8(addr, val); }
	void write16(moira::u32 addr, moira::u16 val) const override { _memoryManager->Write16(addr, val); }
	void write32(moira::u32 addr, moira::u32 val) const override
	{
		_memoryManager->Write16(addr, (uint16_t)(val >> 16));
		_memoryManager->Write16(addr + 2, (uint16_t)val);
	}

	//MAME holds every one of this machine's interrupt lines until the processor takes the
	//interrupt (HOLD_LINE), so taking one is what drops it
	void willInterrupt(moira::u8 level) override
	{
		_owner->ClearIrq(level);
	}

	void cpuDidHalt() override
	{
		MessageManager::Log("[SAC] 68000 halted at $" + HexUtilities::ToHex24(getPC0()));
	}

public:
	SacCpuCore(SacCpu* owner, SacMemoryManager* memoryManager)
	{
		_owner = owner;
		_memoryManager = memoryManager;
		setModel(moira::Model::M68000);
	}

	SacCpuState GetState()
	{
		SacCpuState state = {};
		state.PC = getPC();
		state.SR = getSR();
		for(int i = 0; i < 8; i++) {
			state.D[i] = getD(i);
			state.A[i] = getA(i);
		}
		state.CycleCount = (uint64_t)getClock();
		state.Halted = isHalted();
		return state;
	}

	//Everything that makes up a running 68000 as Moira holds it. Its jump tables and the
	//disassembler's settings follow from the model and are not state.
	void Serialize(Serializer& s)
	{
		SV(clock);
		SV(reg.pc);
		SV(reg.pc0);
		SV(reg.sr.t1);
		SV(reg.sr.t0);
		SV(reg.sr.s);
		SV(reg.sr.m);
		SV(reg.sr.x);
		SV(reg.sr.n);
		SV(reg.sr.z);
		SV(reg.sr.v);
		SV(reg.sr.c);
		SV(reg.sr.ipl);
		SVArray(reg.r, 16);
		SV(reg.usp);
		SV(reg.isp);
		SV(reg.msp);
		SV(reg.ipl);
		SV(reg.vbr);
		SV(reg.sfc);
		SV(reg.dfc);
		SV(reg.cacr);
		SV(reg.caar);
		SV(queue.irc);
		SV(queue.ird);
		SV(ipl);
		SV(fcl);
		SV(fcSource);
		SV(exception);
		SV(loopModeDelay);
		SV(readBuffer);
		SV(writeBuffer);
		SV(flags);
	}
};

SacCpu::SacCpu(SacMemoryManager* memoryManager)
{
	_core.reset(new SacCpuCore(this, memoryManager));
}

SacCpu::~SacCpu()
{
}

void SacCpu::Reset()
{
	_irqLines = 0;
	_core->setIPL(0);
	_core->reset();
}

void SacCpu::RunUntil(uint64_t cycle)
{
	//A halted 68000 stays halted until reset; its clock still has to keep up with the machine
	if(_core->isHalted()) {
		if((uint64_t)_core->getClock() < cycle) {
			_core->setClock((moira::i64)cycle);
		}
		return;
	}
	_core->executeUntil((moira::i64)cycle);
}

uint64_t SacCpu::GetCycleCount()
{
	return (uint64_t)_core->getClock();
}

void SacCpu::SetIrq(uint8_t level)
{
	_irqLines |= (uint8_t)(1 << level);
	UpdateIpl();
}

void SacCpu::ClearIrq(uint8_t level)
{
	_irqLines &= (uint8_t)~(1 << level);
	UpdateIpl();
}

void SacCpu::UpdateIpl()
{
	uint8_t level = 0;
	for(int i = 7; i > 0; i--) {
		if(_irqLines & (1 << i)) {
			level = (uint8_t)i;
			break;
		}
	}
	_core->setIPL(level);
}

SacCpuState SacCpu::GetState()
{
	return _core->GetState();
}

int SacCpu::Disassemble(uint32_t addr, string& out)
{
	char buffer[128] = {};
	int length = _core->disassemble(buffer, addr);
	out = buffer;
	return length;
}

void SacCpu::Serialize(Serializer& s)
{
	SV(_irqLines);
	_core->Serialize(s);
}
