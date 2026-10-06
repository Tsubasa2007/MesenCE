using System.Text;
using System.Text.RegularExpressions;

namespace EasyMake.Core;

/// <summary>Music for the INSTALL.CMD driver as an editable listing (syntax and events: see
/// FORMAT.md). Song = tracks-1, first track; track
/// entry = slot, flags (APU channel), data pointer; track data = 4-byte instrument header, then
/// 2-byte events, $FF to end; a note's high nibble is the octave (0 = C2), the low nibble the
/// semitone (12-15 = rest).</summary>
public static class Music
{
	public static readonly string[] Names = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-" };

	static int W16(byte[] d, int o) => d[o] | d[o + 1] << 8;

	public static string Disassemble(byte[] d, int o, string name = "music")
	{
		int bank = o / 0x4000, baseCpu = 0x8000 + o % 0x4000, endBank = (bank + 1) * 0x4000;
		int Off(int p) => bank * 0x4000 + p - 0x8000;
		int S = W16(d, o), T = W16(d, o + 2);
		if(!(baseCpu < S && S < 0xC000 && baseCpu < T && T < 0xC000)) throw new InvalidDataException("not a music header");
		var songs = new List<(int cnt, int first)>();
		int p = Off(S), ntracks = 0;
		while(p + 2 <= Off(T) && songs.Count < 64) {
			int cnt = d[p], first = d[p + 1];
			if(cnt > 3) break;
			songs.Add((cnt, first));
			ntracks = Math.Max(ntracks, first + cnt + 1);
			p += 2;
			if(songs.Count == 1) break;
		}
		var tracks = new List<(int slot, int fl, int ptr)>();
		for(int i = 0; i < ntracks; i++) {
			int e = Off(T) + 4 * i;
			if(e + 4 > endBank) throw new InvalidDataException("track table runs past the bank");
			tracks.Add((d[e], d[e + 1], W16(d, e + 2)));
		}
		if(tracks.Count == 0) throw new InvalidDataException("no tracks");
		var dataStarts = tracks.Select(t => t.ptr).Distinct().OrderBy(x => x).ToList();
		int lim = dataStarts.Select(Off).Append(Off(T)).Min();
		while(p + 2 <= lim) {
			int cnt = d[p], first = d[p + 1];
			if(cnt > 3) break;
			songs.Add((cnt, first));
			p += 2;
		}
		while(tracks.Count < songs.Max(s => s.first + s.cnt + 1)) {
			int e = Off(T) + 4 * tracks.Count;
			tracks.Add((d[e], d[e + 1], W16(d, e + 2)));
		}
		dataStarts = tracks.Select(t => t.ptr).Distinct().OrderBy(x => x).ToList();
		var regions = dataStarts.Append(S).Append(T).Distinct().OrderBy(x => x).ToList();
		var label = new Dictionary<int, string> { [S] = "songs", [T] = "tracks" };
		for(int i = 0; i < dataStarts.Count; i++) label.TryAdd(dataStarts[i], "t" + i);
		int end = Math.Max(Off(T) + 4 * tracks.Count, Off(S) + 2 * songs.Count);

		var lines = new List<string> { "; music " + name, $"header @{label[S]} @{label[T]}" };
		int pos = o + 4;
		void RawUntil(int q)
		{
			while(pos < q) {
				int n = Math.Min(q, pos + 16);
				lines.Add("db " + string.Join(" ", d[pos..n].Select(b => b.ToString("X2"))));
				pos = n;
			}
		}
		foreach(int r in regions) {
			int a = Off(r);
			if(a < pos) throw new InvalidDataException("overlapping music regions");
			RawUntil(a);
			lines.Add($"@{label[r]}:");
			if(r == S) {
				foreach(var (c, f) in songs) lines.Add($"song {c} {f}");
				pos = a + 2 * songs.Count;
			} else if(r == T) {
				foreach(var (slot, fl, ptr) in tracks) lines.Add($"track ${slot:X2} ${fl:X2} @{label[ptr]}");
				pos = a + 4 * tracks.Count;
			} else {
				int nxt = regions.Select(Off).Where(x => x > a).Append(endBank).Min();
				int q = a;
				lines.Add("inst " + string.Join(" ", d[q..(q + 4)].Select(b => b.ToString("X2"))));
				q += 4;
				while(q < nxt) {
					int op = d[q];
					if(op == 0xFF) { lines.Add("end"); q++; break; }
					if(q + 2 > nxt) break;
					int arg = d[q + 1];
					if(op < 0xA0) {
						if((op & 0x0F) < 12) lines.Add($"{Names[op & 0x0F]}{(op >> 4) + 2} {arg:X2}");
						else lines.Add($"rest ${op:X2} {arg:X2}");
					} else {
						lines.Add($"ev {op:X2} {arg:X2}");
					}
					q += 2;
					if(op == 0xAD && arg == 0) {
						lines.Add("inst " + string.Join(" ", d[q..(q + 4)].Select(b => b.ToString("X2"))));
						q += 4;
					}
				}
				pos = q;
				end = Math.Max(end, q);
			}
		}
		RawUntil(end);
		return string.Join("\n", lines) + "\n";
	}

	static readonly Regex NoteRe = new(@"^([A-Ga-g])([-#])(\d+)$");

	static int ParseNum(string s)
	{
		s = s.Replace("$", "0x");
		return s.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? Convert.ToInt32(s[2..], 16) : int.Parse(s);
	}

	/// <summary>Listing -> bytes, for a resource that starts at CPU address 'baseAddr'</summary>
	public static byte[] Assemble(string text, int baseAddr)
	{
		var items = text.Split('\n').Select(l => l.Split(';')[0].Trim()).Where(l => l.Length > 0).ToList();
		List<byte> Emit(Dictionary<string, int> labels)
		{
			var o = new List<byte>();
			int Ref(string s) => s.StartsWith('@') ? labels.GetValueOrDefault(s[1..], 0) : ParseNum(s);
			foreach(var it in items) {
				var m = Regex.Match(it, @"^@(\w+):$");
				if(m.Success) { labels[m.Groups[1].Value] = baseAddr + o.Count; continue; }
				var f = it.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
				switch(f[0].ToLowerInvariant()) {
					case "header": {
						int a = Ref(f[1]), b = Ref(f[2]);
						o.AddRange(new[] { (byte)a, (byte)(a >> 8), (byte)b, (byte)(b >> 8) });
						break;
					}
					case "song": o.Add((byte)int.Parse(f[1])); o.Add((byte)int.Parse(f[2])); break;
					case "track": {
						int a = Ref(f[3]);
						o.AddRange(new[] { (byte)Ref(f[1]), (byte)Ref(f[2]), (byte)a, (byte)(a >> 8) });
						break;
					}
					case "inst": case "db": case "ev":
						foreach(var x in f.Skip(1)) o.Add(Convert.ToByte(x, 16));
						break;
					case "end": o.Add(0xFF); break;
					case "rest": o.Add((byte)Ref(f[1])); o.Add(Convert.ToByte(f[2], 16)); break;
					default: {
						var n = NoteRe.Match(f[0]);
						if(!n.Success) throw new InvalidDataException("bad music line: " + it);
						int semi = Array.IndexOf(Names, n.Groups[1].Value.ToUpperInvariant() + n.Groups[2].Value);
						int octave = int.Parse(n.Groups[3].Value) - 2;
						if(semi < 0 || octave < 0 || octave > 9) throw new InvalidDataException("octave out of range (2-11): " + it);
						o.Add((byte)(octave << 4 | semi));
						o.Add(Convert.ToByte(f[1], 16));
						break;
					}
				}
			}
			return o;
		}
		var labels = new Dictionary<string, int>();
		Emit(labels);
		return Emit(labels).ToArray();
	}
}
