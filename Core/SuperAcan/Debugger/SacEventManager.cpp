#include "pch.h"
#include "SuperAcan/Debugger/SacEventManager.h"
#include "SuperAcan/SacConsole.h"
#include "SuperAcan/SacPpu.h"
#include "SuperAcan/SacTypes.h"
#include "Shared/ColorUtilities.h"
#include "Debugger/Debugger.h"
#include "Debugger/DebugBreakHelper.h"

SacEventManager::SacEventManager(Debugger* debugger, SacConsole* console)
{
	_debugger = debugger;
	_console = console;

	_ppuBuffer = new uint16_t[SacConstants::MaxPixelCount];
	memset(_ppuBuffer, 0, SacConstants::MaxPixelCount * sizeof(uint16_t));
}

SacEventManager::~SacEventManager()
{
	delete[] _ppuBuffer;
}

void SacEventManager::AddEvent(DebugEventType type, MemoryOperationInfo& operation, int32_t breakpointId)
{
	DebugEventInfo evt = {};
	evt.Type = type;
	evt.Flags = (uint32_t)EventFlags::ReadWriteOp;
	evt.Operation = operation;
	evt.Scanline = (int16_t)_console->GetScanline();
	evt.Cycle = _console->GetLineCycle();
	evt.BreakpointId = breakpointId;
	evt.DmaChannel = -1;
	evt.ProgramCounter = _debugger->GetProgramCounter(CpuType::Sac, true);
	_debugEvents.push_back(evt);
}

void SacEventManager::AddEvent(DebugEventType type)
{
	DebugEventInfo evt = {};
	evt.Type = type;
	evt.Scanline = (int16_t)_console->GetScanline();
	evt.Cycle = _console->GetLineCycle();
	evt.BreakpointId = -1;
	evt.DmaChannel = -1;
	evt.ProgramCounter = _debugger->GetProgramCounter(CpuType::Sac, true);
	_debugEvents.push_back(evt);
}

DebugEventInfo SacEventManager::GetEvent(uint16_t y, uint16_t x)
{
	auto lock = _lock.AcquireSafe();

	x /= 2; //convert to cycle value
	y /= 2; //convert to scanline value

	for(DebugEventInfo& evt : _sentEvents) {
		if(evt.Cycle == x && evt.Scanline == y) {
			return evt;
		}
	}

	//If no exact match, extend to the background color
	for(int i = (int)_sentEvents.size() - 1; i >= 0; i--) {
		DebugEventInfo& evt = _sentEvents[i];
		if(std::abs((int)evt.Cycle - (int)x) <= 1 && std::abs((int)evt.Scanline - (int)y) <= 1) {
			return evt;
		}
	}

	DebugEventInfo empty = {};
	empty.ProgramCounter = 0xFFFFFFFF;
	return empty;
}

bool SacEventManager::ShowPreviousFrameEvents()
{
	return _config.ShowPreviousFrameEvents;
}

void SacEventManager::SetConfiguration(BaseEventViewerConfig& config)
{
	_config = (SacEventViewerConfig&)config;
}

//By address, as the 68000 reaches each: the video chip's registers ($F00000), palette ($F00200)
//and video RAM ($F40000), the pads and sound registers in sound RAM ($E80200, $E80400), the
//UM6619 ($E90000) and the lockout chip ($EB0D00)
EventViewerCategoryCfg SacEventManager::GetEventConfig(DebugEventInfo& evt)
{
	switch(evt.Type) {
		default: return {};
		case DebugEventType::Breakpoint: return _config.MarkedBreakpoints;
		case DebugEventType::Irq: return _config.Irq;
		case DebugEventType::Register: {
			bool isWrite = evt.Operation.Type == MemoryOperationType::Write || evt.Operation.Type == MemoryOperationType::DmaWrite;
			uint32_t addr = evt.Operation.Address & 0xFFFFFF;

			if(addr >= 0xF40000 && addr <= 0xF5FFFF) {
				return isWrite ? _config.VramWrite : _config.VramRead;
			} else if(addr >= 0xF00200 && addr <= 0xF003FF) {
				return isWrite ? _config.PaletteWrite : _config.PaletteRead;
			} else if(addr >= 0xF00000 && addr <= 0xF001FF) {
				uint32_t reg = addr & 0x1FF;
				if(!isWrite) {
					return reg <= 0x03 ? _config.VideoStatusRead : _config.VideoOtherRead;
				} else if(reg >= 0x10 && reg <= 0x1F) {
					return _config.VideoDmaWrite;
				} else if(reg >= 0x20 && reg <= 0x27) {
					return _config.VideoSpriteWrite;
				} else if((reg >= 0x100 && reg <= 0x15F && (reg & 0x1F) >= 0x04 && (reg & 0x1F) <= 0x07) || (reg >= 0x184 && reg <= 0x193)) {
					return _config.VideoScrollWrite;
				} else if(reg >= 0x1D0 && reg <= 0x1D5) {
					return _config.VideoWindowWrite;
				}
				return _config.VideoOtherWrite;
			} else if(addr >= 0xE80200 && addr <= 0xE80203) {
				return isWrite ? _config.OtherWrite : _config.InputRead;
			} else if(addr >= 0xE80400 && addr <= 0xE804FF) {
				return isWrite ? _config.SoundWrite : _config.SoundRead;
			} else if(addr >= 0xE90020 && addr <= 0xE9003F) {
				return isWrite ? _config.DmaWrite : _config.DmaRead;
			} else if(addr >= 0xE90010 && addr <= 0xE90011) {
				return isWrite ? _config.IrqWrite : _config.IrqRead;
			} else if(addr >= 0xE90014 && addr <= 0xE90019) {
				return isWrite ? _config.TimerWrite : _config.TimerRead;
			}
			return isWrite ? _config.OtherWrite : _config.OtherRead;
		}
	}
}

void SacEventManager::ConvertScanlineCycleToRowColumn(int32_t& x, int32_t& y)
{
	y *= 2;
	x *= 2;
}

//Lines are drawn as the machine reaches them, so above the current line the buffer holds this
//frame and below it the last one
uint32_t SacEventManager::TakeEventSnapshot(bool forAutoRefresh)
{
	DebugBreakHelper breakHelper(_debugger);
	auto lock = _lock.AcquireSafe();

	SacPpu* ppu = _console->GetPpu();
	_screenWidth = ppu->GetScreenWidth();
	_screenHeight = ppu->GetScreenHeight();
	_firstLine = ppu->GetFirstLine();
	memcpy(_ppuBuffer, _console->GetFrameBuffer(), SacConstants::MaxPixelCount * sizeof(uint16_t));

	_snapshotCurrentFrame = _debugEvents;
	_snapshotPrevFrame = _prevDebugEvents;
	_snapshotScanline = (int16_t)_console->GetScanline();
	_snapshotCycle = _console->GetLineCycle();
	_forAutoRefresh = forAutoRefresh;
	return SacConstants::ScanlineCount;
}

FrameInfo SacEventManager::GetDisplayBufferSize()
{
	FrameInfo size;
	size.Width = ScanlineWidth;
	size.Height = SacConstants::ScanlineCount * 2;
	return size;
}

void SacEventManager::DrawScreen(uint32_t* buffer)
{
	for(uint32_t y = 0, len = _screenHeight * 2; y < len; y++) {
		uint32_t row = _firstLine * 2 + y;
		if(row >= SacConstants::ScanlineCount * 2) {
			break;
		}
		for(uint32_t x = 0, width = _screenWidth * 2; x < width; x++) {
			uint16_t color = _ppuBuffer[(y >> 1) * _screenWidth + (x >> 1)];
			buffer[row * ScanlineWidth + x] = ColorUtilities::Rgb555ToArgb(color);
		}
	}
}
