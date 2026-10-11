#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/NesConsole.h"
#include "NES/NesCpu.h"
#include "NES/APU/NesApu.h"
#include "NES/NesConstants.h"
#include "NES/Mappers/Bbk/BbkFdc.h"
#include "NES/Mappers/Unlicensed/AudioTape.h"
#include "Shared/NotificationManager.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/Audio/AudioFileReader.h"

//The audio-visual cassette cards: a cartridge that listens to a cassette player while the tape
//plays. The tape's left channel is the lesson's speech, which goes straight to the speaker; the
//right channel carries the pictures and text as a pulse train, which a comparator on the card
//turns into one data bit. Two cards read it, each its own way (mapped as the reference emulator
//has them):
// - the PLQ card (16KB dump: the 8KB RAM at $C000 as it read, then the program at $E000) leaves
//   the timing to the program. A small logic chip latches the data bit among others: writes to
//   $8000-$9FFF clock it, with data bits 0-3 and address lines A1/A2 as its inputs, and reads of
//   $A000-$BFFF return its register, which the program then times by counting its own cycles.
// - the QLS card (32KB dump of $8000-$FFFF) does the timing itself: each falling edge of the data
//   bit raises the IRQ and latches a free-running 8-bit counter clocked at 176.4KHz, which the
//   program reads at $9000-$9FFF (the read takes the IRQ away). 1KB of RAM sits at $8000-$8FFF;
//   $A000-$B7FF reads 0 and the program is at $B800-$FFFF.
//Both carry mapper 172 in their headers, which is Mesen's TXC board, so they are told apart by
//their PRG CRC32.
class PlqAvCard : public BaseMapper
{
private:
	class TapeListener final : public INotificationListener
	{
	private:
		PlqAvCard* _mapper;

	public:
		TapeListener(PlqAvCard* mapper) : _mapper(mapper) {}

		void ProcessNotification(ConsoleNotificationType type, void* parameter) override
		{
			if(type == ConsoleNotificationType::ExecuteShortcut) {
				ExecuteShortcutParams* params = (ExecuteShortcutParams*)parameter;
				switch(params->Shortcut) {
					case EmulatorShortcut::FdsEjectDisk: _mapper->EjectTape(); break;
					case EmulatorShortcut::FdsInsertNextDisk: _mapper->InsertNextTape(); break;
					case EmulatorShortcut::FdsInsertDiskNumber: _mapper->InsertTape(params->Param); break;
					case EmulatorShortcut::TapePlayPause: _mapper->PlayPause(); break;
					case EmulatorShortcut::TapeSeekBack: _mapper->Seek(-10); break;
					case EmulatorShortcut::TapeSeekForward: _mapper->Seek(10); break;
					default: break;
				}
			}
		}
	};

	static constexpr uint32_t PlqPrgCrc = 0x756C66A5;
	static constexpr uint32_t QlsPrgCrc = 0x0E2ADA41;

	bool _isQls = false;
	vector<uint8_t> _ram;
	uint8_t _palReg = 0xFF;

	shared_ptr<TapeListener> _listener;
	shared_ptr<AudioTape> _tape;
	bool _tapeChecked = false;
	//A power cycle builds the card anew: the tape stays in the player, scoped to the card it was
	//played with
	inline static string _persistedTapeRom;
	inline static string _persistedTapePath;

	bool _playing = false;
	uint64_t _tapeTick = 0;
	uint32_t _tickAcc = 0;
	bool _dataIn = false;
	uint32_t _counter = 0;
	uint8_t _latchedCount = 0;

	int16_t _speechPrev = 0;
	int16_t _speechCur = 0;
	uint8_t _speechStep = 0;
	int16_t _lastOutput = 0;

	string GetConfiguredFolder()
	{
		const char* folder = _console->GetNesConfig().BbkDiskFolder;
		return folder[0] ? string(folder) : string();
	}

	void LoadTape(const string& path)
	{
		_tape = AudioTape::Open(path);
		_tapeTick = 0;
		_playing = true;
		_persistedTapeRom = _emu->GetRomInfo().RomFile.GetFilePath();
		_persistedTapePath = path;
	}

	//Puts a tape in at the first clock once the rom's path is known: one opened from the UI with
	//the card, else the one left in by the last session of this card, else "<rom name>.<audio>"
	//beside the rom or in the disk folder
	void CheckForTape()
	{
		//The console clocks the CPU through its power-on reset before the rom's path is filled in
		string romPath = _emu->GetRomInfo().RomFile.GetFilePath();
		if(romPath.empty()) {
			return;
		}
		_tapeChecked = true;

		string pending = BbkFdc::TakePendingBootDisk();
		if(!pending.empty() && AudioFileReader::IsAudioFile(pending)) {
			LoadTape(pending);
			MessageManager::Log("[Tape] Playing " + pending);
			return;
		}

		if(_persistedTapeRom == romPath && !_persistedTapePath.empty() && ifstream(_persistedTapePath)) {
			LoadTape(_persistedTapePath);
			MessageManager::Log("[Tape] Playing " + _persistedTapePath);
			return;
		}

		string baseName = FolderUtilities::GetFilename(romPath, false);
		vector<string> folders = { FolderUtilities::GetFolderName(romPath) };
		string configured = GetConfiguredFolder();
		if(!configured.empty() && configured != folders[0]) {
			folders.push_back(configured);
		}
		for(string& folder : folders) {
			for(const string& ext : AudioFileReader::GetExtensions()) {
				string path = FolderUtilities::CombinePath(folder, baseName + ext);
				if(ifstream(path)) {
					LoadTape(path);
					MessageManager::Log("[Tape] Playing " + path);
					return;
				}
			}
		}
	}

	//One tick of the 176.4KHz clock the tape is read at
	void ClockTick()
	{
		_counter++;

		bool bit = false;
		if(_playing && _tape) {
			if(_tape->IsPast(_tapeTick)) {
				_playing = false;
				MessageManager::DisplayMessage("Tape", "End of tape");
			} else {
				bit = _tape->GetBit(_tapeTick++);
			}
		}

		if(_isQls && _dataIn && !bit) {
			_latchedCount = (uint8_t)_counter;
			_console->GetCpu()->SetIrqSource(IRQSource::External);
		}
		_dataIn = bit;

		//The speech, ramped across the ticks between its samples; a stopped tape fades out rather
		//than holding the level it stopped on
		if(++_speechStep == AudioTape::TicksPerSpeechSample) {
			_speechStep = 0;
			_speechPrev = _speechCur;
			_speechCur = (_playing && _tape) ? _tape->GetSpeech(_tapeTick / AudioTape::TicksPerSpeechSample) : (int16_t)(_speechCur * 15 / 16);
		}
		if(_console->GetApu()->IsApuEnabled()) {
			int32_t level = _speechPrev + (_speechCur - _speechPrev) * _speechStep / (int32_t)AudioTape::TicksPerSpeechSample;
			//On the VRC7 slot, the one expansion channel mixed at 1x (see BbkLpcAudio)
			int16_t out = (int16_t)(level >> 3);
			if(out != _lastOutput) {
				_console->GetApu()->AddExpansionAudioDelta(AudioChannel::VRC7, out - _lastOutput);
				_lastOutput = out;
			}
		}
	}

	//The PLQ card's logic chip, from its equations as the reference emulator has them: D4/D5 are
	//address lines A2/A1, D6 is the data bit
	void WritePal(uint16_t addr, uint8_t data)
	{
		uint8_t D0 = data & 1, D1 = (data >> 1) & 1, D2 = (data >> 2) & 1, D3 = (data >> 3) & 1;
		uint8_t D4 = (addr >> 2) & 1, D5 = (addr >> 1) & 1, D6 = _dataIn ? 1 : 0;
		uint8_t Q0 = _palReg & 1, Q1 = (_palReg >> 1) & 1, Q2 = (_palReg >> 2) & 1, Q3 = (_palReg >> 3) & 1;
		uint8_t Q4 = (_palReg >> 4) & 1, Q5 = (_palReg >> 5) & 1, Q6 = (_palReg >> 6) & 1, Q7 = (_palReg >> 7) & 1;
		uint8_t nD0 = !D0, nD1 = !D1, nD2 = !D2, nD3 = !D3, nD4 = !D4, nD5 = !D5;
		uint8_t nQ0 = !Q0, nQ1 = !Q1, nQ2 = !Q2, nQ3 = !Q3, nQ4 = !Q4, nQ5 = !Q5, nQ6 = !Q6, nQ7 = !Q7;

		uint8_t q0 = (D0 & nD4 & nD5) | (D0 & nQ0 & nD4 & D5) | (nD0 & Q0 & nD4 & D5) | (D0 & D4 & nD5) | (Q0 & D4 & nD5) | (D0 & Q0 & D4 & D5);
		uint8_t q1 = (D1 & nD4 & nD5) | (D1 & nQ1 & nD4 & D5) | (nD1 & Q1 & nD4 & D5) | (D1 & D4 & nD5) | (Q1 & D4 & nD5) | (nD0 & D1 & Q1 & D4 & D5) | (D0 & Q1 & D4 & D5);
		uint8_t q2 = (D2 & nD4 & nD5) | (D2 & nQ2 & nD4 & D5) | (nD2 & Q2 & nD4 & D5) | (D2 & D4 & nD5) | (Q2 & D4 & nD5) | (nD0 & D2 & Q2 & D4 & D5) | (D0 & Q2 & D4 & D5);
		uint8_t q3 = (D3 & nD4 & nD5) | (D3 & nQ3 & nD4 & D5) | (nD3 & Q3 & nD4 & D5) | (D3 & D4 & nD5) | (Q3 & D4 & nD5) | (nD0 & D3 & Q3 & D4 & D5) | (D0 & Q3 & D4 & D5);
		uint8_t q4 = (nD4 & Q4 & nD5) | (D3 & nQ3 & nD4 & D5) | (nD3 & Q3 & nD4 & D5) | (D4 & Q4 & nD5) | (D3 & D4 & nD5) | (D0 & Q3 & D4 & nQ4 & D5) | (D0 & nQ3 & D4 & Q4 & D5) | (nD0 & D4 & D5 & D6);
		uint8_t q5 = (nD4 & nD5 & Q5) | (D2 & nQ2 & nD4 & D5) | (nD2 & Q2 & nD4 & D5) | (D4 & nD5 & Q5) | (D2 & D4 & nD5) | (D0 & Q2 & D4 & D5 & nQ5) | (D0 & nQ2 & D4 & D5 & Q5);
		uint8_t q6 = (nD4 & nD5 & Q6) | (D1 & nQ1 & nD4 & D5) | (nD1 & Q1 & nD4 & D5) | (D4 & nD5 & Q6) | (D1 & D4 & nD5) | (D0 & Q1 & D4 & D5 & nQ6) | (D0 & nQ1 & D4 & D5 & Q6);
		uint8_t q7 = (nD4 & nD5 & Q7) | (D0 & nQ0 & nD4 & D5) | (nD0 & Q0 & nD4 & D5) | (D4 & nD5 & Q7) | (D1 & D4 & nD5) | (D0 & Q0 & D4 & D5 & nQ7) | (D0 & nQ0 & D4 & D5 & Q7);

		//The outputs come back on the data bus in this order
		_palReg = q0 | (q1 << 1) | (q2 << 2) | (q3 << 3) | (q7 << 4) | (q6 << 5) | (q5 << 6) | (q4 << 7);
	}

	void UpdateMapping()
	{
		if(_isQls) {
			//The dump is the whole $8000-$FFFF as it read; the registers sit over its lower part
			for(int i = 0; i < 4; i++) {
				SelectPrgPage(i, i);
			}
		} else {
			SetCpuMemoryMapping(0xC000, 0xDFFF, _ram.data(), 0, (uint32_t)_ram.size(), MemoryAccessType::ReadWrite);
			SelectPrgPage(3, 1);
		}
	}

protected:
	uint16_t GetPrgPageSize() override { return 0x2000; }
	uint16_t GetChrPageSize() override { return 0x2000; }
	bool AllowRegisterRead() override { return true; }
	bool EnableCpuClockHook() override { return true; }

	//Nothing answers at $6000-$7FFF: the cards' RAM is their own, at $8000 or $C000, and the
	//console has none there. The QLS dump's header asks for a battery, which would put 8KB of
	//saved RAM there - and when its program loses the tape it reads on through that range, so
	//whatever the last session left in it decided what came on screen next.
	uint32_t GetWorkRamSize() override { return 0; }
	uint32_t GetSaveRamSize() override { return 0; }
	bool ForceWorkRamSize() override { return true; }
	bool ForceSaveRamSize() override { return true; }

	void InitMapper() override
	{
		_isQls = _romInfo.Hash.PrgCrc32 == QlsPrgCrc;
		_ram.assign(_isQls ? 0x400 : 0x2000, 0);
		_palReg = 0xFF;

		RemoveRegisterRange(0x8000, 0xFFFF, MemoryOperation::Any);
		AddRegisterRange(0x8000, _isQls ? 0xB7FF : 0xBFFF, MemoryOperation::Any);

		SelectChrPage(0, 0);
		SetMirroringType(MirroringType::Horizontal);
		UpdateMapping();

		_tapeChecked = false;
		if(!_listener) {
			_listener.reset(new TapeListener(this));
			_emu->GetNotificationManager()->RegisterNotificationListener(_listener);
		}
	}

	void Reset(bool softReset) override
	{
		_palReg = 0xFF;
		_console->GetCpu()->ClearIrqSource(IRQSource::External);
	}

	void ProcessCpuClock() override
	{
		BaseProcessCpuClock();
		if(!_tapeChecked) {
			CheckForTape();
		}
		uint32_t clockRate = NesConstants::GetClockRate(_console->GetRegion());
		_tickAcc += AudioTape::TickRate;
		while(_tickAcc >= clockRate) {
			_tickAcc -= clockRate;
			ClockTick();
		}
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		if(_isQls) {
			if(addr < 0x9000) {
				return _ram[addr & 0x3FF];
			} else if(addr < 0xA000) {
				_console->GetCpu()->ClearIrqSource(IRQSource::External);
				return _latchedCount;
			}
			return 0;
		}
		return addr >= 0xA000 ? _palReg : 0;
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(_isQls) {
			if(addr < 0x9000) {
				_ram[addr & 0x3FF] = value;
			}
		} else if(addr < 0xA000) {
			WritePal(addr, value);
		}
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);
		SVVector(_ram);
		SV(_palReg);
		SV(_playing);
		SV(_tapeTick);
		SV(_tickAcc);
		SV(_dataIn);
		SV(_counter);
		SV(_latchedCount);
		SV(_speechPrev);
		SV(_speechCur);
		SV(_speechStep);
		SV(_lastOutput);
		if(!s.IsSaving()) {
			UpdateMapping();
		}
	}

public:
	static bool IsAvCard(uint32_t prgCrc) { return prgCrc == PlqPrgCrc || prgCrc == QlsPrgCrc; }

	//1 for the PLQ card, 2 for the QLS one
	int GetCardKind() { return _isQls ? 2 : 1; }

	//Which card plays a tape, by the code its data is recorded in: the 42/84-tick tapes (some of
	//the PLQ brand's own) are the ones the QLS card loses, while on the 48/96-tick ones - every
	//QLS tape, the unbranded ones and the rest of the PLQ brand's - the PLQ card only gets as far
	//as a still picture and the QLS card plays them through. 0 when the tape doesn't say. The
	//decoded tape stays cached for the card that then plays it.
	static int ChooseCardKind(const string& path)
	{
		switch(AudioTape::Open(path)->DetectPulseCode()) {
			case AudioTape::PulseCode::Short: return 1;
			case AudioTape::PulseCode::Long: return 2;
			default: return 0;
		}
	}

	vector<string> GetTapeFileList()
	{
		string folder = GetConfiguredFolder();
		if(folder.empty()) {
			folder = FolderUtilities::GetFolderName(_emu->GetRomInfo().RomFile.GetFilePath());
		}
		vector<string> files = FolderUtilities::GetFilesInFolder(folder, AudioFileReader::GetExtensions(), false);
		std::sort(files.begin(), files.end());
		return files;
	}

	uint32_t GetTapeCount() { return (uint32_t)GetTapeFileList().size(); }

	string GetCurrentTapeFilename() { return _tape ? _tape->GetPath() : ""; }

	//Putting a tape in starts it from the beginning, as pressing play on a rewound cassette does
	void InsertTape(const string& path)
	{
		auto lock = _emu->AcquireLock();
		_tapeChecked = true;
		LoadTape(path);
		MessageManager::DisplayMessage("Tape", "Playing " + FolderUtilities::GetFilename(path, true));
	}

	void InsertTape(uint32_t index)
	{
		vector<string> tapes = GetTapeFileList();
		if(index < tapes.size()) {
			InsertTape(tapes[index]);
		}
	}

	void InsertNextTape()
	{
		vector<string> tapes = GetTapeFileList();
		if(tapes.empty()) {
			return;
		}
		int current = -1;
		for(size_t i = 0; i < tapes.size(); i++) {
			if(tapes[i] == GetCurrentTapeFilename()) {
				current = (int)i;
				break;
			}
		}
		InsertTape(tapes[(current + 1) % tapes.size()]);
	}

	void EjectTape()
	{
		auto lock = _emu->AcquireLock();
		if(_tape) {
			MessageManager::DisplayMessage("Tape", "Tape ejected: " + FolderUtilities::GetFilename(_tape->GetPath(), true));
		}
		_tape.reset();
		_playing = false;
		_tapeChecked = true;
		_persistedTapeRom.clear();
		_persistedTapePath.clear();
	}

	void PlayPause()
	{
		auto lock = _emu->AcquireLock();
		if(!_tape) {
			return;
		}
		if(!_playing && _tape->IsPast(_tapeTick)) {
			_tapeTick = 0;
		}
		_playing = !_playing;
		MessageManager::DisplayMessage("Tape", _playing ? "Play" : "Pause");
	}

	void Seek(int seconds)
	{
		auto lock = _emu->AcquireLock();
		if(!_tape) {
			return;
		}
		int64_t tick = (int64_t)_tapeTick + (int64_t)seconds * AudioTape::TickRate;
		tick = std::max<int64_t>(tick, 0);
		uint64_t length = _tape->GetLength();
		if(length && (uint64_t)tick > length) {
			tick = (int64_t)length;
		}
		_tapeTick = (uint64_t)tick;
		uint32_t pos = (uint32_t)(_tapeTick / AudioTape::TickRate);
		MessageManager::DisplayMessage("Tape", std::to_string(pos / 60) + ":" + (pos % 60 < 10 ? "0" : "") + std::to_string(pos % 60));
	}
};
