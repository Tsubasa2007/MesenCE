#pragma once
#include "pch.h"
#include "SuperAcan/SacTypes.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/BaseEventManager.h"

class SacConsole;
class Debugger;

struct SacEventViewerConfig : public BaseEventViewerConfig
{
	EventViewerCategoryCfg VideoScrollWrite;
	EventViewerCategoryCfg VideoWindowWrite;
	EventViewerCategoryCfg VideoSpriteWrite;
	EventViewerCategoryCfg VideoDmaWrite;
	EventViewerCategoryCfg VideoOtherWrite;
	EventViewerCategoryCfg VideoStatusRead;
	EventViewerCategoryCfg VideoOtherRead;
	EventViewerCategoryCfg PaletteWrite;
	EventViewerCategoryCfg PaletteRead;
	EventViewerCategoryCfg VramWrite;
	EventViewerCategoryCfg VramRead;
	EventViewerCategoryCfg SoundWrite;
	EventViewerCategoryCfg SoundRead;
	EventViewerCategoryCfg InputRead;
	EventViewerCategoryCfg DmaWrite;
	EventViewerCategoryCfg DmaRead;
	EventViewerCategoryCfg IrqWrite;
	EventViewerCategoryCfg IrqRead;
	EventViewerCategoryCfg TimerWrite;
	EventViewerCategoryCfg TimerRead;
	EventViewerCategoryCfg OtherWrite;
	EventViewerCategoryCfg OtherRead;

	EventViewerCategoryCfg Irq;
	EventViewerCategoryCfg MarkedBreakpoints;

	bool ShowPreviousFrameEvents;
};

//The event viewer's view of the 68000's accesses to the video chip, the sound registers and the
//UM6619, placed by line and by pixel clock within the line (342 of them)
class SacEventManager final : public BaseEventManager
{
private:
	static constexpr int ScanlineWidth = SacConstants::ClocksPerScanline * 2;

	SacEventViewerConfig _config = {};
	Debugger* _debugger = nullptr;
	SacConsole* _console = nullptr;

	uint16_t* _ppuBuffer = nullptr;
	uint32_t _screenWidth = SacConstants::ScreenWidth;
	uint32_t _screenHeight = SacConstants::ScreenHeight;
	uint32_t _firstLine = 0;

protected:
	bool ShowPreviousFrameEvents() override;
	void ConvertScanlineCycleToRowColumn(int32_t& x, int32_t& y) override;
	void DrawScreen(uint32_t* buffer) override;

public:
	SacEventManager(Debugger* debugger, SacConsole* console);
	~SacEventManager();

	void AddEvent(DebugEventType type, MemoryOperationInfo& operation, int32_t breakpointId = -1) override;
	void AddEvent(DebugEventType type) override;

	EventViewerCategoryCfg GetEventConfig(DebugEventInfo& evt) override;

	uint32_t TakeEventSnapshot(bool forAutoRefresh) override;
	DebugEventInfo GetEvent(uint16_t y, uint16_t x) override;

	FrameInfo GetDisplayBufferSize() override;
	void SetConfiguration(BaseEventViewerConfig& config) override;
};
