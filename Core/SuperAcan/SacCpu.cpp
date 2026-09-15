#include "pch.h"
#include <mutex>
#include "SuperAcan/SacCpu.h"
#include "SuperAcan/SacMemoryManager.h"
#include "Shared/Emulator.h"
#include "Shared/CpuType.h"
#include "Shared/MemoryOperationType.h"
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
	Emulator* _emu = nullptr;
	SacMemoryManager* _memoryManager = nullptr;

	//An interrupt being taken, reported to the debugger once its vector is known
	bool _irqPending = false;
	uint8_t _irqLevel = 0;
	uint32_t _irqReturnPc = 0;

	//The function code pins tell an instruction fetch from a data read
	MemoryOperationType GetReadType() const
	{
		return (fcl & 0x02) ? MemoryOperationType::ExecOperand : MemoryOperationType::Read;
	}

protected:
	//The 68000 has a 16-bit bus, so a long is two word accesses, high word first. Every access
	//goes past the debugger, which does nothing unless one is running.
	moira::u8 read8(moira::u32 addr) const override
	{
		uint8_t value = _memoryManager->Read8(addr);
		_emu->ProcessMemoryRead<CpuType::Sac>(addr & 0xFFFFFF, value, GetReadType());
		return value;
	}

	moira::u16 read16(moira::u32 addr) const override
	{
		uint16_t value = _memoryManager->Read16(addr);
		_emu->ProcessMemoryRead<CpuType::Sac, 2>(addr & 0xFFFFFF, value, GetReadType());
		return value;
	}

	moira::u32 read32(moira::u32 addr) const override
	{
		return ((moira::u32)read16(addr) << 16) | read16(addr + 2);
	}

	//The disassembler must not disturb anything it reads
	moira::u16 read16Dasm(moira::u32 addr) const override
	{
		return _memoryManager->DebugRead16(addr);
	}

	void write8(moira::u32 addr, moira::u8 val) const override
	{
		uint8_t value = val;
		if(_emu->ProcessMemoryWrite<CpuType::Sac>(addr & 0xFFFFFF, value, MemoryOperationType::Write)) {
			_memoryManager->Write8(addr, value);
		}
	}

	void write16(moira::u32 addr, moira::u16 val) const override
	{
		uint16_t value = val;
		if(_emu->ProcessMemoryWrite<CpuType::Sac, 2>(addr & 0xFFFFFF, value, MemoryOperationType::Write)) {
			_memoryManager->Write16(addr, value);
		}
	}

	void write32(moira::u32 addr, moira::u32 val) const override
	{
		write16(addr, (uint16_t)(val >> 16));
		write16(addr + 2, (uint16_t)val);
	}

	//MAME holds every one of this machine's interrupt lines until the processor takes the
	//interrupt (HOLD_LINE), so taking one is what drops it
	void willInterrupt(moira::u8 level) override
	{
		_owner->ClearIrq(level);
		_irqPending = true;
		_irqLevel = level;
		_irqReturnPc = reg.pc;
	}

	void didJumpToVector(int nr, moira::u32 addr) override
	{
		if(_irqPending) {
			_irqPending = false;
			_emu->ProcessInterrupt<CpuType::Sac>(_irqReturnPc, addr & 0xFFFFFF, _irqLevel == 7);
		}
	}

	void cpuDidHalt() override
	{
		MessageManager::Log("[SAC] 68000 halted at $" + HexUtilities::ToHex24(getPC0()));
	}

public:
	SacCpuCore(SacCpu* owner, Emulator* emu, SacMemoryManager* memoryManager)
	{
		_owner = owner;
		_emu = emu;
		_memoryManager = memoryManager;
		setModel(moira::Model::M68000);
	}

	bool IsStopped()
	{
		return (flags & moira::State::STOPPED) != 0;
	}

	//Whether the next execute() starts an exception instead of an instruction: a trace exception
	//already pending, or an interrupt above the mask in the level Moira has polled (its own test,
	//see Moira::execute and checkForIrq)
	bool WillTakeException()
	{
		if(flags & moira::State::TRACE_EXC) {
			return true;
		}
		return (flags & moira::State::CHECK_IRQ) && (reg.ipl > reg.sr.ipl || reg.ipl == 7);
	}

	void SyncState(SacCpuState& state)
	{
		state.CycleCount = (uint64_t)clock;

		//The 68000 keeps a 32-bit PC but has a 24-bit bus: code reached through a sign-extended
		//short address runs at $FFxxxxxx, which is $FFxxxx to everything else
		state.PC = reg.pc0 & 0xFFFFFF;
		for(int i = 0; i < 8; i++) {
			state.D[i] = reg.d[i];
			state.A[i] = reg.a[i];
		}
		state.USP = reg.sr.s ? reg.usp : reg.a[7];
		state.SSP = reg.sr.s ? reg.a[7] : reg.isp;
		state.SR = getSR();
		state.Halted = (flags & moira::State::HALTED) != 0;
		state.Stopped = (flags & moira::State::STOPPED) != 0;
	}

	//Registers edited in the debugger. A7 is the active stack pointer, and the other one is taken
	//from USP or SSP.
	void ApplyState(SacCpuState& state)
	{
		setSR(state.SR);
		for(int i = 0; i < 8; i++) {
			reg.d[i] = state.D[i];
			reg.a[i] = state.A[i];
		}
		if(reg.sr.s) {
			reg.usp = state.USP;
		} else {
			reg.isp = state.SSP;
		}
		if(state.PC != (reg.pc0 & 0xFFFFFF)) {
			SetPc(state.PC);
		}
	}

	//Between instructions the prefetch queue holds the word at PC and the one after it
	void SetPc(uint32_t pc)
	{
		reg.pc = pc;
		reg.pc0 = pc;
		queue.ird = read16Dasm(pc);
		queue.irc = read16Dasm(pc + 2);
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

//A 68000 with no machine behind it, reading from a copy of an instruction's bytes, for
//disassembly that must not touch the bus
class SacDasmCore final : public moira::Moira
{
private:
	const uint8_t* _bytes = nullptr;
	uint32_t _base = 0;
	uint32_t _size = 0;

	moira::u16 Word(moira::u32 addr) const
	{
		uint32_t offset = (addr - _base) & 0xFFFFFF;
		return offset + 1 < _size ? (moira::u16)((_bytes[offset] << 8) | _bytes[offset + 1]) : 0;
	}

protected:
	moira::u8 read8(moira::u32 addr) const override
	{
		uint32_t offset = (addr - _base) & 0xFFFFFF;
		return offset < _size ? _bytes[offset] : 0;
	}

	moira::u16 read16(moira::u32 addr) const override { return Word(addr); }
	moira::u32 read32(moira::u32 addr) const override { return ((moira::u32)Word(addr) << 16) | Word(addr + 2); }
	moira::u16 read16Dasm(moira::u32 addr) const override { return Word(addr); }

	void write8(moira::u32 addr, moira::u8 val) const override {}
	void write16(moira::u32 addr, moira::u16 val) const override {}
	void write32(moira::u32 addr, moira::u32 val) const override {}

public:
	SacDasmCore()
	{
		setModel(moira::Model::M68000);
	}

	int Disassemble(uint32_t addr, const uint8_t* bytes, uint32_t size, char* out)
	{
		_bytes = bytes;
		_base = addr;
		_size = size;
		return disassemble(out, addr);
	}
};

SacCpu::SacCpu(Emulator* emu, SacMemoryManager* memoryManager)
{
	_emu = emu;
	_core.reset(new SacCpuCore(this, emu, memoryManager));
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

//One instruction at a time, so the debugger sees each one before it runs. A 68000 waiting in
//STOP is reported as halted rather than as the same instruction over and over, and a step that
//takes an interrupt instead of an instruction is not reported at all: the instruction runs after
//the handler, and the debugger hears of the interrupt from didJumpToVector.
void SacCpu::RunUntil(uint64_t cycle)
{
	//A halted 68000 stays halted until reset; its clock still has to keep up with the machine
	if(_core->isHalted()) {
		if((uint64_t)_core->getClock() < cycle) {
			_core->setClock((moira::i64)cycle);
		}
		return;
	}

	while((uint64_t)_core->getClock() < cycle) {
		if(_core->IsStopped()) {
			_emu->ProcessHaltedCpu<CpuType::Sac>();
		} else if(!_core->WillTakeException()) {
			_emu->ProcessInstruction<CpuType::Sac>();
		}
		_core->execute();
	}
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

SacCpuState& SacCpu::GetState()
{
	_core->SyncState(_state);
	return _state;
}

void SacCpu::ApplyState()
{
	_core->ApplyState(_state);
}

void SacCpu::SetProgramCounter(uint32_t pc)
{
	_core->SetPc(pc & 0xFFFFFF);
}

int SacCpu::Disassemble(uint32_t addr, string& out)
{
	char buffer[128] = {};
	int length = _core->disassemble(buffer, addr);
	out = buffer;
	return length;
}

//Built once and shared: the emulation and the debugger window both disassemble
static SacDasmCore* GetDasmCore()
{
	static SacDasmCore* dasm = new SacDasmCore();
	return dasm;
}

int SacCpu::DisassembleBytes(uint32_t addr, const uint8_t* bytes, uint32_t size, string& out)
{
	static std::mutex lock;

	std::lock_guard<std::mutex> guard(lock);
	char buffer[128] = {};
	int length = GetDasmCore()->Disassemble(addr & 0xFFFFFF, bytes, size, buffer);
	out += buffer;
	return length;
}

static_assert((int)moira::Mode::IXPC == (int)SacAddrMode::PcIndex && (int)moira::Mode::IP == (int)SacAddrMode::None, "SacAddrMode must follow moira::Mode");

//The instruction table is filled when the core is built and only read after, so no lock
void SacCpu::GetOpInfo(uint16_t opCode, SacAddrMode& mode, uint8_t& size)
{
	moira::InstrInfo info = GetDasmCore()->getInstrInfo(opCode);
	mode = (int)info.M <= (int)SacAddrMode::None ? (SacAddrMode)info.M : SacAddrMode::None;
	size = info.S <= 4 ? (uint8_t)info.S : 0;
}

void SacCpu::Serialize(Serializer& s)
{
	SV(_irqLines);
	_core->Serialize(s);
}
