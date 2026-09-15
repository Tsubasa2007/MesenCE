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

//The timer interrupts every 5 chip clocks for each step from its 16-bit period up to $10000, and
//bit 6 of its control register reads as a flag that flips each time. The sound drivers depend on
//that flag: every interrupt they read the register and, with bit 6 set, advance the tune (writing
//$4F back), otherwise run the effects and the voices layered over a tune (writing $8F). MAME fires
//every 10 clocks and reads back what was written, so the second half never runs: one cartridge's
//title music leaves its layered voices sounding for ever, and the game waits for them to finish
//before starting the menu tune. Twice MAME's rate with the flag keeps every tune at MAME's tempo,
//which matches Bcan's recordings; all 12 cartridges' drivers alternate the two halves this way.
uint64_t SacApu::GetTimerPeriod()
{
	uint16_t period = (uint16_t)((_regs[0x12] << 8) | _regs[0x11]);
	return (uint64_t)5 * (0x10000 - period) * MasterClocksPerChipClock;
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
			_regs[0x14] ^= 0x40;
			_memoryManager->SetSoundIrqLine(7, true);
			_timerClock += GetTimerPeriod();
		} else {
			MixSample();
			_nextSampleClock += MasterClocksPerSample;
		}
	}

	if(masterClock > _masterClock) {
		_masterClock = masterClock;
	}
}

void SacApu::RestartVoice(uint8_t voice)
{
	Channel& channel = _channels[voice];
	channel.CurrAddr = (uint16_t)(channel.StartAddr << 6);
	channel.EndAddr = (uint16_t)(channel.CurrAddr + channel.Length);
	_activeChannels |= (uint16_t)(1 << voice);
}

//The volume envelope. Nothing documents it (MAME stores $Ax-$Dx as "not yet known"), so this is
//Bcan's, measured from its output with a test cartridge that plays one note at a time and sweeps
//one register at a time - a model of another emulator, not of the hardware:
// $Ax  attack: a straight rise to full volume, within 1 dB after 60/40/20 ms ($00-$08/-$14/-$1F)
// $Bx  first decay: a straight fall to the level set by $Dx's high nibble n, (15 - n) / 16 (halved
//      when $Cx bit 3 is set), taking K / $Bx seconds, K by n's class; $00 holds at full volume
// $Cx  with bit 3 set, a second decay after that level, (C & 7) * 6.1 dB/s
// $Dx  low nibble: the release after key-off, in dB/s (0 keeps the voice sounding)
//The voice goes quiet below 1/256, where Bcan's notes stop.
static constexpr double DecaySecondsTimesB[16] = { 0.42, 0.84, 5.3, 5.3, 5.3, 5.3, 5.3, 5.3, 2.45, 2.45, 2.45, 2.45, 2.45, 2.45, 2.45, 2.45 };
static constexpr double ReleaseDbPerSecond[16] = { 0, 0.16, 0.32, 0.64, 1.29, 2.58, 5.15, 10.3, 20.6, 82, 177, 329, 575, 1150, 1150, 2300 };

static uint32_t GetEnvFactor(double dbPerSecond)
{
	return (uint32_t)(std::pow(10.0, -dbPerSecond / 20.0 / SacApu::SampleRate) * 4294967296.0);
}

void SacApu::EnterPhase(Channel& channel, uint8_t phase)
{
	uint8_t a = channel.Envelope[0];
	uint8_t b = channel.Envelope[1];
	uint8_t c = channel.Envelope[2];
	uint8_t d = channel.Envelope[3];

	channel.Phase = phase;
	switch(phase) {
		case EnvAttack: {
			double seconds = (a <= 0x08 ? 0.06 : (a <= 0x14 ? 0.04 : 0.02)) / 0.89;
			channel.EnvStep = std::max<uint32_t>(1, (uint32_t)(EnvFull / (seconds * SampleRate)));
			break;
		}

		case EnvDecay: {
			uint8_t n = d >> 4;
			channel.EnvTarget = (uint32_t)((15 - n) * (EnvFull / 16) / ((c & 0x08) ? 2 : 1));
			if(b == 0) {
				EnterPhase(channel, EnvSustain);
			} else if(channel.EnvLevel <= channel.EnvTarget) {
				EnterPhase(channel, EnvDecay2);
			} else {
				double samples = DecaySecondsTimesB[n] / b * SampleRate;
				channel.EnvStep = std::max<uint32_t>(1, (uint32_t)((EnvFull - channel.EnvTarget) / samples));
			}
			break;
		}

		case EnvDecay2:
			//Bit 4 asks for the second decay as bit 3 does. The test cartridge this was measured
			//with swept $Cx over $00-$0F only, so bit 4 was never seen; a voice that sets it and is
			//then left alone - never keyed off - would otherwise hold its level for ever and be
			//heard as a drone once the tune has finished.
			if((c & 0x18) && (c & 0x07)) {
				channel.EnvFactor = GetEnvFactor((c & 0x07) * 6.1);
			} else {
				channel.Phase = EnvSustain;
			}
			break;

		case EnvRelease:
			if(ReleaseDbPerSecond[d & 0x0F] > 0) {
				channel.EnvFactor = GetEnvFactor(ReleaseDbPerSecond[d & 0x0F]);
			} else {
				channel.Phase = EnvSustain;
			}
			break;
	}
}

//One sample's worth of the envelope; false once the voice has faded out
bool SacApu::StepEnvelope(Channel& channel)
{
	switch(channel.Phase) {
		case EnvAttack:
			channel.EnvLevel = std::min(EnvFull, channel.EnvLevel + channel.EnvStep);
			if(channel.EnvLevel >= EnvFull) {
				EnterPhase(channel, EnvDecay);
			}
			return true;

		case EnvDecay:
			if(channel.EnvLevel > channel.EnvTarget + channel.EnvStep) {
				channel.EnvLevel -= channel.EnvStep;
			} else {
				channel.EnvLevel = channel.EnvTarget;
				EnterPhase(channel, EnvDecay2);
			}
			break;

		case EnvDecay2:
		case EnvRelease:
			channel.EnvLevel = (uint32_t)(((uint64_t)channel.EnvLevel * channel.EnvFactor) >> 32);
			break;

		default:
			break;
	}
	return channel.EnvLevel >= EnvFull / 256;
}

//Samples are unsigned bytes centred on $80. A voice with register 9 set is fed by the sound
//processor as it plays: at its end it interrupts and starts over from the top of its buffer.
void SacApu::MixSample()
{
	int32_t left = 0;
	int32_t right = 0;

	//Development aid, like SAC_RAW_AUDIO: SAC_MUTE is a mask of voices to leave out of the mix, so
	//a voice can be subtracted from a recording to find which one carries an unwanted sound
	static const uint32_t muteMask = [] {
#pragma warning(push)
#pragma warning(disable: 4996)
		const char* value = std::getenv("SAC_MUTE");
#pragma warning(pop)
		return value ? (uint32_t)strtoul(value, nullptr, 0) : 0u;
	}();

	for(uint8_t i = 0; i < 16 && _activeChannels; i++) {
		if(!(_activeChannels & (1 << i))) {
			continue;
		}

		Channel& channel = _channels[i];
		if(!StepEnvelope(channel)) {
			_activeChannels &= (uint16_t)~(1 << i);
			continue;
		}

		int16_t raw = (int16_t)(uint16_t)((uint8_t)(_soundRam[channel.CurrAddr] + 0x80) << 8);
		int32_t sample = (int32_t)(((int64_t)raw * channel.EnvLevel) >> 24);

		channel.Frac += channel.AddrIncrement;
		channel.CurrAddr += (uint16_t)(channel.Frac >> 16);
		channel.Frac &= 0xFFFF;

		if(!(muteMask & (1u << i))) {
			left += (sample * channel.VolumeL) >> 8;
			right += (sample * channel.VolumeR) >> 8;
		}

		if(channel.CurrAddr >= channel.EndAddr) {
			if(channel.Register9) {
				//A streamed voice starts over from the top of its buffer; its envelope carries on
				_memoryManager->SetSoundIrqLine(6, true);
				RestartVoice(i);
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
			if(reg == 0x14 && (value & 0x80) && !_timerActive) {
				//Bit 7 starts the timer; the drivers set it on every other interrupt, which must not
				//restart a running count or the two halves of their tick fall out of step
				_timerActive = true;
				_timerClock = _masterClock + GetTimerPeriod();
			} else if(reg == 0x17) {
				//Key on/off: the voice in the low nibble, on if any upper bit is set. Key-on starts
				//the envelope from silence; key-off lets it release rather than cutting it.
				uint8_t keyVoice = value & 0x0F;
				Channel& keyChannel = _channels[keyVoice];
				if(value & 0xF0) {
					RestartVoice(keyVoice);
					keyChannel.EnvLevel = 0;
					EnterPhase(keyChannel, EnvAttack);
				} else if(_activeChannels & (1 << keyVoice)) {
					EnterPhase(keyChannel, EnvRelease);
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
			//Bit 7 of this register (MAME keeps the nibble as "unk_upper_05") is still unread. The
			//voices carrying a tune write $6x here; a voice heard as a constant noise under the
			//music writes $Ex, and its sample bytes swing between $01 and $FF where the others sit
			//smoothly around $80. Reading that sample as signed rather than unsigned was tried and
			//made the noise worse, so bit 7 means something else - perhaps a packed sample format,
			//or that the voice is not a sample at all. SAC_MUTE=0x1000 silences it meanwhile.
			break;

		case 0x6: channel.StartAddr = (uint16_t)((channel.StartAddr & 0x00FF) | (value << 8)); break;
		case 0x7: channel.StartAddr = (uint16_t)((channel.StartAddr & 0xFF00) | value); break;
		case 0x9:
			//Ending a stream silences the voice. At the interrupt for its last buffer the driver
			//clears this register, but the voice has already moved on to the other buffer of the
			//pair, which still holds an earlier stretch of the clip; the voices that stream never set
			//a release, so left to key-off that stretch would play again in full - a word said
			//twice, or a syllable repeated at its end.
			if(channel.Register9 && !value) {
				_activeChannels &= (uint16_t)~(1 << voice);
			}
			channel.Register9 = value;
			break;

		case 0xA:
		case 0xB:
		case 0xC:
		case 0xD:
			channel.Envelope[(reg >> 4) - 0xA] = value;
			break;

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
		SVI(_channels[i].Envelope[0]);
		SVI(_channels[i].Envelope[1]);
		SVI(_channels[i].Envelope[2]);
		SVI(_channels[i].Envelope[3]);
		SVI(_channels[i].Phase);
		SVI(_channels[i].EnvLevel);
		SVI(_channels[i].EnvStep);
		SVI(_channels[i].EnvFactor);
		SVI(_channels[i].EnvTarget);
	}
	SVArray(_regs, 0x100);
	SV(_activeChannels);
	SV(_masterClock);
	SV(_nextSampleClock);
	SV(_timerActive);
	SV(_timerClock);
}
