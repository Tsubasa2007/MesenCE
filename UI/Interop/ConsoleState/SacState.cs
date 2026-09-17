using System;
using System.Runtime.InteropServices;

namespace Mesen.Interop;

//The 68000's registers, laid out as the core's SacCpuState: PC is the address of the instruction
//being executed, A[7] the active stack pointer, USP/SSP the two stack pointers
public struct SacCpuState : BaseState
{
	public UInt64 CycleCount;
	public UInt32 PC;
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public UInt32[] D;
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public UInt32[] A;
	public UInt32 USP;
	public UInt32 SSP;
	public UInt16 SR;
	[MarshalAs(UnmanagedType.I1)] public bool Halted;
	[MarshalAs(UnmanagedType.I1)] public bool Stopped;
}

//The core's SacPpuState: where the frame is, and the video registers as words
public struct SacPpuState : BaseState
{
	public UInt32 FrameCount;
	public UInt16 Scanline;
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 0x100)] public UInt16[] VideoRegs;
}

//The sound processor's registers, laid out as the core's SacSoundCpuState
public struct SacSoundCpuState : BaseState
{
	public UInt64 CycleCount;
	public UInt16 PC;
	public byte A;
	public byte X;
	public byte Y;
	public byte SP;
	public byte PS;
	[MarshalAs(UnmanagedType.I1)] public bool Running;
	[MarshalAs(UnmanagedType.I1)] public bool Waiting;
	[MarshalAs(UnmanagedType.I1)] public bool Stopped;
	[MarshalAs(UnmanagedType.I1)] public bool IrqLine;
	[MarshalAs(UnmanagedType.I1)] public bool NmiPending;
}

public struct SacSystemState
{
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 2)] public UInt32[] DmaSource;
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 2)] public UInt32[] DmaDest;
	public UInt32 SpriteDmaSource;
	public UInt32 SpriteDmaDest;
	public Int32 LineOnTarget;
	public Int32 LineOffTarget;
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 2)] public UInt16[] DmaCount;
	public UInt16 SoundCpuControl;
	public UInt16 FrcControl;
	public UInt16 FrcFrequency;
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 2)] public UInt16[] LatchedControls;
	public byte IrqMask;
	public byte IrqLines;
	public byte SoundIrqEnable;
	public byte SoundIrqSource;
	public byte SoundShiftControl;
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 2)] public byte[] SoundShiftRegs;
	public byte SoundStatus;
	public byte SoundRegAddress;
	public byte LockoutAddress;
	[MarshalAs(UnmanagedType.I1)] public bool BootRomLow;
	[MarshalAs(UnmanagedType.I1)] public bool BootRomHigh;
}

public struct SacApuChannelState
{
	public UInt16 Pitch;
	public UInt16 Length;
	public UInt16 StartAddr;
	public UInt16 CurrAddr;
	public UInt16 EndAddr;
	public byte Volume;
	public byte Streaming;
	[MarshalAs(UnmanagedType.I1)] public bool OneShot;
	[MarshalAs(UnmanagedType.I1)] public bool Active;
}

public struct SacApuState
{
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)]
	public SacApuChannelState[] Channels;
	public UInt16 TimerPeriod;
	public byte TimerControl;
	[MarshalAs(UnmanagedType.I1)] public bool TimerActive;
}

//Everything the register viewer shows, laid out as the core's SacState
public struct SacState : BaseState
{
	public SacCpuState Cpu;
	public SacPpuState Ppu;
	public SacSoundCpuState SoundCpu;
	public SacSystemState System;
	public SacApuState Apu;
}
