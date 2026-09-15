using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Interop;
using Mesen.Utilities;
using System;
using System.Text;

namespace Mesen.Debugger.StatusViews
{
	//The Super A'Can's sound processor, a 65C02
	public partial class SacSoundStatusViewModel : BaseConsoleStatusViewModel
	{
		private const byte FlagCarry = 0x01;
		private const byte FlagZero = 0x02;
		private const byte FlagIrqDisable = 0x04;
		private const byte FlagDecimal = 0x08;
		private const byte FlagOverflow = 0x40;
		private const byte FlagNegative = 0x80;

		[ObservableProperty] public partial byte RegA { get; set; }
		[ObservableProperty] public partial byte RegX { get; set; }
		[ObservableProperty] public partial byte RegY { get; set; }
		[ObservableProperty] public partial byte RegSP { get; set; }
		[ObservableProperty] public partial UInt16 RegPC { get; set; }
		[ObservableProperty] public partial byte RegPS { get; set; }

		[ObservableProperty] public partial bool FlagN { get; set; }
		[ObservableProperty] public partial bool FlagV { get; set; }
		[ObservableProperty] public partial bool FlagD { get; set; }
		[ObservableProperty] public partial bool FlagI { get; set; }
		[ObservableProperty] public partial bool FlagZ { get; set; }
		[ObservableProperty] public partial bool FlagC { get; set; }

		[ObservableProperty] public partial string StackPreview { get; private set; } = "";
		[ObservableProperty] public partial string CpuStatus { get; private set; } = "";

		public SacSoundStatusViewModel()
		{
			bool preventUpdate = false;

			this.ObserveProp([nameof(FlagC), nameof(FlagZ), nameof(FlagI), nameof(FlagD), nameof(FlagV), nameof(FlagN)], () => {
				if(!preventUpdate) {
					UpdatePsValue();
				}
			});

			this.ObserveProp(nameof(RegPS), () => {
				preventUpdate = true;
				FlagN = (RegPS & FlagNegative) != 0;
				FlagV = (RegPS & FlagOverflow) != 0;
				FlagD = (RegPS & FlagDecimal) != 0;
				FlagI = (RegPS & FlagIrqDisable) != 0;
				FlagZ = (RegPS & FlagZero) != 0;
				FlagC = (RegPS & FlagCarry) != 0;
				preventUpdate = false;
			});
		}

		//The break and reserved bits are not flags the checkboxes change, so they are kept
		private void UpdatePsValue()
		{
			RegPS = (byte)(
				(RegPS & 0x30) |
				(FlagN ? FlagNegative : 0) |
				(FlagV ? FlagOverflow : 0) |
				(FlagD ? FlagDecimal : 0) |
				(FlagI ? FlagIrqDisable : 0) |
				(FlagZ ? FlagZero : 0) |
				(FlagC ? FlagCarry : 0)
			);
		}

		protected override void InternalUpdateUiState()
		{
			SacSoundCpuState cpu = DebugApi.GetCpuState<SacSoundCpuState>(CpuType.SacSound);

			UpdateCycleCount(cpu.CycleCount);

			RegA = cpu.A;
			RegX = cpu.X;
			RegY = cpu.Y;
			RegSP = cpu.SP;
			RegPC = cpu.PC;
			RegPS = cpu.PS;

			CpuStatus = !cpu.Running ? "held in reset" : cpu.Stopped ? "stopped (STP)" : cpu.Waiting ? "waiting (WAI)" : "running";

			StringBuilder sb = new StringBuilder();
			for(UInt32 i = (UInt32)0x100 + cpu.SP + 1; i < 0x200; i++) {
				sb.Append($"${DebugApi.GetMemoryValue(MemoryType.SacSoundMemory, i):X2} ");
			}
			StackPreview = sb.ToString();
		}

		protected override void InternalUpdateConsoleState()
		{
			SacSoundCpuState cpu = DebugApi.GetCpuState<SacSoundCpuState>(CpuType.SacSound);

			cpu.A = RegA;
			cpu.X = RegX;
			cpu.Y = RegY;
			cpu.SP = RegSP;
			cpu.PC = RegPC;
			cpu.PS = RegPS;

			DebugApi.SetCpuState(cpu, CpuType.SacSound);
		}
	}
}
