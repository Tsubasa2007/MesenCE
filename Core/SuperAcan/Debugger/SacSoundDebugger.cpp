#include "pch.h"
#include "SuperAcan/Debugger/SacSoundDebugger.h"
#include "SuperAcan/Debugger/SacSoundDisUtils.h"
#include "SuperAcan/Debugger/SacSoundTraceLogger.h"
#include "SuperAcan/Debugger/SacSoundAssembler.h"
#include "SuperAcan/SacConsole.h"
#include "SuperAcan/SacMemoryManager.h"
#include "SuperAcan/W65C02.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/Disassembler.h"
#include "Debugger/CallstackManager.h"
#include "Debugger/BreakpointManager.h"
#include "Debugger/Debugger.h"
#include "Debugger/MemoryAccessCounter.h"
#include "Shared/Emulator.h"
#include "Shared/MemoryOperationType.h"

SacSoundDebugger::SacSoundDebugger(Debugger* debugger) : IDebugger(debugger->GetEmulator())
{
	_debugger = debugger;
	_disassembler = debugger->GetDisassembler();
	_memoryAccessCounter = debugger->GetMemoryAccessCounter();
	_console = (SacConsole*)debugger->GetConsole();
	_memoryManager = _console->GetMemoryManager();

	_traceLogger.reset(new SacSoundTraceLogger(debugger, this, _console));
	_callstackManager.reset(new CallstackManager(debugger, this));
	_assembler.reset(new SacSoundAssembler(debugger->GetLabelManager()));

	//Marked breakpoints appear in the 68000's event viewer, as the SPC's do in the SNES one
	_breakpointManager.reset(new BreakpointManager(debugger, this, CpuType::SacSound, debugger->GetEventManager(CpuType::Sac)));
	_step.reset(new StepRequest());
}

SacSoundDebugger::~SacSoundDebugger()
{
}

void SacSoundDebugger::Reset()
{
	_callstackManager->Clear();
	ResetPrevOpCode();
}

SacSoundCpuState& SacSoundDebugger::UpdateState()
{
	_memoryManager->GetSoundCpuState(_state);
	return _state;
}

void SacSoundDebugger::ProcessInstruction()
{
	SacSoundCpuState& state = UpdateState();
	uint16_t pc = state.PC;
	uint8_t opCode = _memoryManager->SoundDebugRead(pc);
	AddressInfo addressInfo = { pc, MemoryType::SacSoundRam };
	MemoryOperationInfo operation(pc, opCode, MemoryOperationType::ExecOpCode, MemoryType::SacSoundMemory);
	InstructionProgress.LastMemOperation = operation;
	InstructionProgress.StartCycle = state.CycleCount;

	//The 68000 loads the sound program, some of it by DMA the debugger does not see, so an
	//instruction cached from an earlier program is dropped once its bytes no longer match
	DisassemblyInfo cached = _disassembler->GetDisassemblyInfo(addressInfo, pc, 0, CpuType::SacSound);
	uint8_t* byteCode = cached.GetByteCode();
	for(int i = 0; i < cached.GetOpSize(); i++) {
		if(byteCode[i] != _memoryManager->SoundDebugRead((uint16_t)(pc + i))) {
			_disassembler->InvalidateCache(addressInfo, CpuType::SacSound);
			break;
		}
	}
	_disassembler->BuildCache(addressInfo, 0, CpuType::SacSound);

	ProcessCallStackUpdates(addressInfo, pc, state.SP);

	if(_traceLogger->IsEnabled()) {
		DisassemblyInfo disInfo = _disassembler->GetDisassemblyInfo(addressInfo, pc, 0, CpuType::SacSound);
		_traceLogger->Log(state, disInfo, operation, addressInfo);
	}

	_prevOpCode = opCode;
	_prevProgramCounter = pc;
	_prevStackPointer = state.SP;

	_step->ProcessCpuExec();
	_debugger->ProcessBreakConditions(CpuType::SacSound, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

void SacSoundDebugger::ProcessCallStackUpdates(AddressInfo& destAddr, uint16_t destPc, uint8_t sp)
{
	if(SacSoundDisUtils::IsJumpToSub(_prevOpCode)) {
		//JSR, BRK
		uint16_t returnPc = (uint16_t)(_prevProgramCounter + SacSoundDisUtils::GetOpSize(_prevOpCode));
		AddressInfo src = { _prevProgramCounter, MemoryType::SacSoundRam };
		AddressInfo ret = { returnPc, MemoryType::SacSoundRam };
		_callstackManager->Push(src, _prevProgramCounter, destAddr, destPc, ret, returnPc, _prevStackPointer, StackFrameFlags::None);
	} else if(SacSoundDisUtils::IsReturnInstruction(_prevOpCode)) {
		//RTS, RTI
		_callstackManager->Pop(destAddr, destPc, sp);

		if(_step->BreakAddress == (int64_t)destPc && _step->BreakStackPointer == (int64_t)sp) {
			//Back at the expected return address - break now for step over/step out
			_step->Break(BreakSource::CpuStep);
		}
	}
}

//Called once the return address and status are on the stack and the vector has been taken
void SacSoundDebugger::ProcessInterrupt(uint32_t originalPc, uint32_t currentPc, bool forNmi)
{
	AddressInfo ret = { (int32_t)originalPc, MemoryType::SacSoundRam };
	AddressInfo dest = { (int32_t)currentPc, MemoryType::SacSoundRam };
	uint8_t originalSp = (uint8_t)(UpdateState().SP + 3);

	//A call or return just before the interrupt has not been seen by ProcessInstruction yet
	ProcessCallStackUpdates(ret, (uint16_t)originalPc, originalSp);
	ResetPrevOpCode();

	_callstackManager->Push(ret, originalPc, dest, currentPc, ret, originalPc, originalSp, forNmi ? StackFrameFlags::Nmi : StackFrameFlags::Irq);
	_step->ProcessNmiIrq(forNmi);
}

void SacSoundDebugger::ProcessRead(uint32_t addr, uint8_t value, MemoryOperationType type)
{
	AddressInfo addressInfo = { (int32_t)addr, MemoryType::SacSoundRam };
	MemoryOperationInfo operation(addr, value, type, MemoryType::SacSoundMemory);
	InstructionProgress.LastMemOperation = operation;

	if(type == MemoryOperationType::ExecOpCode) {
		//Logged and checked for breakpoints in ProcessInstruction
		_memoryAccessCounter->ProcessMemoryExec(addressInfo, _console->GetMasterClock());
		return;
	}

	if(type == MemoryOperationType::ExecOperand) {
		_memoryAccessCounter->ProcessMemoryExec(addressInfo, _console->GetMasterClock());
	} else {
		_memoryAccessCounter->ProcessMemoryRead(addressInfo, _console->GetMasterClock());
	}

	if(_traceLogger->IsEnabled()) {
		_traceLogger->LogNonExec(operation, addressInfo);
	}
	_debugger->ProcessBreakConditions(CpuType::SacSound, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

void SacSoundDebugger::ProcessWrite(uint32_t addr, uint8_t value, MemoryOperationType type)
{
	AddressInfo addressInfo = { (int32_t)addr, MemoryType::SacSoundRam };
	MemoryOperationInfo operation(addr, value, type, MemoryType::SacSoundMemory);
	InstructionProgress.LastMemOperation = operation;

	_disassembler->InvalidateCache(addressInfo, CpuType::SacSound);
	_debugger->ProcessBreakConditions(CpuType::SacSound, *_step.get(), _breakpointManager.get(), operation, addressInfo);
	_memoryAccessCounter->ProcessMemoryWrite(addressInfo, _console->GetMasterClock());

	if(_traceLogger->IsEnabled()) {
		_traceLogger->LogNonExec(operation, addressInfo);
	}
}

void SacSoundDebugger::Run()
{
	_step.reset(new StepRequest());
}

void SacSoundDebugger::Step(int32_t stepCount, StepType type)
{
	StepRequest step(type);

	switch(type) {
		case StepType::Step: step.StepCount = stepCount; break;

		case StepType::StepOut:
			step.BreakAddress = _callstackManager->GetReturnAddress();
			step.BreakStackPointer = _callstackManager->GetReturnStackPointer();
			break;

		case StepType::StepOver:
			if(SacSoundDisUtils::IsJumpToSub(_prevOpCode)) {
				step.BreakAddress = (uint16_t)(_prevProgramCounter + SacSoundDisUtils::GetOpSize(_prevOpCode));
				step.BreakStackPointer = _prevStackPointer;
			} else {
				//For any other instruction, step over is the same as step into
				step.StepCount = 1;
			}
			break;

		default:
			break;
	}

	_step.reset(new StepRequest(step));
}

DebuggerFeatures SacSoundDebugger::GetSupportedFeatures()
{
	DebuggerFeatures features = {};
	features.RunToIrq = true;
	features.RunToNmi = true;
	features.StepOver = true;
	features.StepOut = true;
	features.CallStack = true;
	features.ChangeProgramCounter = AllowChangeProgramCounter;
	return features;
}

void SacSoundDebugger::SetProgramCounter(uint32_t addr, bool updateDebuggerOnly)
{
	if(!updateDebuggerOnly) {
		_memoryManager->GetSoundCpu()->SetProgramCounter((uint16_t)addr);
	}
	_prevOpCode = _memoryManager->SoundDebugRead((uint16_t)addr);
	_prevProgramCounter = (uint16_t)addr;
	_prevStackPointer = UpdateState().SP;
}

uint32_t SacSoundDebugger::GetProgramCounter(bool getInstPc)
{
	return getInstPc ? _prevProgramCounter : UpdateState().PC;
}

uint64_t SacSoundDebugger::GetCpuCycleCount(bool forProfiler)
{
	return UpdateState().CycleCount;
}

void SacSoundDebugger::ResetPrevOpCode()
{
	_prevOpCode = 0xEA; //NOP: neither a call nor a return
}

CallstackManager* SacSoundDebugger::GetCallstackManager()
{
	return _callstackManager.get();
}

BreakpointManager* SacSoundDebugger::GetBreakpointManager()
{
	return _breakpointManager.get();
}

IAssembler* SacSoundDebugger::GetAssembler()
{
	return _assembler.get();
}

//Like the SPC, the sound processor has no event viewer of its own
BaseEventManager* SacSoundDebugger::GetEventManager()
{
	return nullptr;
}

ITraceLogger* SacSoundDebugger::GetTraceLogger()
{
	return _traceLogger.get();
}

BaseState& SacSoundDebugger::GetState()
{
	return UpdateState();
}

void SacSoundDebugger::ApplyState()
{
	_memoryManager->SetSoundCpuState(_state);
}
