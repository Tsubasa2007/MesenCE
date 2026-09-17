using Mesen.Debugger.ViewModels;
using Mesen.Interop;
using System;
using System.Collections.Generic;
using static Mesen.Debugger.ViewModels.RegEntry;

namespace Mesen.Debugger.RegisterViewer;

//The Super A'Can's registers, at the addresses the 68000 reaches them: the video chip at $F00000,
//the UM6619 at $E90000 and the sound registers at $E80400 (the sound processor's $0400)
public class SacRegisterViewer
{
	public static List<RegisterViewerTab> GetTabs(ref SacState state)
	{
		return new List<RegisterViewerTab>() {
			GetVideoTab(ref state),
			GetLayerTab(ref state),
			GetSystemTab(ref state),
			GetSoundTab(ref state),
			GetSoundCpuTab(ref state),
		};
	}

	private static UInt16 Reg(ref SacState state, int addr)
	{
		return state.Ppu.VideoRegs[(addr >> 1) & 0xFF];
	}

	//Table and map registers hold a video RAM address in units of 4 bytes
	private static string VramAddress(UInt16 value)
	{
		return "$" + ((value << 2) & 0x1FFFF).ToString("X5");
	}

	private static string LayerSize(UInt16 flags)
	{
		return (flags & 0x0F00) switch {
			0x200 => "16x16 tiles",
			0x600 => "64x32 tiles",
			0xA00 => "128x32 tiles",
			0xC00 => "64x64 tiles",
			_ => "32x32 tiles"
		};
	}

	private static string Depth(int region)
	{
		return region switch {
			0 => "8 bpp",
			1 => "4 bpp",
			2 => "2 bpp",
			_ => "1 bpp"
		};
	}

	private static int Signed12(UInt16 value)
	{
		int v = value & 0xFFF;
		return (v & 0x800) != 0 ? v - 0x1000 : v;
	}

	//The rotate/zoom steps are 8.8 fixed point
	private static RegEntry FixedPoint(string addr, string name, UInt16 value)
	{
		return new RegEntry(addr, name, ((Int16)value / 256.0).ToString("0.####"), value);
	}

	private static string LineTarget(Int32 target)
	{
		return target < 0 ? "None" : target.ToString();
	}

	private static RegisterViewerTab GetVideoTab(ref SacState state)
	{
		SacPpuState ppu = state.Ppu;
		UInt16 flags = Reg(ref state, 0x08);
		UInt16 lineOn = Reg(ref state, 0x0A);
		UInt16 lineOff = Reg(ref state, 0x0C);
		UInt16 spriteDma = Reg(ref state, 0x1E);
		UInt16 window = Reg(ref state, 0x1D0);
		//10-bit signed
		int windowScroll = Reg(ref state, 0x1D4) & 0x3FF;
		if((windowScroll & 0x200) != 0) {
			windowScroll -= 0x400;
		}

		List<RegEntry> entries = new List<RegEntry>() {
			new RegEntry("", "State"),
			new RegEntry("", "Frame Number", ppu.FrameCount),
			new RegEntry("", "Scanline", ppu.Scanline),

			new RegEntry("$F00000", "Status (R)"),
			new RegEntry("$F00000.1", "Odd Frame", (ppu.FrameCount & 1) != 0),
			new RegEntry("$F00000.15", "Vertical Blank", ppu.Scanline >= 240),
			new RegEntry("$F00002", "Scanline (R)", ppu.Scanline, Format.X16),

			new RegEntry("$F00008", "Video Flags", flags, Format.X16),
			new RegEntry("$F00008.1", "Window Enabled", (flags & 0x02) != 0),
			new RegEntry("$F00008.2", "Rotate/Zoom Layer Enabled", (flags & 0x04) != 0),
			new RegEntry("$F00008.3", "Sprites Enabled", (flags & 0x08) != 0),
			new RegEntry("$F00008.5", "Tilemap 2 Enabled", (flags & 0x20) != 0),
			new RegEntry("$F00008.6", "Tilemap 1 Enabled", (flags & 0x40) != 0),
			new RegEntry("$F00008.7", "Tilemap 0 Enabled", (flags & 0x80) != 0),
			new RegEntry("$F00008.8", "Screen Width", (flags & 0x100) != 0 ? "320" : "256", (flags >> 8) & 1),
			new RegEntry("$F00008.9", "Screen Height", (flags & 0x200) != 0 ? "224" : "240", (flags >> 9) & 1),

			new RegEntry("", "Raster Interrupt (level 5)"),
			new RegEntry("$F0000A", "Raise At", lineOn, Format.X16),
			new RegEntry("$F0000A.0-7", "Raise At Line", lineOn & 0xFF),
			new RegEntry("$F0000A.15", "Raise Armed", (lineOn & 0x8000) != 0),
			new RegEntry("$F0000C", "Drop At", lineOff, Format.X16),
			new RegEntry("$F0000C.0-7", "Drop At Line", lineOff & 0xFF),
			new RegEntry("$F0000C.15", "Drop Armed", (lineOff & 0x8000) != 0),
			new RegEntry("", "Pending Raise Line", LineTarget(state.System.LineOnTarget), null),
			new RegEntry("", "Pending Drop Line", LineTarget(state.System.LineOffTarget), null),

			new RegEntry("", "Sprites"),
			new RegEntry("$F00020", "Sprite Table Address", VramAddress(Reg(ref state, 0x20)), Reg(ref state, 0x20)),
			new RegEntry("$F00022", "Sprite Count", Reg(ref state, 0x22) + 1),
			new RegEntry("$F00026.0", "Sprite Depth", Depth((Reg(ref state, 0x26) & 0x01) != 0 ? 0 : 1), Reg(ref state, 0x26) & 0x01),

			new RegEntry("", "Video DMA"),
			new RegEntry("$F00010", "Word Count", Reg(ref state, 0x10) + 1),
			new RegEntry("$F00012-$F00015", "Destination", ((uint)Reg(ref state, 0x12) << 16) | Reg(ref state, 0x14), Format.X32),
			new RegEntry("$F00016", "Destination Step (words)", Reg(ref state, 0x16), Format.X16),
			new RegEntry("$F00018-$F0001B", "Source", ((uint)Reg(ref state, 0x18) << 16) | Reg(ref state, 0x1A), Format.X32),
			new RegEntry("$F0001C", "Source Step (words)", Reg(ref state, 0x1C), Format.X16),
			new RegEntry("$F0001E", "Control", spriteDma, Format.X16),
			new RegEntry("$F0001E.8", "Fill With Zero", (spriteDma & 0x0100) != 0),
			new RegEntry("$F0001E.13-14", "To Video RAM", (spriteDma & 0x6000) != 0),
			new RegEntry("", "Current Source", state.System.SpriteDmaSource, Format.X32),
			new RegEntry("", "Current Destination", state.System.SpriteDmaDest, Format.X32),

			new RegEntry("", "Window"),
			new RegEntry("$F001D0", "Control", window, Format.X16),
			new RegEntry("$F001D0.0-7", "Colour", window & 0xFF),
			new RegEntry("$F001D0.8", "Edges Per Line", (window & 0x0100) != 0),
			new RegEntry("$F001D0.11", "Fill Outside", (window & 0x0800) != 0),
			new RegEntry("$F001D0.13-14", "Priority", (window >> 13) & 0x03),
			new RegEntry("$F001D2", "Edge Table Address", VramAddress(Reg(ref state, 0x1D2)), Reg(ref state, 0x1D2)),
			new RegEntry("$F001D4", "Scroll X", windowScroll.ToString(), Reg(ref state, 0x1D4)),

			new RegEntry("$F001F0.0-2", "Graphics Mode", Reg(ref state, 0x1F0) & 0x07),
		};

		return new RegisterViewerTab("Video", entries, CpuType.Sac, MemoryType.SacMemory);
	}

	private static RegisterViewerTab GetLayerTab(ref SacState state)
	{
		List<RegEntry> entries = new List<RegEntry>();

		int gfxMode = Reg(ref state, 0x1F0) & 0x07;
		int[] layer0Depth = { 2, 1, 0, 1, 0, 0, 0, 0 };
		int[] layer1Depth = { 2, 1, 1, 1, 2, 2, 2, 2 };

		for(int layer = 0; layer < 3; layer++) {
			int addr = 0x100 + layer * 0x20;
			string prefix = "$F00" + addr.ToString("X3");
			string Address(int offset) => "$F00" + (addr + offset).ToString("X3");

			UInt16 flags = Reg(ref state, addr);
			UInt16 tileMode = Reg(ref state, addr + 2);
			UInt16 mode = Reg(ref state, addr + 0x0A);
			int depth = layer == 0 ? layer0Depth[gfxMode] : (layer == 1 ? layer1Depth[gfxMode] : 2);

			entries.AddRange(new List<RegEntry>() {
				new RegEntry("", "Tilemap " + layer),
				new RegEntry(prefix, "Flags", flags, Format.X16),
				new RegEntry(prefix + ".0", "Vertical Flip", (flags & 0x01) != 0),
				new RegEntry(prefix + ".1", "Horizontal Flip", (flags & 0x02) != 0),
				new RegEntry(prefix + ".2-4", "Mosaic", (flags >> 2) & 0x07),
				new RegEntry(prefix + ".5", "Wrap", (flags & 0x20) != 0),
				new RegEntry(prefix + ".8-11", "Size", LayerSize(flags), (flags >> 8) & 0x0F),
				new RegEntry(prefix + ".13-15", "Priority", (flags >> 13) & 0x07),
				new RegEntry("", "Depth", Depth(depth), depth),
				new RegEntry(Address(2), "Tile Mode", tileMode, Format.X16),
				new RegEntry(Address(2) + ".9", "Upper Palettes", (tileMode & 0x0200) != 0),
				new RegEntry(Address(2) + ".11", "Line Select", (tileMode & 0x0800) != 0),
				new RegEntry(Address(2) + ".14", "Line Scroll", (tileMode & 0x4000) != 0),
				new RegEntry(Address(4), "Scroll X", Signed12(Reg(ref state, addr + 4)).ToString(), Reg(ref state, addr + 4)),
				new RegEntry(Address(6), "Scroll Y", Signed12(Reg(ref state, addr + 6)).ToString(), Reg(ref state, addr + 6)),
				new RegEntry(Address(8), "Map Address", VramAddress(Reg(ref state, addr + 8)), Reg(ref state, addr + 8)),
				new RegEntry(Address(0x0A) + ".12-14", "Tile Bank", (mode >> 12) & 0x07),
				new RegEntry(Address(0x0C), "Line Scroll Table", VramAddress(Reg(ref state, addr + 0x0C)), Reg(ref state, addr + 0x0C)),
				new RegEntry(Address(0x0E), "Line Select Table", VramAddress(Reg(ref state, addr + 0x0E)), Reg(ref state, addr + 0x0E)),
			});
		}

		UInt16 rozMode = Reg(ref state, 0x180);
		int[] rozDepth = { 4, 2, 1, 0 };
		entries.AddRange(new List<RegEntry>() {
			new RegEntry("", "Rotate/Zoom Layer"),
			new RegEntry("$F00180", "Mode", rozMode, Format.X16),
			new RegEntry("$F00180.0-1", "Depth", Depth(rozDepth[rozMode & 0x03]), rozMode & 0x03),
			new RegEntry("$F00180.5", "Wrap", (rozMode & 0x20) != 0),
			new RegEntry("$F00180.8-11", "Size", LayerSize(rozMode), (rozMode >> 8) & 0x0F),
			new RegEntry("$F00180.13-15", "Priority", (rozMode >> 13) & 0x07),
			new RegEntry("$F00182.9", "Upper Palettes", (Reg(ref state, 0x182) & 0x0200) != 0),
			new RegEntry("$F00184-$F00187", "Scroll X", ((uint)Reg(ref state, 0x184) << 16) | Reg(ref state, 0x186), Format.X32),
			new RegEntry("$F00188-$F0018B", "Scroll Y", ((uint)Reg(ref state, 0x188) << 16) | Reg(ref state, 0x18A), Format.X32),
			FixedPoint("$F0018C", "X Step Per Pixel", Reg(ref state, 0x18C)),
			FixedPoint("$F00190", "Y Step Per Pixel", Reg(ref state, 0x190)),
			FixedPoint("$F0018E", "X Step Per Line", Reg(ref state, 0x18E)),
			FixedPoint("$F00192", "Y Step Per Line", Reg(ref state, 0x192)),
			new RegEntry("$F00194", "Map Address", VramAddress(Reg(ref state, 0x194)), Reg(ref state, 0x194)),
			new RegEntry("$F00196.12-15", "Tile Bank", (Reg(ref state, 0x196) >> 12) & 0x0F),
			new RegEntry("$F00198", "Line Step Table", VramAddress(Reg(ref state, 0x198)), Reg(ref state, 0x198)),
			new RegEntry("$F0019A", "Line Scroll X Table", VramAddress(Reg(ref state, 0x19A)), Reg(ref state, 0x19A)),
			new RegEntry("$F0019E", "Line Scroll Y Table", VramAddress(Reg(ref state, 0x19E)), Reg(ref state, 0x19E)),
		});

		return new RegisterViewerTab("Tilemaps", entries, CpuType.Sac, MemoryType.SacMemory);
	}

	private static RegisterViewerTab GetSystemTab(ref SacState state)
	{
		SacSystemState sys = state.System;

		List<RegEntry> entries = new List<RegEntry>() {
			new RegEntry("", "Interrupts"),
			new RegEntry("$E90010", "Interrupt Mask", sys.IrqMask, Format.X8),
			new RegEntry("$E90010.4", "Line Interrupt (level 4)", (sys.IrqMask & 0x10) != 0),
			new RegEntry("$E90010.7", "Vertical Blank (level 7)", (sys.IrqMask & 0x80) != 0),
			new RegEntry("", "Lines Held", sys.IrqLines, Format.X8),
			new RegEntry("", "Level 3: Free-Running Counter", (sys.IrqLines & 0x08) != 0),
			new RegEntry("", "Level 4: Line", (sys.IrqLines & 0x10) != 0),
			new RegEntry("", "Level 5: Raster", (sys.IrqLines & 0x20) != 0),
			new RegEntry("", "Level 6: Sound Processor", (sys.IrqLines & 0x40) != 0),
			new RegEntry("", "Level 7: Vertical Blank", (sys.IrqLines & 0x80) != 0),

			new RegEntry("", "Free-Running Counter"),
			new RegEntry("$E90014", "Control", sys.FrcControl, Format.X16),
			new RegEntry("$E90014.8-15", "Enabled ($A2)", (sys.FrcControl & 0xFF00) == 0xA200),
			new RegEntry("$E90014.0-3", "Rate", sys.FrcControl & 0x0F),
			new RegEntry("$E90016", "Frequency", sys.FrcFrequency, Format.X16),

			new RegEntry("", "Control"),
			new RegEntry("$E9001C", "Control", sys.SoundCpuControl, Format.X16),
			new RegEntry("$E9001C.0", "Sound Processor Running", (sys.SoundCpuControl & 0x01) != 0),
			new RegEntry("$E9001C.1", "Boot ROM Out Of $000000", (sys.SoundCpuControl & 0x02) != 0),
			new RegEntry("$E9001C.3", "Boot ROM Out Of $F80000", (sys.SoundCpuControl & 0x08) != 0),
			new RegEntry("", "Boot ROM At $000000", sys.BootRomLow),
			new RegEntry("", "Boot ROM At $F80000", sys.BootRomHigh),
			new RegEntry("", "Lockout Chip Address", sys.LockoutAddress, Format.X8),
		};

		for(int ch = 0; ch < 2; ch++) {
			string Address(int offset) => "$E900" + (0x20 + ch * 0x10 + offset).ToString("X2");
			entries.AddRange(new List<RegEntry>() {
				new RegEntry("", "DMA " + ch),
				new RegEntry(Address(0) + "-" + Address(3), "Source", sys.DmaSource[ch], Format.X32),
				new RegEntry(Address(4) + "-" + Address(7), "Destination", sys.DmaDest[ch], Format.X32),
				new RegEntry(Address(8), "Count", sys.DmaCount[ch] + 1),
			});
		}

		return new RegisterViewerTab("System", entries, CpuType.Sac, MemoryType.SacMemory);
	}

	private static RegisterViewerTab GetSoundTab(ref SacState state)
	{
		SacSystemState sys = state.System;
		SacApuState apu = state.Apu;

		List<RegEntry> entries = new List<RegEntry>() {
			new RegEntry("", "Sound Registers"),
			new RegEntry("$E80402", "Pad 1 Shift Register", sys.SoundShiftRegs[0], Format.X8),
			new RegEntry("$E80403", "Pad 2 Shift Register", sys.SoundShiftRegs[1], Format.X8),
			new RegEntry("$E80407", "Pad Shift Control", sys.SoundShiftControl, Format.X8),
			new RegEntry("", "Pad 1 Latched", sys.LatchedControls[0], Format.X16),
			new RegEntry("", "Pad 2 Latched", sys.LatchedControls[1], Format.X16),
			new RegEntry("$E80410", "Interrupt Enable", sys.SoundIrqEnable, Format.X8),
			new RegEntry("$E80411", "Interrupt Sources", sys.SoundIrqSource, Format.X8),
			new RegEntry("$E80411.5", "68000 Mailbox", (sys.SoundIrqSource & 0x20) != 0),
			new RegEntry("$E80411.6", "Streaming", (sys.SoundIrqSource & 0x40) != 0),
			new RegEntry("$E80411.7", "Timer", (sys.SoundIrqSource & 0x80) != 0),
			new RegEntry("$E80420", "Sound Chip Address (W)", sys.SoundRegAddress, Format.X8),
			new RegEntry("$E80420", "Sound Chip Status (R)", sys.SoundStatus, Format.X8),

			new RegEntry("", "Sound Chip Timer"),
			new RegEntry("", "$11/12: Period", apu.TimerPeriod, Format.X16),
			new RegEntry("", "$14: Control", apu.TimerControl, Format.X8),
			new RegEntry("", "$14.6: Interrupts", (apu.TimerControl & 0x40) != 0),
			new RegEntry("", "Running", apu.TimerActive),
		};

		for(int i = 0; i < 16; i++) {
			SacApuChannelState ch = apu.Channels[i];
			string n = i.ToString("X");
			entries.AddRange(new List<RegEntry>() {
				new RegEntry("", "Voice " + i),
				new RegEntry("", "Playing", ch.Active),
				new RegEntry("", "$2" + n + "/$3" + n + ": Pitch", ch.Pitch, Format.X16),
				new RegEntry("", "$5" + n + ": Length", ch.Length, Format.X16),
				new RegEntry("", "$5" + n + ".0: One-Shot", ch.OneShot),
				new RegEntry("", "$6" + n + "/$7" + n + ": Start", "$" + ((ch.StartAddr << 6) & 0xFFFF).ToString("X4"), ch.StartAddr),
				new RegEntry("", "$9" + n + ": Streaming", ch.Streaming, Format.X8),
				new RegEntry("", "$E" + n + ": Volume (L/R)", ch.Volume, Format.X8),
				new RegEntry("", "Position", ch.CurrAddr, Format.X16),
				new RegEntry("", "End", ch.EndAddr, Format.X16),
			});
		}

		return new RegisterViewerTab("Sound", entries, CpuType.Sac, MemoryType.SacMemory);
	}

	private static RegisterViewerTab GetSoundCpuTab(ref SacState state)
	{
		SacSoundCpuState cpu = state.SoundCpu;

		List<RegEntry> entries = new List<RegEntry>() {
			new RegEntry("", "65C02"),
			new RegEntry("", "Running", cpu.Running),
			new RegEntry("", "PC", cpu.PC, Format.X16),
			new RegEntry("", "A", cpu.A, Format.X8),
			new RegEntry("", "X", cpu.X, Format.X8),
			new RegEntry("", "Y", cpu.Y, Format.X8),
			new RegEntry("", "SP", cpu.SP, Format.X8),
			new RegEntry("", "P", cpu.PS, Format.X8),
			new RegEntry("", "Cycle Count", cpu.CycleCount),
			new RegEntry("", "Waiting (WAI)", cpu.Waiting),
			new RegEntry("", "Stopped (STP)", cpu.Stopped),
			new RegEntry("", "IRQ Line", cpu.IrqLine),
			new RegEntry("", "NMI Pending", cpu.NmiPending),
		};

		return new RegisterViewerTab("Sound CPU", entries, CpuType.Sac, MemoryType.SacMemory);
	}
}
