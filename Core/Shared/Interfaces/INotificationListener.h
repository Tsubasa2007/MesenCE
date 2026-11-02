#pragma once
#include "pch.h"

enum class EmulatorShortcut;

enum class ConsoleNotificationType
{
	GameLoaded,
	StateLoaded,
	GameReset,
	GamePaused,
	GameResumed,
	CodeBreak,
	DebuggerResumed,
	PpuFrameDone,
	ResolutionChanged,
	ExecuteShortcut,
	ReleaseShortcut,
	EmulationStopped,
	BeforeEmulationStop,
	ViewerRefresh,
	EventViewerRefresh,
	MissingFirmware,
	SufamiTurboFilePrompt,
	BeforeGameUnload,
	BeforeGameLoad,
	GameLoadFailed,
	CheatsChanged,
	RequestConfigChange,
	RefreshSoftwareRenderer,
	AfterInitConsole,
	NetplayStopped,
	//The YuXing host's browser wants a page: the parameter is a YuxingWebRequestParams
	YuxingWebRequest,
};

struct YuxingWebRequestParams
{
	uint32_t Id;
	const char* Url;
};

struct GameLoadedEventParams
{
	bool IsPaused;
	bool IsPowerCycle;
};

class INotificationListener
{
public:
	virtual void ProcessNotification(ConsoleNotificationType type, void* parameter) = 0;
};

struct ExecuteShortcutParams
{
	EmulatorShortcut Shortcut;
	uint32_t Param;
	void* ParamPtr;
};