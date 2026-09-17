#include "pch.h"
#include "SuperAcan/SacConsole.h"
#include "SuperAcan/SacControlManager.h"
#include "SuperAcan/SacCpu.h"
#include "SuperAcan/SacMemoryManager.h"
#include "SuperAcan/SacPpu.h"
#include "SuperAcan/SacApu.h"
#include "SuperAcan/SacDefaultVideoFilter.h"
#include "Shared/CpuType.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/SettingTypes.h"
#include "Shared/RewindManager.h"
#include "Shared/EventType.h"
#include "Shared/NotificationManager.h"
#include "Shared/MessageManager.h"
#include "Shared/BatteryManager.h"
#include "Shared/Video/VideoDecoder.h"
#include "Shared/RenderedFrame.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/Serializer.h"
#include "Utilities/ArchiveReader.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/VirtualFile.h"
#include "Utilities/PNGHelper.h"
#include "Shared/ColorUtilities.h"

SacConsole::SacConsole(Emulator* emu)
{
	_emu = emu;
}

SacConsole::~SacConsole()
{
	delete[] _prgRom;
	delete[] _workRam;
	delete[] _frameBuffer;
}

LoadRomResult SacConsole::LoadRom(VirtualFile& romFile)
{
	vector<uint8_t> romData;
	romFile.ReadFile(romData);

	//Every cartridge opens with the 68000's reset vectors, and the stack the first one names is
	//in the system's work RAM, which sits at $FC0000 and is mirrored to the end of the address
	//space - so the first word reads $00FC to $00FF (several cartridges stack at $00FFFFFE). Dumps
	//come in both byte orders: some as the 68000 sees them, most with every word swapped (what MAME
	//calls load16_word_swap). That same word says which, and a file where it says neither is not
	//one of these - .bin is claimed by nothing else here, but it is not a format either.
	if(romData.size() < 8 || (romData.size() & 0x01)) {
		return LoadRomResult::Failure;
	}

	auto isCartStart = [](const vector<uint8_t>& data) {
		return data.size() >= 8 && ((data[1] == 0x00 && data[0] >= 0xFC) || (data[0] == 0x00 && data[1] >= 0xFC));
	};
	bool swapped = romData[1] == 0x00 && romData[0] >= 0xFC;
	if(!isCartStart(romData)) {
		return LoadRomResult::Failure;
	}

	//One cartridge has two ROM chips, dumped as two files - 2MB at $000000 and 1MB after it, in the
	//same byte order - and its archive holds both. Loaded alone the first runs until its attract
	//mode reaches data on the second, then clears a zero-length span of video RAM for ever: a flat
	//colour, then black. The other file in the archive, if it is not a cartridge start itself, is
	//that second chip.
	if(romFile.IsArchive()) {
		unique_ptr<ArchiveReader> reader = ArchiveReader::GetReader(romFile.GetFilePath());
		if(reader) {
			vector<string> files = reader->GetFileList();
			if(files.size() == 2) {
				string other = files[0] == romFile.GetFileName() ? files[1] : files[0];
				VirtualFile secondChip(romFile.GetFilePath(), other);
				vector<uint8_t> secondData;
				if(other != romFile.GetFileName() && secondChip.ReadFile(secondData) && !secondData.empty() && !(secondData.size() & 0x01) && !isCartStart(secondData)) {
					MessageManager::Log("[SAC] Second ROM chip: " + other);
					romData.insert(romData.end(), secondData.begin(), secondData.end());
				}
			}
		}
	}

	if(swapped) {
		for(size_t i = 0; i < romData.size(); i += 2) {
			std::swap(romData[i], romData[i + 1]);
		}
	}

	auto readLong = [&](uint32_t addr) {
		return ((uint32_t)romData[addr] << 24) | (romData[addr + 1] << 16) | (romData[addr + 2] << 8) | romData[addr + 3];
	};

	MessageManager::Log("------------------------------");
	MessageManager::Log("File: " + romFile.GetFileName());
	MessageManager::Log("Size: " + std::to_string(romData.size() / 1024) + " KB" + (swapped ? " (word-swapped dump)" : ""));
	MessageManager::Log("Reset vectors: SSP $" + HexUtilities::ToHex32(readLong(0)) + ", PC $" + HexUtilities::ToHex32(readLong(4)));
	MessageManager::Log("------------------------------");

	_prgRomSize = (uint32_t)romData.size();
	_prgRom = new uint8_t[_prgRomSize];
	memcpy(_prgRom, romData.data(), _prgRomSize);
	_emu->RegisterMemory(MemoryType::SacPrgRom, _prgRom, _prgRomSize);

	_workRamSize = SacConstants::WorkRamSize;
	_workRam = new uint8_t[_workRamSize];
	memset(_workRam, 0, _workRamSize);
	_emu->RegisterMemory(MemoryType::SacWorkRam, _workRam, _workRamSize);

	_frameBuffer = new uint16_t[SacConstants::FrameBufferSize];
	memset(_frameBuffer, 0, SacConstants::FrameBufferSize * sizeof(uint16_t));

	_controlManager.reset(new SacControlManager(_emu));
	_memoryManager.reset(new SacMemoryManager(_emu));
	_cpu.reset(new SacCpu(_emu, _memoryManager.get()));
	_memoryManager->Init(this, _cpu.get(), _prgRom, _prgRomSize, _workRam);
	_ppu.reset(new SacPpu());
	_ppu->Init(_memoryManager->GetVideoRam(), _memoryManager->GetPaletteRam(), _memoryManager->GetVideoRegs(), _frameBuffer);
	_apu.reset(new SacApu(_emu, _memoryManager.get(), _memoryManager->GetSoundRam()));
	_memoryManager->SetApu(_apu.get());

	//For the debugger's memory and video viewers. Video RAM and the palette are held as the
	//68000 addresses them, a byte at a time, high byte of each word first.
	_emu->RegisterMemory(MemoryType::SacVideoRam, _memoryManager->GetVideoRam(), 0x20000);
	_emu->RegisterMemory(MemoryType::SacPaletteRam, _memoryManager->GetPaletteRam(), 0x200);
	_emu->RegisterMemory(MemoryType::SacSoundRam, _memoryManager->GetSoundRam(), 0x10000);
	_emu->RegisterMemory(MemoryType::SacSaveRam, _memoryManager->GetSaveRam(), 0x8000);
	//The cartridge's battery-backed RAM, kept in a .sav beside the other consoles' - the byte order
	//Bcan uses too, one byte per word of the 68000's $EC0000 window. One game's load screen shows
	//the hero's portrait only once a record has been saved there. Half the cartridges have no save
	//RAM, so a .sav is only written once the game has used it or when one was already there.
	if(_emu->GetBatteryManager()->GetBatteryFileSize(".sav") > 0) {
		_emu->GetBatteryManager()->LoadBattery(".sav", _memoryManager->GetSaveRam(), 0x8000);
		_memoryManager->MarkSaveRamUsed();
	}
	LoadSoundTables();
	LoadBootRom();

	//From the boot ROM's vectors when there is one, otherwise straight into the cartridge from
	//its own, as the boot ROM would leave it mapped
	_cpu->Reset();
	SacCpuState cpu = _cpu->GetState();
	MessageManager::Log("[SAC] 68000 reset: PC $" + HexUtilities::ToHex24(cpu.PC) + ", SP $" + HexUtilities::ToHex24(cpu.A[7]));

	return LoadRomResult::Success;
}

//The sound processor's half of the machine's internal ROM (MAME's internal6502): the machine
//copies it into the bottom 16KB of sound RAM at reset. It is not code - the games bring their
//own - but a register default and the wave tables the sound code plays from. Looked for as
//MAME keeps it, supracan.zip, in the firmware folder and then beside the emulator.
void SacConsole::LoadSoundTables()
{
	for(string folder : { FolderUtilities::GetFirmwareFolder(), FolderUtilities::GetHomeFolder() }) {
		string zipPath = FolderUtilities::CombinePath(folder, "supracan.zip");
		VirtualFile rom1(zipPath, "internal_6502_1.bin");
		VirtualFile rom2(zipPath, "internal_6502_2.bin");
		if(rom1.IsValid() && rom2.IsValid() && rom1.GetSize() == 0x2000 && rom2.GetSize() == 0x2000) {
			vector<uint8_t> tables;
			vector<uint8_t> second;
			rom1.ReadFile(tables);
			rom2.ReadFile(second);
			tables.insert(tables.end(), second.begin(), second.end());
			_memoryManager->LoadSoundTables(tables);
			MessageManager::Log("[SAC] sound tables loaded from " + zipPath);
			return;
		}
	}
	MessageManager::Log("[SAC] supracan.zip not found - the sound processor starts with empty sound RAM");
}

//The 68000's half of the internal ROM (internal_68k.bin, stored with every word swapped) and the
//lockout chip's key (umc6650.bin), from the same supracan.zip. Without them the machine starts
//the cartridge directly.
void SacConsole::LoadBootRom()
{
	for(string folder : { FolderUtilities::GetFirmwareFolder(), FolderUtilities::GetHomeFolder() }) {
		string zipPath = FolderUtilities::CombinePath(folder, "supracan.zip");
		VirtualFile romFile(zipPath, "internal_68k.bin");
		VirtualFile keyFile(zipPath, "umc6650.bin");
		if(romFile.IsValid() && keyFile.IsValid() && romFile.GetSize() == 0x1000 && keyFile.GetSize() == 0x10) {
			vector<uint8_t> rom;
			vector<uint8_t> key;
			romFile.ReadFile(rom);
			keyFile.ReadFile(key);
			for(size_t i = 0; i < rom.size(); i += 2) {
				std::swap(rom[i], rom[i + 1]);
			}
			_memoryManager->LoadBootRom(rom, key);
			MessageManager::Log("[SAC] boot ROM loaded from " + zipPath);
			return;
		}
	}
	MessageManager::Log("[SAC] no boot ROM (internal_68k.bin and umc6650.bin in supracan.zip) - starting the cartridge directly");
}

//A frame is 262 lines of 570 68000 cycles, with the sound processor at half that clock. MAME's
//scanline callback decides the interrupts at the start of each line: level 7 for vblank on line
//240 (with an NMI for the sound processor), level 4 on every other visible line, each only if
//the UM6619's interrupt mask allows it.
void SacConsole::RunFrame()
{
	for(uint32_t line = 0; line < SacConstants::ScanlineCount; line++) {
		_scanline = line;
		if(line == 0) {
			_emu->ProcessEvent(EventType::StartFrame, CpuType::Sac);
		}
		_emu->ProcessPpuCycle<CpuType::Sac>();

		//A visible line is drawn as the machine reaches it, from the registers as they stand
		_memoryManager->ProcessLineIrqs(line);
		_ppu->RenderLine(line);

		uint8_t irqMask = _memoryManager->GetIrqMask();
		if(line == SacConstants::VblankLine) {
			_memoryManager->ProcessFrcFrame();
			if(irqMask & 0x80) {
				_cpu->SetIrq(7);
				_memoryManager->TriggerSoundNmi();
			}
		} else if(line != 0 && line < SacConstants::VblankLine && (irqMask & 0x10)) {
			_cpu->SetIrq(4);
		}

		_cpuTargetCycle += SacConstants::CpuCyclesPerLine;
		RunCpu(_cpuTargetCycle);

		_soundCpuTargetCycle += SacConstants::SoundCpuCyclesPerLine;
		_memoryManager->RunSoundCpu(_soundCpuTargetCycle);

		//The sound chip, up to the end of the line (the sound processor's clock is 1/12 of the master)
		_apu->Run(_soundCpuTargetCycle * SacConstants::SoundCpuClockDivider);
	}

	SendFrame();
	LogProgress();
}

//Where the 68000 has got to within the line being run, in pixel clocks (0-341)
uint16_t SacConsole::GetLineCycle()
{
	uint64_t lineStart = _cpuTargetCycle >= SacConstants::CpuCyclesPerLine ? _cpuTargetCycle - SacConstants::CpuCyclesPerLine : 0;
	uint64_t now = _cpu->GetCycleCount();
	if(now < lineStart) {
		return 0;
	}
	uint64_t clocks = (now - lineStart) * SacConstants::CpuClockDivider / SacConstants::PixelClockDivider;
	return (uint16_t)std::min<uint64_t>(clocks, SacConstants::ClocksPerScanline - 1);
}

//The 68000 up to a target, stopping on the way wherever the free-running counter interrupts
void SacConsole::RunCpu(uint64_t targetCycle)
{
	while(true) {
		uint64_t frcCycle = _memoryManager->GetFrcTarget();
		if(frcCycle == 0 || frcCycle >= targetCycle) {
			_cpu->RunUntil(targetCycle);
			return;
		}
		_cpu->RunUntil(frcCycle);
		_memoryManager->ProcessFrc(_cpu->GetCycleCount());
	}
}

//Development aid while there is no debugger: where the 68000 is, once a second for the first
//ten seconds
void SacConsole::LogProgress()
{
	if(_frameCount <= 600 && _frameCount % 60 == 0) {
		SacCpuState cpu = _cpu->GetState();
		MessageManager::Log("[SAC] frame " + std::to_string(_frameCount) + ": PC $" + HexUtilities::ToHex24(cpu.PC) +
			" SR $" + HexUtilities::ToHex(cpu.SR) + " SP $" + HexUtilities::ToHex24(cpu.A[7]) +
			" irqmask $" + HexUtilities::ToHex(_memoryManager->GetIrqMask()) + (cpu.Halted ? " HALTED" : "") +
			(_memoryManager->IsSoundCpuRunning() ? " | 6502 PC $" + HexUtilities::ToHex(_memoryManager->GetSoundCpuState().PC) : " | 6502 held"));

		if(_frameCount == 60) {
			//What it is doing there, and what it is holding
			string regs = "[SAC]   ";
			for(int i = 0; i < 8; i++) {
				regs += "D" + std::to_string(i) + "=" + HexUtilities::ToHex32(cpu.D[i]) + " ";
			}
			MessageManager::Log(regs);
			regs = "[SAC]   ";
			for(int i = 0; i < 8; i++) {
				regs += "A" + std::to_string(i) + "=" + HexUtilities::ToHex32(cpu.A[i]) + " ";
			}
			MessageManager::Log(regs);

			//From a little before where it is, so a loop it is sitting at the bottom of shows whole.
			//The first line or two may start mid-instruction.
			uint32_t addr = cpu.PC - 0x20;
			for(int i = 0; i < 20; i++) {
				string text;
				int length = _cpu->Disassemble(addr, text);
				MessageManager::Log("[SAC]   $" + HexUtilities::ToHex24(addr) + ": " + text);
				addr += length > 0 ? length : 2;
			}
		}
	}
}

//Development aid while there is no debugger: SAC_DUMP_FRAMES names frames (comma separated) to
//save as PNG files, named after the frame, in SAC_DUMP_DIR or the home folder
static string GetDevSetting(const char* name)
{
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
	const char* value = std::getenv(name);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
	return value ? string(value) : string();
}

void SacConsole::DumpFrame()
{
	static const string frames = [] {
		string value = GetDevSetting("SAC_DUMP_FRAMES");
		return value.empty() ? value : "," + value + ",";
	}();
	if(frames.empty() || frames.find("," + std::to_string(_frameCount) + ",") == string::npos) {
		return;
	}

	uint32_t width = _ppu->GetScreenWidth();
	uint32_t height = _ppu->GetScreenHeight();
	vector<uint32_t> argb(width * height);
	for(uint32_t i = 0; i < width * height; i++) {
		argb[i] = ColorUtilities::Rgb555ToArgb(_frameBuffer[i]);
	}

	string dir = GetDevSetting("SAC_DUMP_DIR");
	string path = FolderUtilities::CombinePath(dir.empty() ? FolderUtilities::GetHomeFolder() : dir, "sac_frame" + std::to_string(_frameCount) + ".png");
	PNGHelper::WritePNG(path, argb.data(), width, height);
	MessageManager::Log("[SAC] frame " + std::to_string(_frameCount) + " saved to " + path);

	//And the video registers it was drawn with, the ones holding anything
	uint16_t* regs = _memoryManager->GetVideoRegs();
	string line = "[SAC]   vregs";
	for(int i = 0; i < 0x100; i++) {
		if(regs[i]) {
			line += " " + HexUtilities::ToHex((uint16_t)(i * 2)) + "=" + HexUtilities::ToHex(regs[i]);
		}
	}
	MessageManager::Log(line);
}

void SacConsole::SendFrame()
{
	_frameCount++;

	_emu->ProcessEvent(EventType::EndFrame, CpuType::Sac);
	_emu->GetNotificationManager()->SendNotification(ConsoleNotificationType::PpuFrameDone);

	DumpFrame();

	//Both widths fill the same width of a TV (the 320-pixel mode has a faster pixel clock), and
	//the 224-line mode is the middle of the 240 lines, so the frame is sent at one fixed size and
	//the window does not resize when a game changes mode
	_frameBuffer[SacConstants::MaxPixelCount] = (uint16_t)_ppu->GetScreenWidth();
	_frameBuffer[SacConstants::MaxPixelCount + 1] = (uint16_t)_ppu->GetScreenHeight();
	RenderedFrame frame(_frameBuffer, SacConstants::MaxScreenWidth, SacConstants::MaxScreenHeight, 1.0, _frameCount, _controlManager->GetPortStates());
	bool rewinding = _emu->GetRewindManager()->IsRewinding();
	_emu->GetVideoDecoder()->UpdateFrame(frame, rewinding, rewinding);

	_apu->PlayQueuedAudio();

	_emu->ProcessEndOfFrame();

	_controlManager->UpdateControlDevices();
	_controlManager->UpdateInputState();
}

void SacConsole::Reset()
{
	//The machine has a reset button; until the rest of it is here, it starts the machine again
	_emu->ReloadRom(true);
}

void SacConsole::SaveBattery()
{
	if(_memoryManager && _memoryManager->IsSaveRamUsed()) {
		_emu->GetBatteryManager()->SaveBattery(".sav", _memoryManager->GetSaveRam(), 0x8000);
	}
}

BaseControlManager* SacConsole::GetControlManager()
{
	return _controlManager.get();
}

ConsoleRegion SacConsole::GetRegion()
{
	return ConsoleRegion::Ntsc;
}

ConsoleType SacConsole::GetConsoleType()
{
	return ConsoleType::SuperAcan;
}

vector<CpuType> SacConsole::GetCpuTypes()
{
	return { CpuType::Sac, CpuType::SacSound };
}

uint64_t SacConsole::GetMasterClock()
{
	return _cpu->GetCycleCount() * SacConstants::CpuClockDivider;
}

uint32_t SacConsole::GetMasterClockRate()
{
	return SacConstants::MasterClockRate;
}

double SacConsole::GetFps()
{
	return (double)SacConstants::MasterClockRate / SacConstants::MasterClocksPerFrame;
}

BaseVideoFilter* SacConsole::GetVideoFilter(bool getDefaultFilter)
{
	return new SacDefaultVideoFilter(_emu);
}

PpuFrameInfo SacConsole::GetPpuFrame()
{
	PpuFrameInfo frame = {};
	frame.FirstScanline = 0;
	frame.FrameCount = _frameCount;
	//The same fixed size SendFrame gives the video decoder, with the frame's mode after the pixels
	frame.Width = SacConstants::MaxScreenWidth;
	frame.Height = SacConstants::MaxScreenHeight;
	frame.ScanlineCount = SacConstants::ScanlineCount;
	frame.CycleCount = SacConstants::ClocksPerScanline;
	frame.FrameBufferSize = SacConstants::FrameBufferSize * sizeof(uint16_t);
	frame.FrameBuffer = (uint8_t*)_frameBuffer;
	return frame;
}

uint32_t SacConsole::GetFrameCount()
{
	return _frameCount;
}

RomFormat SacConsole::GetRomFormat()
{
	return RomFormat::SuperAcan;
}

AudioTrackInfo SacConsole::GetAudioTrackInfo()
{
	return AudioTrackInfo();
}

void SacConsole::ProcessAudioPlayerAction(AudioPlayerActionParams p)
{
}

AddressInfo SacConsole::GetAbsoluteAddress(AddressInfo& relAddress)
{
	//The sound processor sees nothing but sound RAM
	if(relAddress.Type == MemoryType::SacSoundMemory) {
		return { relAddress.Address & 0xFFFF, MemoryType::SacSoundRam };
	}

	uint32_t addr = relAddress.Address & 0xFFFFFF;
	if(_memoryManager->IsBootRomMapped(addr)) {
		//The boot ROM has no memory type of its own, so the debugger reads it as the 68000 does
		return { -1, MemoryType::None };
	} else if(addr <= SacConstants::CartEnd) {
		return { (int32_t)(addr % _prgRomSize), MemoryType::SacPrgRom };
	} else if(addr >= SacConstants::WorkRamStart) {
		return { (int32_t)(addr & (SacConstants::WorkRamSize - 1)), MemoryType::SacWorkRam };
	} else if(addr >= 0xF40000 && addr <= 0xF5FFFF) {
		return { (int32_t)(addr - 0xF40000), MemoryType::SacVideoRam };
	} else if(addr >= 0xF00200 && addr <= 0xF003FF) {
		return { (int32_t)(addr - 0xF00200), MemoryType::SacPaletteRam };
	} else if(addr >= 0xE80000 && addr <= 0xE8FFFF) {
		return { (int32_t)(addr & 0xFFFF), MemoryType::SacSoundRam };
	} else if(addr >= 0xEC0000 && addr <= 0xECFFFF && (addr & 1)) {
		return { (int32_t)((addr & 0xFFFF) >> 1), MemoryType::SacSaveRam };
	}
	return { -1, MemoryType::None };
}

AddressInfo SacConsole::GetRelativeAddress(AddressInfo& absAddress, CpuType cpuType)
{
	if(cpuType == CpuType::SacSound) {
		return absAddress.Type == MemoryType::SacSoundRam ? AddressInfo { absAddress.Address, MemoryType::SacSoundMemory } : AddressInfo { -1, MemoryType::None };
	}

	switch(absAddress.Type) {
		case MemoryType::SacPrgRom: return { absAddress.Address, MemoryType::SacMemory };
		case MemoryType::SacWorkRam: return { (int32_t)(SacConstants::WorkRamStart + absAddress.Address), MemoryType::SacMemory };
		case MemoryType::SacVideoRam: return { (int32_t)(0xF40000 + absAddress.Address), MemoryType::SacMemory };
		case MemoryType::SacPaletteRam: return { (int32_t)(0xF00200 + absAddress.Address), MemoryType::SacMemory };
		case MemoryType::SacSoundRam: return { (int32_t)(0xE80000 + absAddress.Address), MemoryType::SacMemory };
		case MemoryType::SacSaveRam: return { (int32_t)(0xEC0001 + absAddress.Address * 2), MemoryType::SacMemory };
		default: return { -1, MemoryType::None };
	}
}

void SacConsole::GetConsoleState(BaseState& state, ConsoleType consoleType)
{
	SacState& sacState = (SacState&)state;
	sacState.Cpu = _cpu->GetState();
	sacState.Ppu.FrameCount = _frameCount;
	sacState.Ppu.Scanline = (uint16_t)_scanline;
	memcpy(sacState.Ppu.VideoRegs, _memoryManager->GetVideoRegs(), sizeof(sacState.Ppu.VideoRegs));
	_memoryManager->GetState(sacState.System, sacState.SoundCpu);
	_apu->GetState(sacState.Apu);
}

void SacConsole::Serialize(Serializer& s)
{
	SV(_frameCount);
	SV(_scanline);
	SV(_cpuTargetCycle);
	SV(_soundCpuTargetCycle);
	SV(_cpu);
	SV(_controlManager);
	SV(_memoryManager);
	SV(_apu);
	SVArray(_workRam, _workRamSize);
}
