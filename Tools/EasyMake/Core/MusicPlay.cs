namespace EasyMake.Core;

/// <summary>Plays a music listing the way the machine does: the INSTALL.CMD driver's frame routine
/// ($45F3, one call per 50 Hz Dendy frame), ported step by step, writes the APU registers of a
/// model of the NES sound chip (the BIOS sets the 5-step frame counter, $4017 = $C0). The first
/// song plays once through: until every track has ended or taken its unconditional loop.</summary>
public static class MusicPlay
{
	public const int Rate = 44100;
	const double Cpu = 1773448;          // Dendy CPU clock
	const int FrameCycles = 35464;       // 312 lines x 341 dots / 3

	static readonly int[] Periods = { 0x6AE, 0x64E, 0x5F4, 0x59E, 0x54D, 0x501, 0x4B9, 0x475, 0x435, 0x3F9, 0x3C0, 0x38A };
	static readonly int[] Kinds = { 0x00, 0x01, 0x82, 0x43 };   // $4A13: bit 7 triangle, bit 6 noise

	/// <summary>The driver's tables from $4A17: vibrato offsets (8 shapes x 16), decay envelope
	/// levels (shapes 1-9 x 16, from $4A97), then the 16 x 16 volume product table ($4B27).
	/// Envelope shapes index from $4A87, so a shape number past 9 reads on into the volume table,
	/// as the driver does.</summary>
	static readonly byte[] Tables = {
		0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0xFF,
		0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
		0x00, 0x01, 0x02, 0x01, 0x00, 0xFF, 0xFE, 0xFF, 0x00, 0x01, 0x02, 0x01, 0x00, 0xFF, 0xFE, 0xFF,
		0x00, 0x00, 0x01, 0x01, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFE, 0xFE, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE,
		0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
		0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
		0xF0, 0xE0, 0xD0, 0xC0, 0xB0, 0xA0, 0x90, 0x80, 0x70, 0x60, 0x50, 0x40, 0x30, 0x20, 0x10, 0x00,
		0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0,
		0xF0, 0xE0, 0xD0, 0xC0, 0xB0, 0xA0, 0x90, 0x80, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0,
		0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0xF0, 0xE0, 0xD0, 0xC0, 0xB0, 0xA0, 0x90, 0x80,
		0xF0, 0xD0, 0xB0, 0x90, 0x70, 0x50, 0x30, 0x10, 0xE0, 0xC0, 0xA0, 0x80, 0x60, 0x40, 0x20, 0x00,
		0xF0, 0xE0, 0xD0, 0xC0, 0xC0, 0xD0, 0xE0, 0xD0, 0xC0, 0xA0, 0x80, 0x60, 0x40, 0x20, 0x10, 0x00,
		0xF0, 0xD0, 0xB0, 0x90, 0xA0, 0xB0, 0x90, 0x70, 0x60, 0x50, 0x40, 0x30, 0x20, 0x10, 0x10, 0x00,
		0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x60, 0x60, 0x70, 0x80, 0xA0, 0xC0, 0xF0, 0xB0, 0x80,
		0xF0, 0xF0, 0xA0, 0x80, 0xF0, 0xF0, 0xA0, 0x80, 0x70, 0x70, 0x60, 0x60, 0x50, 0x50, 0x40, 0x20,
		0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
		0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31,
		0x30, 0x30, 0x30, 0x30, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x32, 0x32, 0x32, 0x32,
		0x30, 0x30, 0x30, 0x31, 0x31, 0x31, 0x31, 0x31, 0x32, 0x32, 0x32, 0x32, 0x32, 0x33, 0x33, 0x33,
		0x30, 0x30, 0x31, 0x31, 0x31, 0x31, 0x32, 0x32, 0x32, 0x32, 0x33, 0x33, 0x33, 0x33, 0x34, 0x34,
		0x30, 0x30, 0x31, 0x31, 0x31, 0x32, 0x32, 0x32, 0x33, 0x33, 0x33, 0x34, 0x34, 0x34, 0x35, 0x35,
		0x30, 0x30, 0x31, 0x31, 0x32, 0x32, 0x32, 0x33, 0x33, 0x34, 0x34, 0x34, 0x35, 0x35, 0x36, 0x36,
		0x30, 0x30, 0x31, 0x31, 0x32, 0x32, 0x33, 0x33, 0x34, 0x34, 0x35, 0x35, 0x36, 0x36, 0x37, 0x37,
		0x30, 0x31, 0x31, 0x32, 0x32, 0x33, 0x33, 0x34, 0x34, 0x35, 0x35, 0x36, 0x36, 0x37, 0x37, 0x38,
		0x30, 0x31, 0x31, 0x32, 0x32, 0x33, 0x34, 0x34, 0x35, 0x35, 0x36, 0x37, 0x37, 0x38, 0x38, 0x39,
		0x30, 0x31, 0x31, 0x32, 0x33, 0x33, 0x34, 0x35, 0x35, 0x36, 0x37, 0x37, 0x38, 0x39, 0x39, 0x3A,
		0x30, 0x31, 0x31, 0x32, 0x33, 0x34, 0x34, 0x35, 0x36, 0x37, 0x37, 0x38, 0x39, 0x3A, 0x3A, 0x3B,
		0x30, 0x31, 0x32, 0x32, 0x33, 0x34, 0x35, 0x36, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3A, 0x3B, 0x3C,
		0x30, 0x31, 0x32, 0x33, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3A, 0x3B, 0x3C, 0x3D,
		0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
		0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
	};
	const int VibBase = 0x4A17, EnvBase = 0x4A87, VolBase = 0x4B27;
	static int Tab(int addr) => addr - VibBase < Tables.Length ? Tables[addr - VibBase] : 0;

	public static float[] Render(string text, double secondsMax = 120) => Render(text, secondsMax, null);

	/// <summary>log, if given, receives every APU register write as (frame, address, value)</summary>
	public static float[] Render(string text, double secondsMax, Action<int, int, int> log)
	{
		var d = new Driver(Music.Assemble(text, 0x8000));
		var apu = new Apu();
		d.Write = (a, v) => { log?.Invoke(d.Frame, a, v); apu.Write(a, v); };
		if(!d.Start(0)) return Array.Empty<float>();
		var o = new List<float>();
		int maxFrames = (int)(secondsMax * Cpu / FrameCycles), tail = -1;
		for(int f = 0; f < maxFrames && tail != 0; f++) {
			d.Tick();
			if(tail < 0 && d.Finished) tail = 25;
			else if(tail > 0) tail--;
			apu.Run(FrameCycles, o);
		}
		return o.ToArray();
	}

	public static void WriteWav(string path, float[] s)
	{
		Lpc.WriteWav(path, s.Select(v => (int)(Math.Clamp(v, -1f, 1f) * 30000)).ToList(), Rate);
	}

	/// <summary>One of the driver's eight track slots ($4406 + slot x $15); field comments give
	/// the offset in the slot</summary>
	sealed class Slot
	{
		public int State = 0xFF;            // +0  event number; 0 = start, $FF = off
		public int Flags;                   // +1  bits 0-1 APU channel, bit 7 vibrato, bits 4-6 its shape
		public int Ptr;                     // +2  track data
		public int Reload, Acc;             // +4 +5 tempo: $10 per frame, a tick on underflow
		public int Vol;                     // +6  $4000-style duty/volume value
		public int Len;                     // +7  ticks left in the note
		public int Sweep;                   // +8
		public int LoopA, LoopB;            // +9 +10 loop counters
		public int Shape, ShapeLen, ShapePos = 0xFE; // +11 +12 +13 decay envelope (RAM leftovers: decayed)
		public int Slide, SlidePeriod, SlideCount; // +14 +15 +16 volume slide
		public int PeriodLo;                // +17 vibrato centre
		public int Mark;                    // +18 the $FD loop mark
		public int VibDelay, VibCount;      // +19 +20
		public bool Looped;                 // (preview) took its unconditional loop or ended
	}

	sealed class Driver
	{
		readonly byte[] data;
		readonly Slot[] slots = Enumerable.Range(0, 8).Select(_ => new Slot()).ToArray();
		int busy, enabled, frameCount, scratch;
		public int Frame;
		public Action<int, int> Write;

		public Driver(byte[] data) { this.data = data; }

		int Rd(int a) => a - 0x8000 is var i && i >= 0 && i < data.Length ? data[i] : 0xFF;
		int Rd16(int a) => Rd(a) | Rd(a + 1) << 8;

		public bool Finished => slots.All(s => s.State == 0xFF || s.Looped) && slots.Any(s => s.Looped);

		/// <summary>$4545: start a song (its tracks into their slots)</summary>
		public bool Start(int song)
		{
			int st = Rd16(0x8000), tt = Rd16(0x8002);
			int cnt = Rd(st + 2 * song), first = Rd(st + 2 * song + 1);
			if(cnt == 0xFF) return false;
			for(int i = 0; i <= Math.Min(cnt, 3); i++) {
				int e = tt + 4 * ((first + i) & 0xFF), x = Rd(e);
				if(x % 0x15 != 0 || x / 0x15 > 7) continue;
				var s = slots[x / 0x15];
				if(s.State != 0xFF) enabled &= Mask(s.Flags & 3) ^ 0x0F;
				s.Flags = Rd(e + 1);
				s.Ptr = Rd16(e + 2);
				s.State = 0;
			}
			return true;
		}

		static int Mask(int ch) => 1 << ch;
		static int Swap(int b) => (((b << 4) | (b >> 4)) + 0x0F) & 0xFF;

		/// <summary>$45F3: once per frame</summary>
		public void Tick()
		{
			busy = 0;
			frameCount = (frameCount + 1) & 0xFF;
			foreach(var s in slots) {
				if(s.State == 0xFF) continue;
				int ch = s.Flags & 3;
				if(s.State == 0) {
					Init(s);
				} else {
					Vibrato(s);
					EnvelopeFrame(s);
					s.ShapePos = (s.ShapePos + 1) & 0xFF;
					if(s.ShapePos >= s.ShapeLen) s.ShapePos = s.ShapeLen;
					s.Acc = (s.Acc - 0x10) & 0xFF;
					if(s.Acc < 0x80) {
						WriteVol(s);
					} else {
						s.Acc = (s.Reload + s.Acc + 1) & 0xFF;
						Slide(s);
						if(s.VibCount != 0) s.VibCount--;
						s.Len = (s.Len - 1) & 0xFF;
						if(s.Len != 0) WriteVol(s);
						else { s.VibCount = s.VibDelay; Events(s); }
					}
				}
				busy |= Mask(ch);
			}
			Frame++;
		}

		bool Tri(Slot s) => (Kinds[s.Flags & 3] & 0x80) != 0;
		bool Noise(Slot s) => (Kinds[s.Flags & 3] & 0x40) != 0;
		bool Busy(Slot s) => (busy & Mask(s.Flags & 3)) != 0;
		static int Reg(Slot s) => 0x4000 + 4 * (s.Flags & 3);

		/// <summary>$467C: read the instrument header, then the first events</summary>
		void Init(Slot s)
		{
			s.Reload = s.Acc = Swap(Rd(s.Ptr));
			int duty = DutyBits(s, Rd(s.Ptr + 1));
			s.Vol = Rd(s.Ptr + 2) | duty;
			s.Sweep = Rd(s.Ptr + 3);
			s.LoopA = s.LoopB = s.Shape = s.Slide = 0;
			s.State = 2;
			s.VibCount = s.VibDelay;
			Events(s);
		}

		/// <summary>$48FC: duty bits from a duty number (the triangle takes its linear counter)</summary>
		int DutyBits(Slot s, int b) => Tri(s) ? b & 0x7F : ((b & 1) << 6) | ((b & 2) << 6);

		/// <summary>$46CE: read events up to the next note or rest</summary>
		void Events(Slot s)
		{
			for(int guard = 0; guard < 4096; guard++) {
				int a = s.Ptr + 2 * s.State;
				int op = Rd(a), arg = Rd(a + 1);
				s.State = (s.State + 1) & 0xFF;
				if(op >= 0xF0) {
					if(op == 0xFD) s.Mark = s.State;
					else if(op == 0xFF) { s.State = 0xFF; s.Looped = true; Silence(s); return; }
				} else if(op >= 0xD0) {
					if(!Tri(s)) {
						s.Slide = op >= 0xE0 ? (-(op & 0x0F)) & 0xFF : op & 0x0F;
						s.SlidePeriod = s.SlideCount = arg;
					}
				} else if(op >= 0xC0) {
					scratch = op & 0x0F;
					if(!Tri(s) && (s.Vol & 0x10) != 0) { s.ShapeLen = arg; s.Shape = op & 0x0F; }
				} else if(op >= 0xB0) {
					int n = op & 0x0F;
					bool jump = true;
					if(n != 0) {
						ref int c = ref (arg == 0 ? ref s.LoopA : ref s.LoopB);
						c = (c - 1) & 0xFF;
						if(c == 0) jump = false;
						else if(c >= 0x80) c = n;
					} else {
						s.Looped = true;
					}
					if(jump) s.State = arg != 0 ? arg : s.Mark;
				} else if(op >= 0xA0) {
					switch(op) {
						case 0xA0: if(!Tri(s)) s.Vol = (s.Vol & 0xC0) | arg; break;
						case 0xA1: s.Sweep = arg; break;
						case 0xA2: s.Vol = Tri(s) ? arg & 0x7F : (s.Vol & 0x1F) | DutyBits(s, arg); break;
						case 0xA3 when arg < 0x80:
							s.VibDelay = s.VibCount = ((arg & 0x0F) << 1) & 0xFF;
							s.Flags = (arg & 0x70) | (s.Flags & 3) | 0x80;
							break;
						case 0xA3: case 0xA4: s.Flags &= 3; break;
						case 0xAD:
							s.Ptr = (s.Ptr + 2 * s.State) & 0xFFFF;
							s.State = 0;
							if(arg == 0) { Init(s); return; }
							break;
						case 0xAE: s.Reload = (s.Reload & 0x7F) | (arg & 0x80); break;
						case 0xAF: s.Reload = s.Acc = Swap(arg); break;
					}
				} else {
					Note(s, op, arg);
					return;
				}
			}
		}

		/// <summary>$4856</summary>
		void Note(Slot s, int op, int arg)
		{
			s.Len = arg;
			int lo, hi;
			if(Noise(s)) {
				if(op >= 0x10) { Silence(s); return; }
				lo = op; hi = 0;
			} else {
				if((op & 0x0F) >= 12) { Silence(s); return; }
				int p = Periods[op & 0x0F] >> (op >> 4);
				lo = p & 0xFF; hi = p >> 8;
			}
			s.ShapePos = 0;
			if(Busy(s)) return;
			enabled |= Mask(s.Flags & 3);
			Write(0x4015, enabled);
			VolReg(s);
			Write(Reg(s) + 1, s.Sweep);
			Write(Reg(s) + 2, lo);
			s.PeriodLo = lo < 2 ? 2 : lo >= 0xFE ? 0xFD : lo;
			Write(Reg(s) + 3, (hi & 7) | 8);
		}

		/// <summary>$48DE</summary>
		void Silence(Slot s)
		{
			if(Busy(s)) return;
			enabled &= Mask(s.Flags & 3) ^ 0x0F;
			Write(0x4015, enabled);
		}

		/// <summary>$4916</summary>
		void WriteVol(Slot s)
		{
			if(!Busy(s)) VolReg(s);
		}

		/// <summary>$4919</summary>
		void VolReg(Slot s)
		{
			if(Tri(s)) Write(0x4008, s.Vol);
			else if(s.Shape != 0) Envelope(s);
			else Write(Reg(s), s.Vol | ((s.Vol & 0x10) << 1));
		}

		/// <summary>$493C</summary>
		void EnvelopeFrame(Slot s)
		{
			if(!Busy(s) && !Tri(s) && s.Shape != 0) Envelope(s);
		}

		/// <summary>$4948: the decay shape's level at ShapePos / ShapeLen, times the volume</summary>
		void Envelope(Slot s)
		{
			int q = 0, a = s.ShapePos;
			for(int i = 0; i < 4; i++) {
				a = (a << 1) & 0xFF;
				int bit = 0;
				if(a >= s.ShapeLen) { a -= s.ShapeLen; bit = 1; }
				q = (q << 1) | bit;
			}
			int y = ((s.Shape << 4) & 0xFF) | q;
			int y2 = (s.Vol & 0x0F) | Tab(EnvBase + y);
			Write(Reg(s), (s.Vol & 0xC0) | Tab(VolBase + y2));
		}

		/// <summary>$4983: volume slide, a step every SlidePeriod ticks</summary>
		void Slide(Slot s)
		{
			if(Tri(s) || s.Slide == 0) return;
			s.SlideCount = (s.SlideCount - 1) & 0xFF;
			if(s.SlideCount != 0) return;
			s.SlideCount = s.SlidePeriod;
			int v = s.Vol & 0x1F;
			if((v & 0x10) == 0) return;
			if(s.Slide < 0x80) {
				s.Slide--;
				if(v != 0x1F) s.Vol++;
			} else {
				s.Slide = (s.Slide + 1) & 0xFF;
				if(v != 0x10) s.Vol--;
			}
		}

		/// <summary>$49C5</summary>
		void Vibrato(Slot s)
		{
			if(Busy(s) || Noise(s) || s.VibCount != 0 || (s.Flags & 0x80) == 0) return;
			int y = (s.Flags & 0x70) | (frameCount & 0x0F);
			Write(Reg(s) + 2, (Tab(VibBase + y) + s.PeriodLo) & 0xFF);
		}
	}

	/// <summary>The two pulse channels, the triangle and the noise of the NES APU (NTSC tables,
	/// as MesenCE uses for Dendy), with the 5-step frame counter, mixed with the APU's nonlinear
	/// mixer and resampled to Rate</summary>
	sealed class Apu
	{
		static readonly int[] LengthTable = {
			10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
			12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30 };
		static readonly int[] NoisePeriods = { 4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068 };
		static readonly int[][] Duties = {
			new[] { 0, 1, 0, 0, 0, 0, 0, 0 }, new[] { 0, 1, 1, 0, 0, 0, 0, 0 },
			new[] { 0, 1, 1, 1, 1, 0, 0, 0 }, new[] { 1, 0, 0, 1, 1, 1, 1, 1 } };
		static readonly int[] TriSeq = Enumerable.Range(0, 32).Select(i => i < 16 ? 15 - i : i - 16).ToArray();

		sealed class Env
		{
			public bool Start, Loop, Constant;
			public int Volume, Divider, Decay;
			public int Out => Constant ? Volume : Decay;
			public void Clock()
			{
				if(Start) { Start = false; Decay = 15; Divider = Volume; }
				else if(Divider == 0) { Divider = Volume; if(Decay > 0) Decay--; else if(Loop) Decay = 15; }
				else Divider--;
			}
		}

		sealed class Pulse
		{
			public readonly Env Env = new();
			public bool Enabled, Halt, SweepOn, Negate, SweepReload, Second;
			public int Duty, Period, Timer, Step, Length, SweepPeriod, SweepShift, SweepDiv;
			int Target => Period + (Negate ? -(Period >> SweepShift) - (Second ? 0 : 1) : Period >> SweepShift);
			bool Muted => Period < 8 || Target > 0x7FF;
			public int Out => Length > 0 && !Muted && Duties[Duty][Step] != 0 ? Env.Out : 0;
			public void Clock()
			{
				if(Timer == 0) { Timer = Period; Step = (Step + 7) & 7; } else Timer--;
			}
			public void Half()
			{
				if(Length > 0 && !Halt) Length--;
				if(SweepDiv == 0 && SweepOn && SweepShift > 0 && !Muted) Period = Math.Max(0, Target);
				if(SweepDiv == 0 || SweepReload) { SweepDiv = SweepPeriod; SweepReload = false; } else SweepDiv--;
			}
		}

		readonly Pulse[] pulse = { new(), new() { Second = true } };
		bool triOn, triControl, triReload, noiseOn, noiseHalt, noiseMode;
		int triPeriod, triTimer, triStep, triLength, triLinear, triLinearLoad;
		readonly Env noiseEnv = new();
		int noisePeriod = 4, noiseTimer, noiseLength, lfsr = 1;
		long cycle;
		double acc, hp, hpPrev, lp;
		int accN;
		double pos;

		public void Write(int a, int v)
		{
			switch(a) {
				case 0x4000: case 0x4004: {
					var p = pulse[(a >> 2) & 1];
					p.Duty = v >> 6; p.Halt = p.Env.Loop = (v & 0x20) != 0; p.Env.Constant = (v & 0x10) != 0; p.Env.Volume = v & 0x0F;
					break;
				}
				case 0x4001: case 0x4005: {
					var p = pulse[(a >> 2) & 1];
					p.SweepOn = (v & 0x80) != 0; p.SweepPeriod = (v >> 4) & 7; p.Negate = (v & 8) != 0; p.SweepShift = v & 7; p.SweepReload = true;
					break;
				}
				case 0x4002: case 0x4006: { var p = pulse[(a >> 2) & 1]; p.Period = (p.Period & 0x700) | v; break; }
				case 0x4003: case 0x4007: {
					var p = pulse[(a >> 2) & 1];
					p.Period = (p.Period & 0xFF) | (v & 7) << 8;
					if(p.Enabled) p.Length = LengthTable[v >> 3];
					p.Step = 0; p.Env.Start = true;
					break;
				}
				case 0x4008: triControl = (v & 0x80) != 0; triLinearLoad = v & 0x7F; break;
				case 0x400A: triPeriod = (triPeriod & 0x700) | v; break;
				case 0x400B:
					triPeriod = (triPeriod & 0xFF) | (v & 7) << 8;
					if(triOn) triLength = LengthTable[v >> 3];
					triReload = true;
					break;
				case 0x400C: noiseHalt = noiseEnv.Loop = (v & 0x20) != 0; noiseEnv.Constant = (v & 0x10) != 0; noiseEnv.Volume = v & 0x0F; break;
				case 0x400E: noiseMode = (v & 0x80) != 0; noisePeriod = NoisePeriods[v & 0x0F]; break;
				case 0x400F: if(noiseOn) noiseLength = LengthTable[v >> 3]; noiseEnv.Start = true; break;
				case 0x4015:
					pulse[0].Enabled = (v & 1) != 0; if(!pulse[0].Enabled) pulse[0].Length = 0;
					pulse[1].Enabled = (v & 2) != 0; if(!pulse[1].Enabled) pulse[1].Length = 0;
					triOn = (v & 4) != 0; if(!triOn) triLength = 0;
					noiseOn = (v & 8) != 0; if(!noiseOn) noiseLength = 0;
					break;
			}
		}

		void Quarter()
		{
			pulse[0].Env.Clock(); pulse[1].Env.Clock(); noiseEnv.Clock();
			if(triReload) triLinear = triLinearLoad; else if(triLinear > 0) triLinear--;
			if(!triControl) triReload = false;
		}

		void Half()
		{
			pulse[0].Half(); pulse[1].Half();
			if(triLength > 0 && !triControl) triLength--;
			if(noiseLength > 0 && !noiseHalt) noiseLength--;
		}

		/// <summary>Run n CPU cycles, appending output samples</summary>
		public void Run(int n, List<float> o)
		{
			double step = Rate / Cpu;
			for(int i = 0; i < n; i++, cycle++) {
				//5-step frame counter: quarter frames at these cycles of a 37282-cycle sequence
				long fc = cycle % 37282;
				if(fc == 7457 || fc == 22371) Quarter();
				else if(fc == 14913 || fc == 37281) { Quarter(); Half(); }
				if((cycle & 1) == 0) {
					pulse[0].Clock(); pulse[1].Clock();
					if(noiseTimer == 0) {
						noiseTimer = noisePeriod / 2 - 1;
						int fb = (lfsr ^ (lfsr >> (noiseMode ? 6 : 1))) & 1;
						lfsr = (lfsr >> 1) | (fb << 14);
					} else noiseTimer--;
				}
				if(triTimer == 0) {
					triTimer = triPeriod;
					if(triLength > 0 && triLinear > 0 && triPeriod >= 2) triStep = (triStep + 1) & 31;
				} else triTimer--;
				int p = pulse[0].Out + pulse[1].Out;
				int t = TriSeq[triStep];
				int nz = noiseLength > 0 && (lfsr & 1) == 0 ? noiseEnv.Out : 0;
				double po = p == 0 ? 0 : 95.88 / (8128.0 / p + 100);
				double tnd = t == 0 && nz == 0 ? 0 : 159.79 / (1 / (t / 8227.0 + nz / 12241.0) + 100);
				acc += po + tnd;
				accN++;
				pos += step;
				if(pos >= 1) {
					pos -= 1;
					double x = acc / accN;
					acc = 0; accN = 0;
					//The console's output filters: high-pass ~90 Hz, low-pass ~14 kHz
					hp = 0.987 * (hp + x - hpPrev);
					hpPrev = x;
					lp += 0.67 * (hp - lp);
					o.Add((float)(lp * 1.6));
				}
			}
		}
	}
}
