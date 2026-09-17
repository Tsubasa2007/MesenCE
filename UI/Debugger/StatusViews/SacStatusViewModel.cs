using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Interop;
using Mesen.Utilities;
using System;
using System.Text;

namespace Mesen.Debugger.StatusViews;

public partial class SacStatusViewModel : BaseConsoleStatusViewModel
{
	[ObservableProperty] public partial UInt32 RegD0 { get; set; }
	[ObservableProperty] public partial UInt32 RegD1 { get; set; }
	[ObservableProperty] public partial UInt32 RegD2 { get; set; }
	[ObservableProperty] public partial UInt32 RegD3 { get; set; }
	[ObservableProperty] public partial UInt32 RegD4 { get; set; }
	[ObservableProperty] public partial UInt32 RegD5 { get; set; }
	[ObservableProperty] public partial UInt32 RegD6 { get; set; }
	[ObservableProperty] public partial UInt32 RegD7 { get; set; }

	[ObservableProperty] public partial UInt32 RegA0 { get; set; }
	[ObservableProperty] public partial UInt32 RegA1 { get; set; }
	[ObservableProperty] public partial UInt32 RegA2 { get; set; }
	[ObservableProperty] public partial UInt32 RegA3 { get; set; }
	[ObservableProperty] public partial UInt32 RegA4 { get; set; }
	[ObservableProperty] public partial UInt32 RegA5 { get; set; }
	[ObservableProperty] public partial UInt32 RegA6 { get; set; }
	[ObservableProperty] public partial UInt32 RegA7 { get; set; }

	[ObservableProperty] public partial UInt32 RegPC { get; set; }
	[ObservableProperty] public partial UInt32 RegUSP { get; set; }
	[ObservableProperty] public partial UInt32 RegSSP { get; set; }
	[ObservableProperty] public partial UInt16 RegSR { get; set; }

	[ObservableProperty] public partial bool FlagCarry { get; set; }
	[ObservableProperty] public partial bool FlagOverflow { get; set; }
	[ObservableProperty] public partial bool FlagZero { get; set; }
	[ObservableProperty] public partial bool FlagNegative { get; set; }
	[ObservableProperty] public partial bool FlagExtend { get; set; }
	[ObservableProperty] public partial bool FlagSupervisor { get; set; }
	[ObservableProperty] public partial bool FlagTrace { get; set; }
	[ObservableProperty] public partial UInt16 IrqMask { get; set; }

	[ObservableProperty] public partial bool FlagHalted { get; set; }
	[ObservableProperty] public partial bool FlagStopped { get; set; }

	[ObservableProperty] public partial string StackPreview { get; private set; } = "";

	public SacStatusViewModel()
	{
		bool preventUpdate = false;

		this.ObserveProp([
			nameof(FlagCarry), nameof(FlagOverflow), nameof(FlagZero), nameof(FlagNegative), nameof(FlagExtend),
			nameof(FlagSupervisor), nameof(FlagTrace), nameof(IrqMask)
		], () => {
			if(!preventUpdate) {
				UpdateStatusRegister();
			}
		});

		this.ObserveProp(nameof(RegSR), () => {
			preventUpdate = true;
			FlagCarry = (RegSR & 0x01) != 0;
			FlagOverflow = (RegSR & 0x02) != 0;
			FlagZero = (RegSR & 0x04) != 0;
			FlagNegative = (RegSR & 0x08) != 0;
			FlagExtend = (RegSR & 0x10) != 0;
			IrqMask = (UInt16)((RegSR >> 8) & 0x07);
			FlagSupervisor = (RegSR & 0x2000) != 0;
			FlagTrace = (RegSR & 0x8000) != 0;
			preventUpdate = false;
		});
	}

	private void UpdateStatusRegister()
	{
		RegSR = (UInt16)(
			(FlagCarry ? 0x01 : 0) |
			(FlagOverflow ? 0x02 : 0) |
			(FlagZero ? 0x04 : 0) |
			(FlagNegative ? 0x08 : 0) |
			(FlagExtend ? 0x10 : 0) |
			((IrqMask & 0x07) << 8) |
			(FlagSupervisor ? 0x2000 : 0) |
			(FlagTrace ? 0x8000 : 0)
		);
	}

	protected override void InternalUpdateUiState()
	{
		SacCpuState cpu = DebugApi.GetCpuState<SacCpuState>(CpuType.Sac);

		UpdateCycleCount(cpu.CycleCount);

		RegD0 = cpu.D[0];
		RegD1 = cpu.D[1];
		RegD2 = cpu.D[2];
		RegD3 = cpu.D[3];
		RegD4 = cpu.D[4];
		RegD5 = cpu.D[5];
		RegD6 = cpu.D[6];
		RegD7 = cpu.D[7];

		RegA0 = cpu.A[0];
		RegA1 = cpu.A[1];
		RegA2 = cpu.A[2];
		RegA3 = cpu.A[3];
		RegA4 = cpu.A[4];
		RegA5 = cpu.A[5];
		RegA6 = cpu.A[6];
		RegA7 = cpu.A[7];

		RegPC = cpu.PC;
		RegUSP = cpu.USP;
		RegSSP = cpu.SSP;
		RegSR = cpu.SR;

		FlagHalted = cpu.Halted;
		FlagStopped = cpu.Stopped;

		//The 68000 is big-endian: each stack entry is a word, high byte first
		StringBuilder sb = new StringBuilder();
		UInt32 stackAddr = cpu.A[7] & 0xFFFFFF;
		byte[] stackValues = DebugApi.GetMemoryValues(MemoryType.SacMemory, stackAddr, stackAddr + 30 * 2 - 1);
		for(int i = 0; i + 1 < stackValues.Length; i += 2) {
			UInt16 value = (UInt16)((stackValues[i] << 8) | stackValues[i + 1]);
			sb.Append($"${value:X4} ");
		}
		StackPreview = sb.ToString();
	}

	protected override void InternalUpdateConsoleState()
	{
		SacCpuState cpu = DebugApi.GetCpuState<SacCpuState>(CpuType.Sac);

		cpu.D = new UInt32[] { RegD0, RegD1, RegD2, RegD3, RegD4, RegD5, RegD6, RegD7 };
		cpu.A = new UInt32[] { RegA0, RegA1, RegA2, RegA3, RegA4, RegA5, RegA6, RegA7 };
		cpu.PC = RegPC;
		cpu.USP = RegUSP;
		cpu.SSP = RegSSP;
		cpu.SR = RegSR;

		DebugApi.SetCpuState(cpu, CpuType.Sac);
	}
}
