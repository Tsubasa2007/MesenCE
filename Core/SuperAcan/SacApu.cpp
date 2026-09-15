#include "pch.h"
#include "SuperAcan/SacApu.h"
#include "SuperAcan/SacMemoryManager.h"
#include "Shared/Emulator.h"
#include "Shared/Audio/SoundMixer.h"
#include "Utilities/Serializer.h"

SacApu::SacApu(Emulator* emu, SacMemoryManager* memoryManager, uint8_t* soundRam)
{
	_soundMixer = emu->GetSoundMixer();
	_memoryManager = memoryManager;
	_soundRam = soundRam;

	_soundBuffer = new int16_t[MaxSamples * 2];
	memset(_soundBuffer, 0, MaxSamples * 2 * sizeof(int16_t));
}

SacApu::~SacApu()
{
	delete[] _soundBuffer;
}

//The timer counts 10 chip clocks for each step from its 16-bit period up to $10000
uint64_t SacApu::GetTimerPeriod()
{
	uint16_t period = (uint16_t)((_regs[0x12] << 8) | _regs[0x11]);
	return (uint64_t)10 * (0x10000 - period) * MasterClocksPerChipClock;
}

void SacApu::Run(uint64_t masterClock)
{
	while(true) {
		bool timerFirst = _timerActive && _timerClock <= _nextSampleClock;
		uint64_t next = timerFirst ? _timerClock : _nextSampleClock;
		if(next > masterClock) {
			break;
		}

		if(timerFirst) {
			//It interrupts, and carries on, only while bit 6 of its control register is set
			if(_regs[0x14] & 0x40) {
				_memoryManager->SetSoundIrqLine(7, true);
				_timerClock += GetTimerPeriod();
			} else {
				_timerActive = false;
			}
		} else {
			MixSample();
			_nextSampleClock += MasterClocksPerSample;
		}
	}

	if(masterClock > _masterClock) {
		_masterClock = masterClock;
	}
}

void SacApu::KeyOn(uint8_t voice)
{
	Channel& channel = _channels[voice];
	channel.CurrAddr = (uint16_t)(channel.StartAddr << 6);
	channel.EndAddr = (uint16_t)(channel.CurrAddr + channel.Length);
	_activeChannels |= (uint16_t)(1 << voice);
}

//Samples are unsigned bytes centred on $80. A voice with register 9 set is fed by the sound
//processor as it plays: at its end it interrupts and starts over from the top of its buffer.
void SacApu::MixSample()
{
	int32_t left = 0;
	int32_t right = 0;

	for(uint8_t i = 0; i < 16 && _activeChannels; i++) {
		if(!(_activeChannels & (1 << i))) {
			continue;
		}

		Channel& channel = _channels[i];
		int16_t sample = (int16_t)(uint16_t)((uint8_t)(_soundRam[channel.CurrAddr] + 0x80) << 8);

		channel.Frac += channel.AddrIncrement;
		channel.CurrAddr += (uint16_t)(channel.Frac >> 16);
		channel.Frac &= 0xFFFF;

		left += (sample * channel.VolumeL) >> 8;
		right += (sample * channel.VolumeR) >> 8;

		if(channel.CurrAddr >= channel.EndAddr) {
			if(channel.Register9) {
				_memoryManager->SetSoundIrqLine(6, true);
				KeyOn(i);
			} else if(channel.OneShot) {
				_activeChannels &= (uint16_t)~(1 << i);
			} else {
				channel.CurrAddr -= channel.Length;
			}
		}
	}

	if(_sampleCount >= MaxSamples) {
		PlayQueuedAudio();
	}

	//MAME's scale: all 16 voices at full volume fill the range
	_soundBuffer[_sampleCount * 2] = (int16_t)std::clamp(left >> 4, -32768, 32767);
	_soundBuffer[_sampleCount * 2 + 1] = (int16_t)std::clamp(right >> 4, -32768, 32767);
	_sampleCount++;
}

void SacApu::GetState(SacApuState& state)
{
	for(int i = 0; i < 16; i++) {
		Channel& channel = _channels[i];
		SacApuChannelState& out = state.Channels[i];
		out.Pitch = channel.Pitch;
		out.Length = channel.Length;
		out.StartAddr = channel.StartAddr;
		out.CurrAddr = channel.CurrAddr;
		out.EndAddr = channel.EndAddr;
		out.Volume = channel.Volume;
		out.Streaming = channel.Register9;
		out.OneShot = channel.OneShot;
		out.Active = (_activeChannels & (1 << i)) != 0;
	}
	state.TimerPeriod = (uint16_t)((_regs[0x12] << 8) | _regs[0x11]);
	state.TimerControl = _regs[0x14];
	state.TimerActive = _timerActive;
}

//Reading the timer control or the streaming status drops that interrupt
uint8_t SacApu::Read(uint8_t reg)
{
	if(reg == 0x14) {
		_memoryManager->SetSoundIrqLine(7, false);
	} else if(reg == 0x16) {
		_memoryManager->SetSoundIrqLine(6, false);
	}
	return _regs[reg];
}

// Registers $10-$1F are the chip's own; the rest are one per voice, voice in the low nibble:
// $2x/$3x pitch low/high     $5x length (bits 1-3, $40 << n bytes) and one-shot (bit 0)
// $6x/$7x start address / $40, high/low    $9x streaming (see MixSample)
// $Ax-$Dx envelope (unknown, not used)     $Ex volume, left in the high nibble
void SacApu::Write(uint8_t reg, uint8_t value)
{
	_regs[reg] = value;

	uint8_t voice = reg & 0x0F;
	Channel& channel = _channels[voice];

	switch(reg >> 4) {
		case 0x1:
			if(reg == 0x14 && (value & 0x80)) {
				_timerActive = true;
				_timerClock = _masterClock + GetTimerPeriod();
			} else if(reg == 0x17) {
				//Key on/off: the voice in the low nibble, on if any upper bit is set
				if(value & 0xF0) {
					KeyOn(value & 0x0F);
				} else {
					_activeChannels &= (uint16_t)~(1 << (value & 0x0F));
				}
			}
			break;

		case 0x2:
		case 0x3:
			channel.Pitch = (reg >> 4) == 0x2 ? (uint16_t)((channel.Pitch & 0xFF00) | value) : (uint16_t)((channel.Pitch & 0x00FF) | (value << 8));
			channel.AddrIncrement = (uint32_t)channel.Pitch << 6;
			break;

		case 0x5:
			channel.Length = (uint16_t)(0x40 << ((value & 0x0E) >> 1));
			channel.OneShot = (value & 0x01) != 0;
			break;

		case 0x6: channel.StartAddr = (uint16_t)((channel.StartAddr & 0x00FF) | (value << 8)); break;
		case 0x7: channel.StartAddr = (uint16_t)((channel.StartAddr & 0xFF00) | value); break;
		case 0x9: channel.Register9 = value; break;

		case 0xE:
			channel.Volume = value;
			channel.VolumeL = (uint8_t)((value & 0xF0) | (value >> 4));
			channel.VolumeR = (uint8_t)((value & 0x0F) | (value << 4));
			break;
	}
}

void SacApu::PlayQueuedAudio()
{
	//Development aid while there is no debugger: SAC_RAW_AUDIO=<path> writes the chip's output
	//there as raw 16-bit stereo at SampleRate
	static std::ofstream* rawOut = [] {
#pragma warning(push)
#pragma warning(disable: 4996)
		const char* path = std::getenv("SAC_RAW_AUDIO");
#pragma warning(pop)
		return path ? new std::ofstream(path, std::ios::binary) : nullptr;
	}();
	if(rawOut) {
		rawOut->write((const char*)_soundBuffer, _sampleCount * 4);
		rawOut->flush();
	}

	_soundMixer->PlayAudioBuffer(_soundBuffer, _sampleCount, SampleRate);
	_sampleCount = 0;
}

void SacApu::Serialize(Serializer& s)
{
	for(int i = 0; i < 16; i++) {
		SVI(_channels[i].Pitch);
		SVI(_channels[i].Length);
		SVI(_channels[i].StartAddr);
		SVI(_channels[i].CurrAddr);
		SVI(_channels[i].EndAddr);
		SVI(_channels[i].AddrIncrement);
		SVI(_channels[i].Frac);
		SVI(_channels[i].Register9);
		SVI(_channels[i].Volume);
		SVI(_channels[i].VolumeL);
		SVI(_channels[i].VolumeR);
		SVI(_channels[i].OneShot);
	}
	SVArray(_regs, 0x100);
	SV(_activeChannels);
	SV(_masterClock);
	SV(_nextSampleClock);
	SV(_timerActive);
	SV(_timerClock);
}
