namespace EasyMake.Core;

/// <summary>Encode speech (a WAV file) into a phrase for the BBK speech chip: per 20 ms frame,
/// 10 reflection coefficients (Levinson-Durbin on the autocorrelation), a voiced/unvoiced
/// decision with a pitch period, and an energy, each quantized to the chip's tables. A phrase
/// is the big-endian length of the stream, the $D6 sync byte and the stream.</summary>
public static class LpcEnc
{
	public const int Rate = 10000, FrameLen = 200;

	/// <summary>-> mono samples at 10 kHz, -1..1</summary>
	public static List<double> ReadWav(string path)
	{
		var d = File.ReadAllBytes(path);
		int pos = 12, ch = 1, width = 2, rate = 10000;
		byte[] data = null;
		while(pos + 8 <= d.Length) {
			string id = System.Text.Encoding.ASCII.GetString(d, pos, 4);
			int len = BitConverter.ToInt32(d, pos + 4);
			if(id == "fmt ") {
				ch = BitConverter.ToInt16(d, pos + 10);
				rate = BitConverter.ToInt32(d, pos + 12);
				width = BitConverter.ToInt16(d, pos + 22) / 8;
			} else if(id == "data") {
				data = d[(pos + 8)..Math.Min(d.Length, pos + 8 + len)];
			}
			pos += 8 + len + (len & 1);
		}
		if(data == null) throw new InvalidDataException("no audio data in " + path);
		var s = new List<double>();
		for(int i = 0; i + width <= data.Length; i += width) {
			s.Add(width switch {
				1 => (data[i] - 128) / 128.0,
				2 => BitConverter.ToInt16(data, i) / 32768.0,
				3 => ((data[i] | data[i + 1] << 8 | data[i + 2] << 16) << 8 >> 8) / 8388608.0,
				_ => BitConverter.ToInt32(data, i) / 2147483648.0
			});
		}
		var mono = new List<double>();
		for(int i = 0; i + ch <= s.Count; i += ch) {
			double sum = 0;
			for(int c = 0; c < ch; c++) sum += s[i + c];
			mono.Add(sum / ch);
		}
		return Resample(mono, rate, Rate);
	}

	public static List<double> Resample(List<double> s, int src, int dst)
	{
		if(src == dst) return s;
		//Low-pass at the lower Nyquist frequency (windowed sinc), then interpolate linearly
		double cut = Math.Min(src, dst) / 2.0 * 0.9 / src;
		const int taps = 31;
		var h = new double[taps];
		for(int i = 0; i < taps; i++) {
			int t = i - taps / 2;
			double v = t == 0 ? 2 * cut : Math.Sin(2 * Math.PI * cut * t) / (Math.PI * t);
			h[i] = v * (0.54 - 0.46 * Math.Cos(2 * Math.PI * i / (taps - 1)));
		}
		var f = new double[s.Count];
		for(int i = 0; i < s.Count; i++) {
			double acc = 0;
			for(int j = 0; j < taps; j++) {
				int k = i - j + taps / 2;
				if(k >= 0 && k < s.Count) acc += h[j] * s[k];
			}
			f[i] = acc;
		}
		var o = new List<double>();
		double step = (double)src / dst, pos = 0.0;
		while(pos < f.Length - 1) {
			int i = (int)pos;
			o.Add(f[i] + (f[i + 1] - f[i]) * (pos - i));
			pos += step;
		}
		return o;
	}

	/// <summary>Autocorrelation -> reflection coefficients (standard sign: k1 > 0 for low-pass)</summary>
	public static double[] Levinson(double[] r, int order = 10)
	{
		var a = new double[order + 1];
		double e = r[0];
		var ks = new List<double>();
		for(int i = 1; i <= order; i++) {
			if(e <= 0) { while(ks.Count < order) ks.Add(0.0); break; }
			double sum = 0;
			for(int j = 1; j < i; j++) sum += a[j] * r[i - j];
			double k = Math.Max(-0.995, Math.Min(0.995, (r[i] - sum) / e));
			ks.Add(k);
			var nw = (double[])a.Clone();
			nw[i] = k;
			for(int j = 1; j < i; j++) nw[j] = a[j] - k * a[i - j];
			a = nw;
			e *= 1 - k * k;
		}
		return ks.ToArray();
	}

	static int Nearest(int[] table, double v, int count = -1)
	{
		int n = count < 0 ? table.Length : count, best = 0;
		for(int i = 1; i < n; i++) if(Math.Abs(table[i] - v) < Math.Abs(table[best] - v)) best = i;
		return best;
	}

	/// <summary>Autocorrelation pitch on a centre-clipped frame: (period in samples, strength 0-1)</summary>
	static (int period, double strength) PitchPeriod(double[] x)
	{
		double peak = x.Length == 0 ? 0 : x.Max(Math.Abs);
		if(peak == 0) return (0, 0);
		double cl = 0.3 * peak;
		var c = x.Select(v => v > cl ? v - cl : v < -cl ? v + cl : 0).ToArray();
		double r0 = 0;
		foreach(var v in c) r0 += v * v;
		if(r0 == 0) return (0, 0);
		int best = 0; double bestv = 0;
		for(int lag = 20; lag < 161; lag++) {
			double acc = 0;
			for(int i = 0; i < c.Length - lag; i++) acc += c[i] * c[i + lag];
			double v = acc / r0;
			if(v > bestv) { best = lag; bestv = v; }
		}
		return (best, bestv);
	}

	sealed class BitWriter
	{
		public readonly List<byte> Out = new();
		int n;
		public void Put(int value, int bits)
		{
			for(int i = bits - 1; i >= 0; i--) {
				if(n % 8 == 0) Out.Add(0);
				if((value >> i & 1) != 0) Out[^1] |= (byte)(1 << (n % 8));
				n++;
			}
		}
	}

	/// <summary>One 20 ms frame as analysed: Silent, pitch period in samples (0 = unvoiced), the
	/// reflection coefficients in the chip's sign, and the frame's RMS level</summary>
	sealed record Frame(bool Silent, int Period, double[] Ks, double Rms);

	/// <summary>The chip's energy is the level of the excitation, before the lattice filter, and
	/// the filter's gain is 1 / sqrt(prod(1 - k^2)) - large on strong vowels. So a frame's energy
	/// comes from its prediction residual (RMS x sqrt(prod(1 - k^2)) over the coefficients the
	/// frame sends). The scale was fitted by re-encoding BBK's own phrases (disk 004, 179 of
	/// them) until the energy steps came back unbiased and the output level matched the input.
	/// Taking the energy from the plain RMS, as before, made vowels about twice as loud as the
	/// recording and pushed them into the chip's clamp.</summary>
	const double EnergyScale = 3.8;

	/// <summary>The level speech is encoded at, whatever it was recorded at: the loud end of the
	/// speech (the 95th percentile of the 20 ms frames above 5% of the loudest one) is brought to
	/// this RMS. The chip's energy steps are coarse at the bottom and stop at the top, so a quiet
	/// recording used only a few of them and a loud one sat on the last; over eight recordings
	/// (Chinese and English, children and adults) the level envelope followed the source best
	/// around 0.28-0.35 and clipped least at the low end of that; 0.28 is also how loud BBK's
	/// own phrases come out of the chip.</summary>
	public const double TargetLevel = 0.28;

	/// <summary>The gain that brings a recording to TargetLevel (1 for silence)</summary>
	public static double AutoGain(List<double> s)
	{
		var rms = new List<double>();
		for(int i = 0; i + FrameLen <= s.Count; i += FrameLen) {
			double sq = 0;
			for(int j = i; j < i + FrameLen; j++) sq += s[j] * s[j];
			rms.Add(Math.Sqrt(sq / FrameLen));
		}
		double max = rms.DefaultIfEmpty().Max();
		if(max <= 0) return 1;
		var speech = rms.Where(r => r > max * 0.05).OrderBy(r => r).ToList();
		double p95 = speech[Math.Min(speech.Count - 1, (int)(speech.Count * 0.95))];
		return Math.Clamp(TargetLevel / p95, 0.1, 20);
	}

	/// <summary>10 kHz samples -> phrase bytes. The recording is levelled first (AutoGain); gain
	/// is a further adjustment on top of that, 1 for none.</summary>
	public static byte[] EncodeSamples(List<double> s, double gain = 1.0)
	{
		gain *= AutoGain(s);
		//Pre-emphasis flattens the spectrum for the analysis; the chip's tables expect it
		var pre = new double[s.Count];
		if(s.Count > 0) pre[0] = s[0];
		for(int i = 1; i < s.Count; i++) pre[i] = s[i] - 0.9375 * s[i - 1];
		int nframes = Math.Max(1, (s.Count + FrameLen - 1) / FrameLen);
		var win = new double[2 * FrameLen];
		for(int i = 0; i < win.Length; i++) win[i] = 0.54 - 0.46 * Math.Cos(2 * Math.PI * i / (2 * FrameLen - 1));
		var frames = new List<Frame>();
		for(int f = 0; f < nframes; f++) {
			int a = f * FrameLen - FrameLen / 2;
			var w = new double[2 * FrameLen];
			var raw = new double[2 * FrameLen];
			for(int i = 0; i < 2 * FrameLen; i++) {
				int k = a + i;
				w[i] = (k >= 0 && k < pre.Length ? pre[k] : 0.0) * win[i];
				raw[i] = k >= 0 && k < s.Count ? s[k] : 0.0;
			}
			var r = new double[11];
			for(int lag = 0; lag < 11; lag++) {
				double acc = 0;
				for(int i = 0; i < w.Length - lag; i++) acc += w[i] * w[i + lag];
				r[lag] = acc;
			}
			double sq = 0;
			for(int i = FrameLen / 2; i < FrameLen / 2 + FrameLen; i++) sq += raw[i] * raw[i];
			double rms = Math.Sqrt(sq / FrameLen) * gain;
			var (period, strength) = PitchPeriod(raw);
			//Silence: a frame whose plain level would round to the bottom energy step (the scale
			//the energy was taken with before it came from the residual)
			if(Nearest(Lpc.Gain, rms * 1.8 * 0x7F00, 15) == 0 || r[0] <= 1e-9) { frames.Add(new Frame(true, 0, null, 0)); continue; }
			bool voiced = strength > 0.35 && period > 0;
			frames.Add(new Frame(false, voiced ? period : 0,
				Levinson(r).Select(k => -k).ToArray(), rms));  // the chip's lattice uses the opposite sign
		}
		SmoothVoicing(frames);
		var bw = new BitWriter();
		foreach(var fr in frames) {
			if(fr.Silent) { bw.Put(0, 4); continue; }
			int pi = fr.Period > 0 ? Nearest(Lpc.Pitch[1..], fr.Period * 16) + 1 : 0;
			int used = pi != 0 ? 10 : 4;
			var kix = Enumerable.Range(0, used).Select(i => Nearest(Lpc.K[i], fr.Ks[i] * 32768)).ToArray();
			double g = 1;
			for(int i = 0; i < used; i++) { double k = Lpc.K[i][kix[i]] / 32768.0; g *= 1 - k * k; }
			int energy = Math.Max(1, Math.Min(14, Nearest(Lpc.Gain, fr.Rms * Math.Sqrt(g) * EnergyScale * 0x7F00, 15)));
			bw.Put(energy, 4);
			bw.Put(0, 1);
			bw.Put(pi, 7);
			for(int i = 0; i < used; i++) bw.Put(kix[i], Lpc.KBits[i]);
		}
		bw.Put(15, 4);
		//The BIOS sends 'length' bytes starting with the $D6, so the stream's last byte never
		//reaches the chip - pad so the end frame is sent
		var stream = bw.Out.Append((byte)0).ToArray();
		return new byte[] { (byte)(stream.Length >> 8), (byte)stream.Length, 0xD6 }.Concat(stream).ToArray();
	}

	/// <summary>A sound does not change between voiced and unvoiced for one frame and back: a lone
	/// frame that disagrees with both neighbours is a misjudged pitch, and on the chip it is a
	/// 20 ms burst of hiss inside a vowel (or of buzz inside a hissing sound). Such a frame takes
	/// its neighbours' voicing - a voiced one their average pitch. Silence is left alone.</summary>
	static void SmoothVoicing(List<Frame> frames)
	{
		//Left to right, each decision seeing the one before it, so a run that alternates every
		//frame settles on its first frame's voicing instead of keeping every other flip
		for(int i = 1; i + 1 < frames.Count; i++) {
			Frame p = frames[i - 1], f = frames[i], n = frames[i + 1];
			if(f.Silent || p.Silent || n.Silent) continue;
			bool pv = p.Period > 0, nv = n.Period > 0;
			if(pv != nv || (f.Period > 0) == pv) continue;
			frames[i] = f with { Period = pv ? (p.Period + n.Period) / 2 : 0 };
		}
	}

	public static byte[] EncodeWav(string path, double gain = 1.0) => EncodeSamples(ReadWav(path), gain);

	/// <summary>Correlation of the 20 ms level envelopes of a decoded phrase and its source</summary>
	public static double Compare(byte[] phrase, IList<double> src)
	{
		var outp = Lpc.Synth(phrase);
		List<double> Env(IList<double> x)
		{
			var e = new List<double>();
			for(int i = 0; i < x.Count - FrameLen; i += FrameLen) {
				double acc = 0;
				for(int j = i; j < i + FrameLen; j++) acc += x[j] * x[j];
				e.Add(Math.Sqrt(acc / FrameLen));
			}
			return e;
		}
		var a = Env(src);
		var b = Env(outp.Select(v => (double)v).ToList());
		int n = Math.Min(a.Count, b.Count);
		if(n < 2) return 0;
		double ma = a.Take(n).Average(), mb = b.Take(n).Average(), num = 0, da = 0, db = 0;
		for(int i = 0; i < n; i++) { num += (a[i] - ma) * (b[i] - mb); da += (a[i] - ma) * (a[i] - ma); db += (b[i] - mb) * (b[i] - mb); }
		double den = Math.Sqrt(da * db);
		return den == 0 ? 0 : num / den;
	}
}
