#include "pch.h"
#include "SuperAcan/Debugger/SacDebugger.h"
#include "SuperAcan/Debugger/SacDisUtils.h"
#include "SuperAcan/Debugger/SacEventManager.h"
#include "SuperAcan/Debugger/SacTraceLogger.h"
#include "SuperAcan/Debugger/SacPpuTools.h"
#include "SuperAcan/Debugger/SacAssembler.h"
#include "SuperAcan/SacTypes.h"
#include "SuperAcan/SacConsole.h"
#include "SuperAcan/SacCpu.h"
#include "SuperAcan/SacMemoryManager.h"
#include "SuperAcan/SacController.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/Disassembler.h"
#include "Debugger/CallstackManager.h"
#include "Debugger/BreakpointManager.h"
#include "Debugger/Debugger.h"
#include "Debugger/MemoryAccessCounter.h"
#include "Debugger/MemoryDumper.h"
#include "Debugger/CodeDataLogger.h"
#include "Debugger/BaseEventManager.h"
#include "Debugger/StepBackManager.h"
#include "Utilities/Patches/IpsPatcher.h"
#include "Shared/EmuSettings.h"
#include "Shared/Emulator.h"
#include "Shared/BaseControlManager.h"
#include "Shared/MemoryOperationType.h"

SacDebugger::SacDebugger(Debugger* debugger) : IDebugger(debugger->GetEmulator())
{
	_debugger = debugger;
	_emu = debugger->GetEmulator();

	_disassembler = debugger->GetDisassembler();
	_memoryAccessCounter = debugger->GetMemoryAccessCounter();

	_console = (SacConsole*)debugger->GetConsole();
	_cpu = _console->GetCpu();
	_memoryManager = _console->GetMemoryManager();

	_settings = _emu->GetSettings();

	_codeDataLogger.reset(new CodeDataLogger(debugger, MemoryType::SacPrgRom, _emu->GetMemory(MemoryType::SacPrgRom).Size, CpuType::Sac, _emu->GetCrc32()));
	_cdlFile = _codeDataLogger->GetCdlFilePath(_emu->GetRomInfo().RomFile.GetFileName());
	_codeDataLogger->LoadCdlFile(_cdlFile, _settings->GetDebugConfig().AutoResetCdl);

	_traceLogger.reset(new SacTraceLogger(debugger, this, _console));
	_ppuTools.reset(new SacPpuTools(debugger, _emu, _console));
	_assembler.reset(new SacAssembler(debugger->GetLabelManager()));

	_stepBackManager.reset(new StepBackManager(_emu, this));
	_eventManager.reset(new SacEventManager(debugger, _console));
	_callstackManager.reset(new CallstackManager(debugger, this));
	_breakpointManager.reset(new BreakpointManager(debugger, this, CpuType::Sac, _eventManager.get()));
	_step.reset(new StepRequest());

	SacDisUtils::SetMemoryDumper(debugger->GetMemoryDumper());
}

SacDebugger::~SacDebugger()
{
	_codeDataLogger->SaveCdlFile(_cdlFile);
	SacDisUtils::SetMemoryDumper(nullptr);
}

void SacDebugger::Reset()
{
	_callstackManager->Clear();
	ResetPrevOpCode();
}

AddressInfo SacDebugger::GetAbsoluteAddress(uint32_t addr)
{
	AddressInfo relAddr = { (int32_t)addr, MemoryType::SacMemory };
	return _console->GetAbsoluteAddress(relAddr);
}

void SacDebugger::ProcessInstruction()
{
	SacCpuState& state = _cpu->GetState();
	uint32_t pc = state.PC;
	AddressInfo addressInfo = GetAbsoluteAddress(pc);
	uint16_t opCode = _memoryManager->DebugRead16(pc);
	MemoryOperationInfo operation(pc, opCode, MemoryOperationType::ExecOpCode, MemoryType::SacMemory);
	InstructionProgress.LastMemOperation = operation;
	InstructionProgress.StartCycle = state.CycleCount;

	if(addressInfo.Address >= 0) {
		if(addressInfo.Type == MemoryType::SacPrgRom) {
			uint8_t prevSize = _prevProgramCounter ? GetPrevOpCodeSize() : 0;
			_codeDataLogger->SetCode(addressInfo.Address, SacDisUtils::GetOpFlags(_prevOpCode, pc, _prevProgramCounter, prevSize));
		}
		_disassembler->BuildCache(addressInfo, 0, CpuType::Sac);
	}

	ProcessCallStackUpdates(addressInfo, pc, state.A[7]);

	if(_traceLogger->IsEnabled()) {
		DisassemblyInfo disInfo = _disassembler->GetDisassemblyInfo(addressInfo, pc, 0, CpuType::Sac);
		_traceLogger->Log(state, disInfo, operation, addressInfo);
	}
	_memoryAccessCounter->ProcessMemoryExec(addressInfo, _console->GetMasterClock());

	_prevOpCode = opCode;
	_prevProgramCounter = pc;
	_prevStackPointer = state.A[7];

	_step->ProcessCpuExec();
	_debugger->ProcessBreakConditions(CpuType::Sac, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

//The accesses the event viewer shows: the video chip (registers, palette, video RAM), the pads
//and sound registers in sound RAM, the UM6619 and the lockout chip
static bool IsEventAddress(uint32_t addr)
{
	addr &= 0xFFFFFF;
	return (addr >= 0xF00000 && addr <= 0xF003FF) || (addr >= 0xF40000 && addr <= 0xF5FFFF) ||
		(addr >= 0xE80200 && addr <= 0xE80203) || (addr >= 0xE80400 && addr <= 0xE804FF) ||
		(addr >= 0xE90000 && addr <= 0xE9003F) || (addr >= 0xEB0D00 && addr <= 0xEB0D03);
}

template<uint8_t accessWidth>
void SacDebugger::ProcessRead(uint32_t addr, uint16_t value, MemoryOperationType type)
{
	AddressInfo addressInfo = GetAbsoluteAddress(addr);
	MemoryOperationInfo operation(addr, value, type, MemoryType::SacMemory);
	InstructionProgress.LastMemOperation = operation;

	if(addressInfo.Address >= 0 && addressInfo.Type == MemoryType::SacPrgRom) {
		for(int i = 0; i < accessWidth; i++) {
			if(type == MemoryOperationType::ExecOperand) {
				_codeDataLogger->SetCode(addressInfo.Address + i);
			} else {
				_codeDataLogger->SetData(addressInfo.Address + i);
			}
		}
	}

	if(_traceLogger->IsEnabled()) {
		_traceLogger->LogNonExec(operation, addressInfo);
	}

	if(type == MemoryOperationType::ExecOperand) {
		_memoryAccessCounter->ProcessMemoryExec<accessWidth>(addressInfo, _console->GetMasterClock());
	} else {
		_memoryAccessCounter->ProcessMemoryRead<accessWidth>(addressInfo, _console->GetMasterClock());
		if(IsEventAddress(addr)) {
			_eventManager->AddEvent(DebugEventType::Register, operation);
		}
	}

	_debugger->ProcessBreakConditions<accessWidth>(CpuType::Sac, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

template<uint8_t accessWidth>
void SacDebugger::ProcessWrite(uint32_t addr, uint16_t value, MemoryOperationType type)
{
	AddressInfo addressInfo = GetAbsoluteAddress(addr);
	MemoryOperationInfo operation(addr, value, type, MemoryType::SacMemory);
	InstructionProgress.LastMemOperation = operation;

	//Sound RAM holds the sound processor's program, which the 68000 loads
	if(addressInfo.Type == MemoryType::SacWorkRam || addressInfo.Type == MemoryType::SacSoundRam) {
		_disassembler->InvalidateCache(addressInfo, CpuType::Sac);
	}

	if(_traceLogger->IsEnabled()) {
		_traceLogger->LogNonExec(operation, addressInfo);
	}

	_memoryAccessCounter->ProcessMemoryWrite<accessWidth>(addressInfo, _console->GetMasterClock());
	if(IsEventAddress(addr)) {
		_eventManager->AddEvent(DebugEventType::Register, operation);
	}
	_debugger->ProcessBreakConditions<accessWidth>(CpuType::Sac, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

void SacDebugger::Run()
{
	_step.reset(new StepRequest());
}

void SacDebugger::Step(int32_t stepCount, StepType type)
{
	StepRequest step(type);

	switch(type) {
		case StepType::Step: step.StepCount = stepCount; break;

		case StepType::StepOut:
			step.BreakAddress = _callstackManager->GetReturnAddress();
			step.BreakStackPointer = _callstackManager->GetReturnStackPointer();
			break;

		case StepType::StepOver:
			if(SacDisUtils::IsJumpToSub(_prevOpCode)) {
				step.BreakAddress = _prevProgramCounter + GetPrevOpCodeSize();
				step.BreakStackPointer = _prevStackPointer;
			} else {
				//For any other instruction, step over is the same as step into
				step.StepCount = 1;
			}
			break;

		//The video side is stepped a line at a time (see ProcessPpuCycle)
		case StepType::PpuStep:
		case StepType::PpuScanline: step.PpuStepCount = stepCount; break;
		case StepType::PpuFrame: step.PpuStepCount = SacConstants::ScanlineCount * stepCount; break;
		case StepType::SpecificScanline: step.BreakScanline = stepCount; break;

		default:
			break;
	}

	_step.reset(new StepRequest(step));
}

uint8_t SacDebugger::GetPrevOpCodeSize()
{
	return SacDisUtils::GetOpSize(_prevProgramCounter, MemoryType::SacMemory, _debugger->GetMemoryDumper());
}

void SacDebugger::ProcessCallStackUpdates(AddressInfo& destAddr, uint32_t destPc, uint32_t sp)
{
	if(SacDisUtils::IsJumpToSub(_prevOpCode)) {
		uint8_t opSize = GetPrevOpCodeSize();
		uint32_t returnPc = _prevProgramCounter + opSize;
		if(destPc != returnPc) {
			//JSR/BSR, and PC doesn't match the next instruction, so the call was done
			AddressInfo src = GetAbsoluteAddress(_prevProgramCounter);
			AddressInfo ret = GetAbsoluteAddress(returnPc);
			_callstackManager->Push(src, _prevProgramCounter, destAddr, destPc, ret, returnPc, _prevStackPointer, StackFrameFlags::None);
		}
	} else if(SacDisUtils::IsReturnInstruction(_prevOpCode)) {
		_callstackManager->Pop(destAddr, destPc, sp);

		if(_step->BreakAddress == (int64_t)destPc && _step->BreakStackPointer == (int64_t)sp) {
			//Back at the expected return address - break now for step over/step out
			_step->Break(BreakSource::CpuStep);
		}
	}
}

//The 68000 pushes the return address and the status register before taking an interrupt
void SacDebugger::ProcessInterrupt(uint32_t originalPc, uint32_t currentPc, bool forNmi)
{
	AddressInfo ret = GetAbsoluteAddress(originalPc);
	AddressInfo dest = GetAbsoluteAddress(currentPc);

	if(dest.Type == MemoryType::SacPrgRom && dest.Address >= 0) {
		_codeDataLogger->SetCode(dest.Address, CdlFlags::SubEntryPoint);
	}

	uint32_t originalSp = _cpu->GetState().A[7] + 6;
	_prevStackPointer = originalSp;

	//If a call/return occurred just before the interrupt, it needs to be processed now
	ProcessCallStackUpdates(ret, originalPc, originalSp);
	ResetPrevOpCode();

	_debugger->InternalProcessInterrupt(CpuType::Sac, *this, *_step.get(), ret, originalPc, dest, currentPc, ret, originalPc, originalSp, forNmi);
}

//Called at the start of each line: refreshes the video viewers and handles line and frame steps
void SacDebugger::ProcessPpuCycle()
{
	uint16_t scanline = (uint16_t)_console->GetScanline();
	if(_ppuTools->HasOpenedViewer()) {
		_ppuTools->UpdateViewers(scanline, 0);
	}

	if(_step->HasRequest) {
		if(_step->HasScanlineBreakRequest() && scanline == _step->BreakScanline) {
			_debugger->SleepUntilResume(CpuType::Sac, _step->GetBreakSource());
		} else if(_step->PpuStepCount > 0) {
			_step->PpuStepCount--;
			if(_step->PpuStepCount == 0) {
				_debugger->SleepUntilResume(CpuType::Sac, _step->GetBreakSource());
			}
		}
	}
}

void SacDebugger::GetPpuState(BaseState& state)
{
	SacPpuState& ppu = (SacPpuState&)state;
	ppu.FrameCount = _console->GetFrameCount();
	ppu.Scanline = (uint16_t)_console->GetScanline();
	memcpy(ppu.VideoRegs, _memoryManager->GetVideoRegs(), sizeof(ppu.VideoRegs));
}

PpuTools* SacDebugger::GetPpuTools()
{
	return _ppuTools.get();
}

DebuggerFeatures SacDebugger::GetSupportedFeatures()
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

void SacDebugger::SetProgramCounter(uint32_t addr, bool updateDebuggerOnly)
{
	if(!updateDebuggerOnly) {
		_cpu->SetProgramCounter(addr);
	}
	_prevOpCode = _memoryManager->DebugRead16(addr);
	_prevProgramCounter = addr;
	_prevStackPointer = _cpu->GetState().A[7];
}

uint32_t SacDebugger::GetProgramCounter(bool getInstPc)
{
	return getInstPc ? _prevProgramCounter : _cpu->GetState().PC;
}

uint64_t SacDebugger::GetCpuCycleCount(bool forProfiler)
{
	return _cpu->GetCycleCount();
}

void SacDebugger::ResetPrevOpCode()
{
	_prevOpCode = 0;
}

BaseEventManager* SacDebugger::GetEventManager()
{
	return _eventManager.get();
}

IAssembler* SacDebugger::GetAssembler()
{
	return _assembler.get();
}

CallstackManager* SacDebugger::GetCallstackManager()
{
	return _callstackManager.get();
}

BreakpointManager* SacDebugger::GetBreakpointManager()
{
	return _breakpointManager.get();
}

ITraceLogger* SacDebugger::GetTraceLogger()
{
	return _traceLogger.get();
}

ISerializable* SacDebugger::GetSerializableCpu()
{
	return _cpu;
}

BaseState& SacDebugger::GetState()
{
	return _cpu->GetState();
}

void SacDebugger::ApplyState()
{
	_cpu->ApplyState();
}

bool SacDebugger::SaveRomToDisk(string filename, bool saveAsIps, CdlStripOption stripOption)
{
	vector<uint8_t> output;

	uint8_t* prgRom = _debugger->GetMemoryDumper()->GetMemoryBuffer(MemoryType::SacPrgRom);
	uint32_t prgRomSize = _debugger->GetMemoryDumper()->GetMemorySize(MemoryType::SacPrgRom);
	vector<uint8_t> rom = vector<uint8_t>(prgRom, prgRom + prgRomSize);

	if(saveAsIps) {
		vector<uint8_t> originalRom;
		_emu->GetRomInfo().RomFile.ReadFile(originalRom);
		output = IpsPatcher::CreatePatch(originalRom, rom);
	} else {
		if(stripOption != CdlStripOption::StripNone) {
			_codeDataLogger->StripData(rom.data(), stripOption);
		}
		output = rom;
	}

	ofstream file(filename, ios::out | ios::binary);
	if(file) {
		file.write((char*)output.data(), output.size());
		file.close();
		return true;
	}
	return false;
}

void SacDebugger::ProcessInputOverrides(DebugControllerState inputOverrides[8])
{
	BaseControlManager* controlManager = _console->GetControlManager();
	for(int i = 0; i < 8; i++) {
		shared_ptr<SacController> controller = std::dynamic_pointer_cast<SacController>(controlManager->GetControlDeviceByIndex(i));
		if(controller && inputOverrides[i].HasPressedButton()) {
			controller->SetBitValue(SacController::Buttons::A, inputOverrides[i].A);
			controller->SetBitValue(SacController::Buttons::B, inputOverrides[i].B);
			controller->SetBitValue(SacController::Buttons::X, inputOverrides[i].X);
			controller->SetBitValue(SacController::Buttons::Y, inputOverrides[i].Y);
			controller->SetBitValue(SacController::Buttons::L, inputOverrides[i].L);
			controller->SetBitValue(SacController::Buttons::R, inputOverrides[i].R);
			controller->SetBitValue(SacController::Buttons::Select, inputOverrides[i].Select);
			controller->SetBitValue(SacController::Buttons::Start, inputOverrides[i].Start);
			controller->SetBitValue(SacController::Buttons::Up, inputOverrides[i].Up);
			controller->SetBitValue(SacController::Buttons::Down, inputOverrides[i].Down);
			controller->SetBitValue(SacController::Buttons::Left, inputOverrides[i].Left);
			controller->SetBitValue(SacController::Buttons::Right, inputOverrides[i].Right);
		}
	}
	controlManager->RefreshHubState();
}

template void SacDebugger::ProcessRead<1>(uint32_t addr, uint16_t value, MemoryOperationType type);
template void SacDebugger::ProcessRead<2>(uint32_t addr, uint16_t value, MemoryOperationType type);
template void SacDebugger::ProcessWrite<1>(uint32_t addr, uint16_t value, MemoryOperationType type);
template void SacDebugger::ProcessWrite<2>(uint32_t addr, uint16_t value, MemoryOperationType type);
