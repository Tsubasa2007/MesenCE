#pragma once
#include "pch.h"
#include "Utilities/ISerializable.h"

class Emulator;
class SoundMixer;
class SacMemoryManager;

//The UM6619's sound engine, after MAME's umc6619_sound: 16 voices playing 8-bit samples out of
//sound RAM, each with a pitch, a looping or one-shot length and a stereo volume, and a timer
//that interrupts the sound processor. Its 3.579545 MHz clock is the master clock divided by 15,
//and it puts out a sample every 80 of its clocks. The sound processor reaches it through an
//address and a data port at $420/$422.
class SacApu final : public ISerializable
{
public:
	static constexpr uint32_t MasterClocksPerChipClock = 15;
	static constexpr uint32_t MasterClocksPerSample = MasterClocksPerChipClock * 80;
	static constexpr uint32_t SampleRate = 53693175 / MasterClocksPerSample;
	static constexpr uint32_t MaxSamples = 4000;

private:
	struct Channel
	{
		uint16_t Pitch;
		uint16_t Length;
		uint16_t StartAddr;
		uint16_t CurrAddr;
		uint16_t EndAddr;
		uint32_t AddrIncrement;
		uint32_t Frac;
		uint8_t Register9;
		uint8_t Volume;
		uint8_t VolumeL;
		uint8_t VolumeR;
		bool OneShot;
	};

	SoundMixer* _soundMixer = nullptr;
	SacMemoryManager* _memoryManager = nullptr;
	uint8_t* _soundRam = nullptr;

	Channel _channels[16] = {};
	uint8_t _regs[0x100] = {};
	uint16_t _activeChannels = 0;

	//Positions on the master clock: how far the chip has run, its next sample, its timer
	uint64_t _masterClock = 0;
	uint64_t _nextSampleClock = MasterClocksPerSample;
	bool _timerActive = false;
	uint64_t _timerClock = 0;

	int16_t* _soundBuffer = nullptr;
	uint32_t _sampleCount = 0;

	uint64_t GetTimerPeriod();
	void KeyOn(uint8_t voice);
	void MixSample();

public:
	SacApu(Emulator* emu, SacMemoryManager* memoryManager, uint8_t* soundRam);
	~SacApu();

	void Run(uint64_t masterClock);
	uint8_t Read(uint8_t reg);
	void Write(uint8_t reg, uint8_t value);
	void PlayQueuedAudio();

	void Serialize(Serializer& s) override;
};
