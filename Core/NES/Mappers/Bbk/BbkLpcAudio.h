#pragma once
#include "pch.h"
#include "NES/APU/NesApu.h"
#include "NES/NesConsole.h"
#include "NES/NesConstants.h"
#include "Utilities/Serializer.h"

//LPC-10 speech synthesizer used by the BBK learning machine ($FF10/$FF18) and, as a
//variant, the Subor SB-2000 ($4302).
//Ported from the VirtuaNES-BBK fork (NES/ApuEX/LPC_D6_SYNTH.C + MapperBBK LPC glue, author fanoble).
//The original ran the decoder on a worker thread with a blocking byte-feed callback;
//this port runs it synchronously: the CPU pushes bytes into a FIFO and one 200-sample
//frame is decoded whenever enough data is buffered, played back at 10KHz.
//SB-2000 variant differences: streams start with an $0A header (not $D6), the end-of-stream
//marker parks the decoder in a Finished state (readable as "end of speech" status), and a
//$F0 command byte restarts it for the next phrase.
class BbkLpcAudio final : public ISerializable
{
private:
	NesConsole* _console = nullptr;

	static constexpr int LpcOrder = 10;
	static constexpr int SamplesPerFrame = 200;
	static constexpr int FracBits = 15; //Q15
	static constexpr int ClampP = 27500;
	static constexpr int ClampN = -(ClampP + 1);

	static constexpr uint32_t SampleRate = 10000; //10KHz mono
	static constexpr uint32_t SubSteps = 8; //Output is upsampled 8x with linear interpolation to soften the 10KHz staircase

	//The original emulator used a 16-byte FIFO but blocked the CPU thread on writes when
	//full, so no byte was ever lost even if the game ignored the busy flag. That flow
	//control is impossible here, so use a FIFO large enough to hold entire phrases
	//(~350 bytes/sec of speech) - dropping bytes corrupts the bitstream into noise.
	static constexpr uint32_t FifoSize = 0x10000; //Power of 2
	static constexpr uint32_t PreloadBytesNeeded = 14; //Max bytes the 2-frame parameter preload can consume (2x55 bits)
	static constexpr uint32_t FrameBytesNeeded = 8; //Max bytes one frame decode can consume (55 bits)

	//Report "busy" once this much data is queued. This matches the real hardware's
	//16-byte FIFO (15 usable): games throttle their writes on the busy flag, and some
	//interleave speech feeding with timing-critical work like CHR streaming,
	//sizing their copy loop so that rendering is re-enabled during vblank. A
	//different threshold reshapes that loop's timing and the re-enable can land
	//mid-frame (visible garbage). All decode steps must need <= this many bytes or
	//they could deadlock against a game that stops writing while busy.
	static constexpr uint32_t BusyThreshold = 15;

	enum class LpcState : uint8_t
	{
		Reset = 0,
		Startup = 1,
		Run = 2,
		Stopped = 3,
		Finished = 4 //SB-2000 only: end-of-stream reached, waiting for the $F0 restart command
	};

	struct LpcFrame
	{
		int16_t Energy;
		int16_t Pitch;
		int16_t K[LpcOrder];
	};

	static constexpr int16_t _gainTab[16] = {
		0x0000, 0x0100, 0x0200, 0x0300, 0x0400, 0x0500, 0x0700, 0x0B00,
		0x1100, 0x1A00, 0x2900, 0x3F00, 0x5500, 0x7000, 0x7F00, 0x0000
	};

	static constexpr int16_t _pitchTab[128] = {
		0x0000, 0x0100, 0x0104, 0x0108, 0x0110, 0x0114, 0x0118, 0x011C,
		0x0124, 0x0128, 0x012C, 0x0134, 0x0138, 0x0140, 0x0144, 0x014C,
		0x0150, 0x0158, 0x015C, 0x0164, 0x016C, 0x0170, 0x0178, 0x0180,
		0x0184, 0x018C, 0x0194, 0x019C, 0x01A4, 0x01AC, 0x01B4, 0x01BC,
		0x01C4, 0x01CC, 0x01D4, 0x01DC, 0x01E4, 0x01F0, 0x01F8, 0x0200,
		0x020C, 0x0214, 0x021C, 0x0228, 0x0230, 0x023C, 0x0248, 0x0250,
		0x025C, 0x0268, 0x0274, 0x0280, 0x028C, 0x0298, 0x02A4, 0x02B0,
		0x02BC, 0x02C8, 0x02D4, 0x02E4, 0x02F0, 0x0300, 0x030C, 0x031C,
		0x0328, 0x0338, 0x0348, 0x0358, 0x0368, 0x0378, 0x0388, 0x0398,
		0x03A8, 0x03BC, 0x03CC, 0x03DC, 0x03F0, 0x0404, 0x0414, 0x0428,
		0x043C, 0x0450, 0x0464, 0x0478, 0x0490, 0x04A4, 0x04BC, 0x04D0,
		0x04E8, 0x0500, 0x0514, 0x052C, 0x0548, 0x0560, 0x0578, 0x0594,
		0x05AC, 0x05C8, 0x05E4, 0x0600, 0x061C, 0x0638, 0x0654, 0x0674,
		0x0690, 0x06B0, 0x06D0, 0x06F0, 0x0710, 0x0734, 0x0754, 0x0778,
		0x079C, 0x07C0, 0x07E4, 0x0808, 0x082C, 0x0854, 0x087C, 0x08A4,
		0x08CC, 0x08F8, 0x0920, 0x094C, 0x0978, 0x09A4, 0x09D0, 0x0A00
	};

	//The reflection-coefficient tables hold negative Q15 values. They are written as signed
	//literals rather than as raw unsigned bit patterns behind a truncating (int16_t) cast:
	//that cast needed a C4310 suppression, and the Linux clang targets now build with
	//-Werror, where an MSVC-only pragma is itself an error.
	static constexpr int16_t _k1Tab[64] = {
		-0x7F00, -0x7DC0, -0x7CC0, -0x7B80, -0x7A40, -0x7900, -0x77C0, -0x7640,
		-0x74C0, -0x7340, -0x71C0, -0x7000, -0x6E40, -0x6C80, -0x6A80, -0x68C0,
		-0x6680, -0x6480, -0x6280, -0x6040, -0x5E00, -0x5BC0, -0x5940, -0x56C0,
		-0x5480, -0x5200, -0x4F40, -0x4C80, -0x49C0, -0x4700, -0x4400, -0x40C0,
		-0x3DC0, -0x3A80, -0x3740, -0x33C0, -0x3040, -0x2C80, -0x2880, -0x24C0,
		-0x20C0, -0x1C80, -0x1840, -0x1400, -0x0FC0, -0x0B40, -0x0640, -0x0140,
		0x0440, 0x09C0, 0x0F40, 0x1580, 0x1C80, 0x2380, 0x2AC0, 0x3280,
		0x3A80, 0x42C0, 0x4B80, 0x5400, 0x5C40, 0x6500, 0x6E00, 0x7880
	};

	static constexpr int16_t _k2Tab[64] = {
		-0x7600, -0x6800, -0x5C40, -0x5240, -0x4B80, -0x4580, -0x4000, -0x3B00,
		-0x3640, -0x31C0, -0x2D40, -0x2940, -0x2540, -0x2180, -0x1E00, -0x1A40,
		-0x16C0, -0x1340, -0x1000, -0x0CC0, -0x0980, -0x0640, -0x0300, 0x0000,
		0x0340, 0x0640, 0x0940, 0x0C40, 0x0F40, 0x1280, 0x1580, 0x1880,
		0x1B80, 0x1E80, 0x2180, 0x24C0, 0x27C0, 0x2AC0, 0x2DC0, 0x30C0,
		0x3400, 0x3700, 0x3A40, 0x3D00, 0x4000, 0x4300, 0x4600, 0x4900,
		0x4C00, 0x4F40, 0x5240, 0x5540, 0x5840, 0x5B40, 0x5E00, 0x6100,
		0x63C0, 0x6680, 0x6940, 0x6C00, 0x6F00, 0x7200, 0x7640, 0x7C00
	};

	static constexpr int16_t _k3Tab[32] = {
		-0x7500, -0x6600, -0x5E00, -0x5700, -0x5100, -0x4B00, -0x4500, -0x4000,
		-0x3B00, -0x3600, -0x3100, -0x2C00, -0x2700, -0x2200, -0x1E00, -0x1900,
		-0x1400, -0x0F00, -0x0A00, -0x0500, 0x0100, 0x0700, 0x0D00, 0x1400,
		0x1A00, 0x2200, 0x2900, 0x3200, 0x3B00, 0x4500, 0x5300, 0x6D00
	};

	static constexpr int16_t _k4Tab[32] = {
		-0x6C00, -0x5000, -0x3E00, -0x3500, -0x2D00, -0x2700, -0x2100, -0x1B00,
		-0x1600, -0x1100, -0x0C00, -0x0700, -0x0200, 0x0300, 0x0700, 0x0C00,
		0x1100, 0x1500, 0x1A00, 0x1F00, 0x2400, 0x2900, 0x2E00, 0x3300,
		0x3800, 0x3E00, 0x4400, 0x4B00, 0x5300, 0x5A00, 0x6400, 0x7400
	};

	static constexpr int16_t _k5Tab[16] = {
		-0x5D00, -0x3B00, -0x2C00, -0x2000, -0x1600, -0x0D00, -0x0400, 0x0400,
		0x0C00, 0x1500, 0x1E00, 0x2700, 0x3100, 0x3D00, 0x4C00, 0x6600
	};

	static constexpr int16_t _k6Tab[16] = {
		-0x5600, -0x2900, -0x1900, -0x0E00, -0x0400, 0x0500, 0x0D00, 0x1400,
		0x1C00, 0x2400, 0x2D00, 0x3600, 0x4000, 0x4A00, 0x5500, 0x6A00
	};

	static constexpr int16_t _k7Tab[16] = {
		-0x5D00, -0x3800, -0x2900, -0x1D00, -0x1300, -0x0B00, -0x0300, 0x0500,
		0x0D00, 0x1400, 0x1D00, 0x2600, 0x3100, 0x3C00, 0x4B00, 0x6700
	};

	static constexpr int16_t _k8Tab[8] = {
		-0x3B00, -0x1C00, -0x0A00, 0x0500, 0x1400, 0x2700, 0x3E00, 0x5800
	};

	static constexpr int16_t _k9Tab[8] = {
		-0x4700, -0x2400, -0x1400, -0x0700, 0x0400, 0x1000, 0x1F00, 0x4500
	};

	static constexpr int16_t _k10Tab[8] = {
		-0x3D00, -0x1A00, -0x0D00, -0x0300, 0x0600, 0x1100, 0x1E00, 0x4300
	};

	static constexpr int16_t _excitTab[160] = {
		0x00A2, 0x00AF, 0x00BA, 0x00C2, 0x00C7, 0x00C9, 0x00CA, 0x00C6,
		0x00C2, 0x00BC, 0x00B5, 0x00AD, 0x00A5, 0x009E, 0x009A, 0x0095,
		0x0095, 0x0098, 0x009F, 0x00A8, 0x00B8, 0x00CA, 0x00E3, 0x00FE,
		0x011F, 0x0141, 0x0169, 0x0191, 0x01BD, 0x01E8, 0x0216, 0x0240,
		0x026C, 0x0292, 0x02B9, 0x02D9, 0x02F8, 0x030F, 0x0325, 0x0332,
		0x033F, 0x0343, 0x0347, 0x0345, 0x0345, 0x033F, 0x033D, 0x033A,
		0x033D, 0x0341, 0x034E, 0x035F, 0x037B, 0x03A0, 0x03D2, 0x040D,
		0x0457, 0x04AD, 0x0511, 0x0582, 0x0600, 0x068A, 0x071F, 0x07BD,
		0x0864, 0x0911, 0x09C1, 0x0A74, 0x0B26, 0x0BD5, 0x0C7F, 0x0D20,
		0x0DB7, 0x0E40, 0x0EBB, 0x0F24, 0x0F7A, 0x0FBC, 0x0FE9, 0x0FFF,
		0x0FFF, 0x0FE9, 0x0FBC, 0x0F7A, 0x0F24, 0x0EBB, 0x0E40, 0x0DB7,
		0x0D20, 0x0C7F, 0x0BD5, 0x0B26, 0x0A74, 0x09C1, 0x0911, 0x0864,
		0x07BD, 0x071F, 0x068A, 0x0600, 0x0582, 0x0511, 0x04AD, 0x0457,
		0x040D, 0x03D2, 0x03A0, 0x037B, 0x035F, 0x034E, 0x0341, 0x033D,
		0x033A, 0x033D, 0x033F, 0x0345, 0x0345, 0x0347, 0x0343, 0x033F,
		0x0332, 0x0325, 0x030F, 0x02F8, 0x02D9, 0x02B9, 0x0292, 0x026C,
		0x0240, 0x0216, 0x01E8, 0x01BD, 0x0191, 0x0169, 0x0141, 0x011F,
		0x00FE, 0x00E3, 0x00CA, 0x00B8, 0x00A8, 0x009F, 0x0098, 0x0095,
		0x0095, 0x009A, 0x009E, 0x00A5, 0x00AD, 0x00B5, 0x00BC, 0x00C2,
		0x00C6, 0x00CA, 0x00C9, 0x00C7, 0x00C2, 0x00BA, 0x00AF, 0x00A2
	};

	//SB-2000 variant flag (see the class comment) and its end-of-speech status flag
	bool _sb2k = false;
	bool _speechEnd = false;

	//Byte FIFO fed by $FF18 writes
	uint8_t _fifo[FifoSize] = {};
	uint32_t _fifoReadPos = 0;
	uint32_t _fifoWritePos = 0;

	//Synth state
	LpcFrame _framePrev = {};
	LpcFrame _frameCurr = {};
	LpcFrame _frameNext = {};

	bool _needInterp = false;
	int16_t _randomSeed = -1; //0xFFFF
	int16_t _currPitch = 0;
	int16_t _sampleIndex = 0;
	int16_t _x[LpcOrder + 1] = {};
	int16_t _synthOut = 0;
	LpcState _state = LpcState::Startup;

	//Bit reader
	int8_t _bitsLeft = 0;
	uint16_t _dataCache = 0;
	bool _magicFound = false;

	//Decoded frame being played back
	int16_t _pcmBuffer[SamplesPerFrame] = {};
	int16_t _pcmPos = SamplesPerFrame; //Exhausted

	//Playback timing/output
	uint32_t _cycleAcc = 0;
	uint8_t _regFF10 = 0;
	uint8_t _subStep = 0;
	int16_t _prevSample = 0;
	int16_t _currSample = 0;
	int16_t _lastOutput = 0;

	uint32_t GetFifoCount()
	{
		return (_fifoWritePos - _fifoReadPos) & (FifoSize - 1);
	}

	bool IsFifoFull()
	{
		return ((_fifoWritePos + 1) & (FifoSize - 1)) == _fifoReadPos;
	}

	static uint8_t ByteRev(uint8_t a)
	{
		a = (a >> 4) | (a << 4);
		a = ((a & 0xCC) >> 2) | ((a & 0x33) << 2);
		a = ((a & 0xAA) >> 1) | ((a & 0x55) << 1);
		return a;
	}

	//Reads bits from the FIFO-backed bitstream (bits <= 8).
	//Unlike the original blocking feed callback, an empty FIFO reads as zero bits;
	//callers ensure enough data is buffered before starting a decode step.
	int16_t GetBits(uint8_t bits)
	{
		if(_state == LpcState::Reset || _state == LpcState::Stopped) {
			return -1;
		}

		if(_bitsLeft < bits) {
			_dataCache <<= 8;
			if(GetFifoCount() > 0) {
				_dataCache |= ByteRev(_fifo[_fifoReadPos]);
				_fifoReadPos = (_fifoReadPos + 1) & (FifoSize - 1);
			}
			_bitsLeft += 8;
		}

		uint16_t data = _dataCache >> (_bitsLeft - bits);
		data &= (1 << bits) - 1;
		_bitsLeft -= bits;

		return (int16_t)data;
	}

	//Returns -1 on end of stream
	int GetFrame(LpcFrame& dst, LpcFrame& ref)
	{
		int16_t energyIdx = GetBits(4);
		if(energyIdx == 0) {
			//Silent frame
			dst.Energy = 0;
			dst.Pitch = 0;
			memset(dst.K, 0, sizeof(dst.K));
			return 0;
		}

		if(energyIdx == 15) {
			//End of stream
			return -1;
		}

		int16_t repeat = GetBits(1);
		int16_t pitchIdx = GetBits(7);

		dst.Energy = _gainTab[energyIdx & 0x0F];
		dst.Pitch = _pitchTab[pitchIdx & 0x7F];

		if(repeat) {
			dst.K[0] = ref.K[0];
			dst.K[1] = ref.K[1];
			dst.K[2] = ref.K[2];
			dst.K[3] = ref.K[3];
		} else {
			dst.K[0] = _k1Tab[GetBits(6) & 0x3F];
			dst.K[1] = _k2Tab[GetBits(6) & 0x3F];
			dst.K[2] = _k3Tab[GetBits(5) & 0x1F];
			dst.K[3] = _k4Tab[GetBits(5) & 0x1F];
		}

		if(pitchIdx == 0) {
			//Unvoiced frame
			dst.K[4] = dst.K[5] = dst.K[6] = dst.K[7] = dst.K[8] = dst.K[9] = 0;
		} else {
			//Voiced frame
			if(repeat) {
				memcpy(dst.K, ref.K, sizeof(ref.K));
			} else {
				dst.K[4] = _k5Tab[GetBits(4) & 0x0F];
				dst.K[5] = _k6Tab[GetBits(4) & 0x0F];
				dst.K[6] = _k7Tab[GetBits(4) & 0x0F];
				dst.K[7] = _k8Tab[GetBits(3) & 0x07];
				dst.K[8] = _k9Tab[GetBits(3) & 0x07];
				dst.K[9] = _k10Tab[GetBits(3) & 0x07];
			}
		}

		return 0;
	}

	void SetInterpFlag()
	{
		_needInterp = true;

		if(_framePrev.Energy == 0) {
			if(_frameNext.Pitch == 0 && _frameNext.Energy) {
				_needInterp = false;
			}
		} else {
			if(_framePrev.Pitch) {
				if(_frameNext.Pitch == 0 && _frameNext.Energy) {
					_needInterp = false;
				}
			} else {
				if(_frameNext.Pitch) {
					_needInterp = false;
				}
			}
		}
	}

	void ResetSynth()
	{
		_framePrev = {};
		_frameCurr = {};
		_frameNext = {};
		_needInterp = false;
		_randomSeed = -1;
		_currPitch = 0;
		_sampleIndex = 0;
		memset(_x, 0, sizeof(_x));
		_synthOut = 0;
		_bitsLeft = 0;
		_dataCache = 0;
		_magicFound = false;
		_state = LpcState::Startup;
	}

	//Startup part 2: once the magic byte was found, decode the two initial frames
	void PreloadFrames()
	{
		_state = LpcState::Run;

		GetFrame(_framePrev, _framePrev);
		GetFrame(_frameNext, _framePrev);

		_frameCurr = _framePrev;

		SetInterpFlag();
	}

	int16_t ReloadPitch()
	{
		int16_t currPitch = _currPitch;

		if(_needInterp) {
			int32_t ratio = ((int32_t)_sampleIndex << FracBits) / SamplesPerFrame;

			int16_t* vector0 = (int16_t*)&_framePrev;
			int16_t* vector1 = (int16_t*)&_frameNext;
			int16_t* vector2 = (int16_t*)&_frameCurr;

			for(size_t i = 0; i < sizeof(LpcFrame) / sizeof(int16_t); i++) {
				int16_t v = vector1[i] - vector0[i];
				v = (int16_t)(((int32_t)v * ratio) >> FracBits);
				vector2[i] = v + vector0[i];
			}

			currPitch += _frameCurr.Pitch;
			if(currPitch > 0) {
				return currPitch;
			}
		}

		if(_frameCurr.Pitch != 0) {
			return currPitch + _frameCurr.Pitch;
		}

		return 0x80;
	}

	void RunFilter()
	{
		int16_t* x = _x;
		int16_t* k = _frameCurr.K;
		int32_t sample = _synthOut;

		for(int i = 0; i < LpcOrder; i++) {
			int index = LpcOrder - 1 - i;

			sample -= ((int32_t)k[index] * x[index]) >> FracBits;

			if(sample > ClampP) {
				sample = ClampP;
			} else if(sample < ClampN) {
				sample = ClampN;
			}

			x[index + 1] = x[index] + (int16_t)(((int32_t)sample * k[index]) >> FracBits);
		}

		x[0] = _synthOut = (int16_t)sample;
	}

	int16_t RandomGen()
	{
		int16_t seed = _randomSeed << 1;
		int16_t r = ((seed >> 12) ^ (seed >> 13)) & 1;
		_randomSeed = seed | r;
		return r;
	}

	//Synthesizes one 200-sample frame into _pcmBuffer, then decodes the next frame's parameters
	void SynthesizeFrame()
	{
		for(int i = 0; i < SamplesPerFrame; i++) {
			_sampleIndex = (int16_t)i;
			_currPitch -= 16;

			if(_currPitch < 0) {
				_currPitch = ReloadPitch();
			}

			int32_t excit;
			if(_frameCurr.Pitch == 0) {
				//Unvoiced
				if(_frameCurr.Energy) {
					excit = RandomGen() ? 1408 : -1408;
					excit = (excit * _frameCurr.Energy) >> FracBits;
				} else {
					//Silent
					excit = 0;
				}
			} else if(_currPitch >= 160) {
				excit = 0;
			} else {
				//Voiced excitation
				excit = _excitTab[_currPitch];
				excit = (excit * _frameCurr.Energy) >> FracBits;
			}

			excit *= 8;

			_synthOut = (int16_t)excit;
			RunFilter();

			_pcmBuffer[i] = _synthOut;
		}

		//Prepare the next frame
		_framePrev = _frameNext;
		_frameCurr = _frameNext;

		if(GetFrame(_frameNext, _framePrev)) {
			//End of stream marker. BBK: stay in Run state, like the original decoder.
			//It keeps consuming the bitstream (zero bytes decode as silent frames)
			//until the game resets it through $FF10. Switching to a stopped state
			//here would halt FIFO draining and hang games that stream the next
			//phrase while polling the $FF18 busy flag.
			//SB-2000: park in the Finished state - the software polls the "end of
			//speech" status nibble and then restarts the decoder with a $F0 command.
			if(_sb2k) {
				_bitsLeft = 0;
				_dataCache = 0;
				_state = LpcState::Finished;
				_speechEnd = true;
			}
		}

		SetInterpFlag();
	}

	//Runs one decode step if the playback buffer is exhausted and enough data is available.
	//Preload and frame synthesis run on separate ticks, and a decode step NEVER runs with
	//fewer bytes than it could consume: the bit reader fabricates zero bits on an empty
	//FIFO, which would shift the bit alignment and turn the rest of the stream into noise.
	//(The stream tail this can leave unconsumed is just the terminator - no audio is lost;
	//the game resets the decoder via $FF10 before the next phrase anyway.)
	void TryDecodeStep()
	{
		if(_pcmPos < SamplesPerFrame) {
			return;
		}

		uint32_t count = GetFifoCount();

		if(_state == LpcState::Finished) {
			//SB-2000: scan for the $F0 restart command, one byte per tick. On restart the
			//ring buffer is flushed (like the reference emulator) - the software sends the
			//command, re-checks the status and only then streams the next phrase.
			if(count >= 1) {
				if(GetBits(8) == 0xF0) { //0xF0 == bit-reversed $0F, the byte the CPU writes
					ResetSynth();
					_fifoReadPos = _fifoWritePos;
					_speechEnd = false;
				}
			}
			return;
		}

		if(_state == LpcState::Startup) {
			if(!_magicFound) {
				//Byte-aligned scan for the stream header ($D6 on the BBK, $0A on the
				//SB-2000), one byte per tick
				uint8_t magic = _sb2k ? 0x50 : 0x6B; //bit-reversed
				if(count >= 1) {
					if(GetBits(8) == magic) {
						_magicFound = true;
					}
				}
			} else if(count >= PreloadBytesNeeded) {
				PreloadFrames();
			}
			return; //First frame is synthesized on the next tick
		}

		if(_state == LpcState::Run) {
			if(count >= FrameBytesNeeded) {
				SynthesizeFrame();
				_pcmPos = 0;
			}
		}
	}

public:
	void Clock()
	{
		if(!_console->GetApu()->IsApuEnabled()) {
			return;
		}

		//8 interpolation substeps per 10KHz sample (80KHz output steps); the original
		//player relied on the OS resampler to smooth the 10KHz output, so plain
		//zero-order hold here sounds noticeably harsher
		_cycleAcc += SampleRate * SubSteps;
		uint32_t clockRate = NesConstants::GetClockRate(_console->GetRegion());
		if(_cycleAcc >= clockRate) {
			_cycleAcc -= clockRate;

			_subStep = (_subStep + 1) % SubSteps;
			if(_subStep == 0) {
				//Fetch the next 10KHz sample; ramp from the previous one across the substeps
				_prevSample = _currSample;

				TryDecodeStep();

				if(_pcmPos < SamplesPerFrame) {
					_currSample = _pcmBuffer[_pcmPos++];
				} else {
					//Waiting for data: decay towards silence to avoid holding a DC offset
					_currSample = (int16_t)(_currSample * 15 / 16);
				}
			}

			int32_t interp = _prevSample + (((int32_t)(_currSample - _prevSample) * _subStep) / (int32_t)SubSteps);

			//Output on the VRC7 slot: it is the only expansion channel mixed at 1x.
			//The other slots get multiplied by the mixer (FDS x20, N163 x20, ...) which
			//would push the +/-27500 synth range far past int16 and wrap around (loud
			//crackling). +/-27500 >> 3 = +/-3437, comparable to full APU music volume.
			int16_t out = (int16_t)(interp >> 3);
			if(out != _lastOutput) {
				_console->GetApu()->AddExpansionAudioDelta(AudioChannel::VRC7, out - _lastOutput);
				_lastOutput = out;
			}
		}
	}

protected:
	void Serialize(Serializer& s) override
	{
		SVArray(_fifo, FifoSize);
		SV(_fifoReadPos); SV(_fifoWritePos);

		SV(_framePrev.Energy); SV(_framePrev.Pitch);
		SV(_frameCurr.Energy); SV(_frameCurr.Pitch);
		SV(_frameNext.Energy); SV(_frameNext.Pitch);
		SVArray(_framePrev.K, LpcOrder);
		SVArray(_frameCurr.K, LpcOrder);
		SVArray(_frameNext.K, LpcOrder);

		SV(_needInterp); SV(_randomSeed); SV(_currPitch); SV(_sampleIndex);
		SVArray(_x, LpcOrder + 1);
		SV(_synthOut); SV(_state); SV(_bitsLeft); SV(_dataCache); SV(_magicFound);

		SVArray(_pcmBuffer, SamplesPerFrame);
		SV(_pcmPos);

		SV(_cycleAcc); SV(_regFF10); SV(_lastOutput);
		SV(_subStep); SV(_prevSample); SV(_currSample);
		SV(_speechEnd);
	}

public:
	BbkLpcAudio(NesConsole* console, bool sb2kVariant = false)
	{
		_console = console;
		_sb2k = sb2kVariant;
	}

	void Reset()
	{
		_fifoReadPos = _fifoWritePos = 0;
		_regFF10 = 0;
		_pcmPos = SamplesPerFrame;
		_cycleAcc = 0;
		_subStep = 0;
		_prevSample = 0;
		_currSample = 0;
		_speechEnd = false;
		ResetSynth();
	}

	//SB-2000 $4302 status: whether the end-of-stream marker was reached, and whether the
	//input FIFO can take more data
	bool IsSpeechEnd() { return _speechEnd; }
	bool IsFull() { return IsFifoFull(); }

	//$FF10 write (SpeakInitPort): bit 0 rising edge resets the decoder.
	//Unlike the original (whose decoder free-ran ahead of real time and had usually
	//drained its buffer by reset time), this port is locked to 10KHz and may still
	//hold stale bytes of the previous phrase - flush them, otherwise the magic-byte
	//scan can false-sync on old data and decode the entire next phrase as noise.
	void WriteControl(uint8_t value)
	{
		if(_regFF10 == 0 && (value & 0x01)) {
			ResetSynth();
			_pcmPos = SamplesPerFrame;
			_fifoReadPos = _fifoWritePos = 0;
		}
		_regFF10 = value & 0x01;
	}

	//$FF18 write (SpeakDataPort)
	void WriteData(uint8_t value)
	{
		if(!IsFifoFull()) {
			_fifo[_fifoWritePos] = value;
			_fifoWritePos = (_fifoWritePos + 1) & (FifoSize - 1);
		}
	}

	//$FF18 read: busy/idle status
	uint8_t ReadStatus()
	{
		return GetFifoCount() >= BusyThreshold ? 0x00 : 0x8F;
	}
};
