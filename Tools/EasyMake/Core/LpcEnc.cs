namespace EasyMake.Core;

/// <summary>Encode speech (a WAV file) into a phrase for the BBK speech chip: per 20 ms frame,
/// 10 reflection coefficients (Levinson-Durbin), a voiced/unvoiced decision with a pitch period,
/// and an energy, each quantized to the chip's tables. A phrase is the big-endian length of the
/// stream, the $D6 sync byte and the stream.
///
/// The analysis follows how the chip (Lpc.Synth) makes its sound rather than the textbook:
/// a frame's values apply from the start of its 20 ms, the voiced excitation is a smooth pulse
/// whose spectrum is taken out before the filter is fitted, and the energy is whatever makes the
/// chip's output as loud as the recording. Re-encoding BBK's own phrases (disk 004) is the test:
/// coefficient error halved and its bias gone, voicing 96% right, energy unbiased.</summary>
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

	/// <summary>Autocorrelation pitch on a centre-clipped frame: (period in samples, strength 0-1).
	/// The strength decides voicing; the period is only one of the candidates TrackPitch chooses
	/// from - on its own it often lands on the octave below (or above)</summary>
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

	/// <summary>Pitch candidates of one frame: (period in samples, correlation). The peaks of the
	/// normalised cross-correlation between the frame and itself shifted by each period the chip
	/// can play (16-160 samples), on a low-passed, centre-clipped copy, each peak placed between
	/// samples - at high pitches one sample is several percent, coarser than the chip's table</summary>
	static List<(double period, double v)> PitchCandidates(double[] raw)
	{
		var x = (double[])raw.Clone();
		//Two passes of a one-pole low-pass near 1 kHz: the formants' ripples weaken, the pitch stays
		double a = Math.Exp(-2 * Math.PI * 1000.0 / Rate);
		for(int pass = 0; pass < 2; pass++) {
			double y = 0;
			for(int i = 0; i < x.Length; i++) { y = (1 - a) * x[i] + a * y; x[i] = y; }
		}
		double peak = x.Length == 0 ? 0 : x.Max(Math.Abs);
		if(peak == 0) return new();
		double cl = 0.3 * peak;
		for(int i = 0; i < x.Length; i++) x[i] = x[i] > cl ? x[i] - cl : x[i] < -cl ? x[i] + cl : 0;
		const int lo = 16, hi = 160;
		int w = x.Length - hi;
		double e0 = 0;
		for(int i = 0; i < w; i++) e0 += x[i] * x[i];
		if(e0 <= 0) return new();
		var r = new double[hi + 2];
		for(int lag = lo - 1; lag + w <= x.Length; lag++) {
			double acc = 0, el = 0;
			for(int i = 0; i < w; i++) { acc += x[i] * x[i + lag]; el += x[i + lag] * x[i + lag]; }
			r[lag] = el > 0 ? acc / Math.Sqrt(e0 * el) : 0;
		}
		var c = new List<(double period, double v)>();
		for(int lag = lo; lag <= hi; lag++) {
			if(r[lag] <= 0.1 || r[lag] < r[lag - 1] || r[lag] < r[lag + 1]) continue;
			double d = r[lag - 1] - 2 * r[lag] + r[lag + 1], off = d < 0 ? 0.5 * (r[lag - 1] - r[lag + 1]) / d : 0;
			c.Add((lag + Math.Clamp(off, -0.5, 0.5), r[lag]));
		}
		return c.OrderByDescending(t => t.v).Take(6).ToList();
	}

	/// <summary>The pitch of each run of voiced frames, chosen for the whole run at once (dynamic
	/// programming): a candidate costs 1 - its correlation, and moving between frames costs the
	/// interval in octaves. Frame by frame, the pitch flipped to its octave whenever the two peaks
	/// were close - on the chip a squeak or growl inside a vowel. Re-encoding disk 004's phrases:
	/// 45 octave jumps -> 0 (the originals have none); voicing is left as each frame decided it.</summary>
	static void TrackPitch(List<Frame> frames)
	{
		for(int from = 0; from < frames.Count; from++) {
			if(frames[from].Period <= 0) continue;
			int to = from;
			while(to < frames.Count && frames[to].Period > 0) to++;
			var cost = new List<double[]>();
			var back = new List<int[]>();
			for(int f = from; f < to; f++) {
				var cs = frames[f].Cands;
				var cur = new double[cs.Count];
				var bk = new int[cs.Count];
				for(int s = 0; s < cs.Count; s++) {
					double best = 0;
					if(f > from) {
						var pc = frames[f - 1].Cands;
						var prev = cost[^1];
						best = double.MaxValue;
						for(int p = 0; p < pc.Count; p++) {
							double t = prev[p] + Math.Abs(Math.Log2(cs[s].period / pc[p].period));
							if(t < best) { best = t; bk[s] = p; }
						}
					}
					cur[s] = best + 1 - cs[s].v;
				}
				cost.Add(cur);
				back.Add(bk);
			}
			int st = Array.IndexOf(cost[^1], cost[^1].Min());
			for(int f = to - 1; f >= from; f--) {
				frames[f] = frames[f] with { Period = frames[f].Cands[st].period };
				st = back[f - from][st];
			}
			from = to;
		}
	}

	/// <summary>One 20 ms frame as analysed: Silent, pitch period in samples (0 = unvoiced), the
	/// power spectrum around the frame's start, the RMS level there, and the pitch candidates
	/// (for a voiced frame, always including its own period)</summary>
	sealed record Frame(bool Silent, double Period, double[] Spectrum, double Rms, List<(double period, double v)> Cands = null);

	const int N = 512;

	static void Fft(System.Numerics.Complex[] a)
	{
		int n = a.Length;
		for(int i = 1, j = 0; i < n; i++) {
			int bit = n >> 1;
			for(; (j & bit) != 0; bit >>= 1) j ^= bit;
			j ^= bit;
			if(i < j) (a[i], a[j]) = (a[j], a[i]);
		}
		for(int len = 2; len <= n; len <<= 1) {
			var wl = System.Numerics.Complex.FromPolarCoordinates(1, -2 * Math.PI / len);
			for(int i = 0; i < n; i += len) {
				var w = System.Numerics.Complex.One;
				for(int j = 0; j < len / 2; j++) {
					var u = a[i + j]; var v = a[i + j + len / 2] * w;
					a[i + j] = u + v; a[i + j + len / 2] = u - v;
					w *= wl;
				}
			}
		}
	}

	/// <summary>The voiced excitation's power spectrum, N/2+1 bins: one pulse as the chip plays it
	/// (Lpc.Excit read every 16th entry as the pitch counter runs down, scaled as Synth scales it,
	/// for a Gain value of 1, in output units of 1/32768), averaged over the 16 phases the counter
	/// can start at. Far from flat - a ~1 ms bump, strongly low-pass - so a filter fitted to the
	/// recording as it is gets the pulse's slope twice: on vowels K2 came out 0.2 too high.</summary>
	static double[] pulse;
	static double[] PulsePower()
	{
		if(pulse != null) return pulse;
		var acc = new double[N / 2 + 1];
		for(int start = 144; start < 160; start++) {
			var x = new System.Numerics.Complex[N];
			int i = 0;
			for(int v = start; v >= 0; v -= 16) x[i++] = Lpc.Excit[v] / 32768.0 * Lpc.Volume / 2 / 32768;
			Fft(x);
			for(int k = 0; k <= N / 2; k++) acc[k] += x[k].Magnitude * x[k].Magnitude / 16;
		}
		return pulse = acc;
	}

	/// <summary>The filter's power response |1/A|^2 for chip coefficients (Q15, the chip's sign)</summary>
	static double[] FilterPower(int[] kq, int used)
	{
		var a = new double[11];
		for(int i = 1; i <= used; i++) {
			double k = -kq[i - 1] / 32768.0;
			var nw = (double[])a.Clone();
			nw[i] = k;
			for(int j = 1; j < i; j++) nw[j] = a[j] - k * a[i - j];
			a = nw;
		}
		var x = new System.Numerics.Complex[N];
		x[0] = 1;
		for(int j = 1; j <= used; j++) x[j] = -a[j];
		Fft(x);
		var o = new double[N / 2 + 1];
		for(int k = 0; k <= N / 2; k++) o[k] = 1 / Math.Max(1e-12, x[k].Magnitude * x[k].Magnitude);
		return o;
	}

	/// <summary>A frame's reflection coefficients (Levinson's sign) from its power spectrum. For a
	/// voiced frame the pulse's spectrum is divided out first (floored at -30 dB, where the pulse
	/// has next to nothing to give); the noise of an unvoiced frame is flat. A 40 Hz lag window
	/// keeps the formants from sharpening past what 20 ms can tell.</summary>
	static double[] FitFilter(double[] spectrum, bool voiced)
	{
		var e = PulsePower();
		double floor = e.Max() * 1e-3;
		var d = new double[N / 2 + 1];
		for(int k = 0; k <= N / 2; k++) d[k] = voiced ? spectrum[k] / Math.Max(e[k], floor) : spectrum[k];
		var r = new double[11];
		for(int l = 0; l <= 10; l++) {
			double acc = d[0] + d[N / 2] * Math.Cos(Math.PI * l);
			for(int k = 1; k < N / 2; k++) acc += 2 * d[k] * Math.Cos(2 * Math.PI * k * l / N);
			r[l] = acc / N * Math.Exp(-0.5 * Math.Pow(2 * Math.PI * 40 * l / Rate, 2));
		}
		r[0] *= 1.0001;
		return Levinson(r);
	}

	/// <summary>The Gain value that makes the chip's output for this frame as loud as rms: the
	/// excitation's power through the filter - for a voiced frame one pulse per pitch period,
	/// for an unvoiced one the noise generator's fixed level. No fitted scale: re-encoding BBK's
	/// own phrases gives their energy steps back unbiased (voiced +0.1, unvoiced -0.1).</summary>
	static double NeededGain(double rms, int[] kq, int used, int pitchIndex)
	{
		var h = FilterPower(kq, used);
		var e = PulsePower();
		double acc, power;
		if(pitchIndex != 0) {
			acc = e[0] * h[0] + e[N / 2] * h[N / 2];
			for(int k = 1; k < N / 2; k++) acc += 2 * e[k] * h[k];
			power = acc / N / (Lpc.Pitch[pitchIndex] / 16.0);
		} else {
			double amp = (double)Lpc.Noise / 32768 * Lpc.Volume / 2 / 32768;
			acc = h[0] + h[N / 2];
			for(int k = 1; k < N / 2; k++) acc += 2 * h[k];
			power = amp * amp * acc / N;
		}
		return rms / Math.Sqrt(power);
	}

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
		int nframes = Math.Max(1, (s.Count + FrameLen - 1) / FrameLen);
		var win = new double[2 * FrameLen];
		for(int i = 0; i < win.Length; i++) win[i] = 0.54 - 0.46 * Math.Cos(2 * Math.PI * i / (2 * FrameLen - 1));
		var frames = new List<Frame>();
		for(int f = 0; f < nframes; f++) {
			//The chip applies a frame's values from the start of its 20 ms (moving towards the
			//next frame's over them), so the analysis is centred there
			int a = f * FrameLen - FrameLen;
			var raw = new double[2 * FrameLen];
			var x = new System.Numerics.Complex[N];
			double energy = 0;
			for(int i = 0; i < 2 * FrameLen; i++) {
				int k = a + i;
				raw[i] = k >= 0 && k < s.Count ? s[k] : 0.0;
				x[i] = raw[i] * win[i];
				energy += raw[i] * win[i] * raw[i] * win[i];
			}
			double sq = 0;
			for(int i = FrameLen / 2; i < FrameLen / 2 + FrameLen; i++) sq += raw[i] * raw[i];
			double rms = Math.Sqrt(sq / FrameLen) * gain;
			var (period, strength) = PitchPeriod(raw);
			//Silence: a frame whose level would round to the bottom of the chip's old energy scale
			if(Nearest(Lpc.Gain, rms * 1.8 * 0x7F00, 15) == 0 || energy <= 1e-12) { frames.Add(new Frame(true, 0, null, 0)); continue; }
			Fft(x);
			var spectrum = new double[N / 2 + 1];
			for(int k = 0; k <= N / 2; k++) spectrum[k] = x[k].Magnitude * x[k].Magnitude;
			var cands = PitchCandidates(raw);
			if(period > 0 && !cands.Any(t => Math.Abs(t.period - period) < 1.5)) cands.Add((period, Math.Min(strength, 0.99)));
			frames.Add(new Frame(false, strength > 0.35 && period > 0 ? period : 0, spectrum, rms, cands));
		}
		TrackPitch(frames);
		SmoothVoicing(frames);
		var bw = new BitWriter();
		foreach(var fr in frames) {
			if(fr.Silent) { bw.Put(0, 4); continue; }
			int pi = fr.Period > 0 ? Nearest(Lpc.Pitch[1..], fr.Period * 16) + 1 : 0;
			int used = pi != 0 ? 10 : 4;
			var ks = FitFilter(fr.Spectrum, pi != 0);
			var kix = Enumerable.Range(0, used).Select(i => Nearest(Lpc.K[i], -ks[i] * 32768)).ToArray();  // the chip's lattice uses the opposite sign
			var kq = Enumerable.Range(0, used).Select(i => Lpc.K[i][kix[i]]).ToArray();
			int energy = Math.Max(1, Math.Min(14, Nearest(Lpc.Gain, NeededGain(fr.Rms, kq, used, pi), 15)));
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
