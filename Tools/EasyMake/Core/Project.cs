using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace EasyMake.Core;

/// <summary>GBK, strictly: a byte that does not start a valid character is kept as {#XX}, so the text is plain Unicode and goes back byte for byte.</summary>
public static class Gbk
{
	static Encoding enc;
	static Encoding Enc
	{
		get {
			if(enc == null) {
				Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
				enc = Encoding.GetEncoding(936, EncoderFallback.ExceptionFallback, DecoderFallback.ExceptionFallback);
			}
			return enc;
		}
	}

	static string TryPair(byte a, byte b)
	{
		if(b < 0x40 || b == 0x7F || b == 0xFF) return null;
		try {
			string s = Enc.GetString(new[] { a, b });
			//Code page 936 gives GBK's user-defined areas private-use characters; they are
			//treated as invalid, so such bytes stay visible as {#XX}
			if(s.Length == 1 && s[0] >= '' && s[0] <= '') return null;
			return s;
		} catch(DecoderFallbackException) { return null; }
	}

	/// <summary>strict = throw on an invalid byte instead of escaping it</summary>
	public static string Decode(byte[] raw, bool strict = false)
	{
		var sb = new StringBuilder();
		for(int i = 0; i < raw.Length; i++) {
			byte c = raw[i];
			if(c < 0x80) { sb.Append((char)c); continue; }
			if(c >= 0x81 && c <= 0xFE && i + 1 < raw.Length) {
				var s = TryPair(c, raw[i + 1]);
				if(s != null && s != "�") { sb.Append(s); i++; continue; }
			}
			if(strict) throw new DecoderFallbackException("not GBK");
			sb.Append($"{{#{c:X2}}}");
		}
		return sb.ToString();
	}

	public static byte[] Encode(string text)
	{
		var o = new List<byte>();
		int i = 0;
		var buf = new char[2];
		while(i < text.Length) {
			var m = Regex.Match(text.Substring(i, Math.Min(5, text.Length - i)), @"^\{#([0-9A-Fa-f]{2})\}");
			if(m.Success) { o.Add(Convert.ToByte(m.Groups[1].Value, 16)); i += 5; continue; }
			int n = char.IsHighSurrogate(text[i]) && i + 1 < text.Length ? 2 : 1;
			o.AddRange(Enc.GetBytes(text.Substring(i, n)));
			i += n;
		}
		return o.ToArray();
	}
}

/// <summary>Extract an EasyMake title into an editable project folder, and build it back.</summary>
public static class Project
{
	public static readonly JsonSerializerOptions JsonOut = new() { WriteIndented = true, Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

	public static JsonNode ReadJson(string path) => JsonNode.Parse(File.ReadAllText(path));
	public static void WriteJson(string path, JsonNode n) => File.WriteAllText(path, n.ToJsonString(JsonOut));

	//--- script ---------------------------------------------------------------------------

	public static string DecodeScript(byte[] raw)
	{
		if(Encoding.ASCII.GetString(raw).Contains("{#")) throw new InvalidDataException("the script already contains \"{#\"");
		return Gbk.Decode(raw);
	}

	public static byte[] EncodeScript(string text) => Gbk.Encode(text);

	static readonly Regex TagRe = new(@"<(/?\w+)([^>]*)>");
	static readonly Regex AttrRe = new(@"(\w+)\s*=\s*(""[^""]*""|\d+)");

	public static Dictionary<string, string> Attrs(string body)
	{
		var kv = new Dictionary<string, string>();
		foreach(Match m in AttrRe.Matches(body)) kv[m.Groups[1].Value.ToLowerInvariant()] = m.Groups[2].Value;
		return kv;
	}

	/// <summary>{RES file name: {resource name: role}} and the backgrounds, from a script</summary>
	public static (Dictionary<string, Dictionary<string, string>> roles, List<string> backgrounds) ScriptRoles(string text, string firstRes = "SDOS.RES")
	{
		var roles = new Dictionary<string, Dictionary<string, string>>();
		var bgs = new List<string>();
		string res = firstRes;
		foreach(Match m in TagRe.Matches(text)) {
			string tag = m.Groups[1].Value.ToLowerInvariant();
			var kv = Attrs(m.Groups[2].Value);
			if(kv.TryGetValue("resource", out var r)) res = r.Trim('"').ToUpperInvariant();
			if(kv.TryGetValue("background", out var b)) {
				b = b.Trim('"').ToUpperInvariant();
				if(b.Length > 0 && !bgs.Contains(b)) bgs.Add(b);
			}
			foreach(var (attr, role) in new[] { ("src", (string)null), ("backsound", "music"), ("act", "act") }) {
				if(kv.TryGetValue(attr, out var v) && v.StartsWith('"') && v.Length > 2) {
					if(!roles.TryGetValue(res, out var d)) roles[res] = d = new Dictionary<string, string>();
					d.TryAdd(v.Trim('"'), role ?? tag);
				}
			}
		}
		return (roles, bgs);
	}

	//--- reading a .RES into objects --------------------------------------------------------

	public sealed class ResReader
	{
		readonly byte[] d;
		readonly Dictionary<string, string> roles;
		public readonly List<(string name, int offset)> Entries;
		public readonly List<Obj> Top = new();
		readonly Dictionary<int, ImageObj> images = new();
		readonly Dictionary<int, Anim> anims = new();
		public readonly Dictionary<string, Obj> Named = new();
		public readonly List<string> Problems = new();

		public ResReader(byte[] data, Dictionary<string, string> roles)
		{
			d = data; this.roles = roles ?? new Dictionary<string, string>();
			Entries = Res.ParseDirectory(d);
		}

		ImageObj ImageAt(int o, string owner)
		{
			if(!images.TryGetValue(o, out var img)) {
				img = ImageObj.Parse(d, o, $"{owner}_i{images.Count}");
				images[o] = img;
				Top.Add(img);
			}
			return img;
		}

		Anim AnimAt(int o, string owner)
		{
			if(!anims.TryGetValue(o, out var a)) {
				a = new Anim($"{owner}_a{anims.Count}", 0, o);
				anims[o] = a;
				Top.Add(a);
				FillAnim(a, o, owner);
			}
			return a;
		}

		void FillAnim(Anim a, int o, string owner)
		{
			int bank = o / Res.Bank;
			int n = d[o] | (d[o + 1] & 0x3F) << 8;
			if((o % Res.Bank) + 2 + 8 * n > Res.Bank) throw new InvalidDataException("animation runs past its bank");
			a.Flags = d[o + 1] & 0xC0;
			for(int i = 0; i < n; i++) {
				int f = o + 2 + 8 * i;
				int p = Obj.W16(d, f + 2);
				if(p < 0x8000 || p >= 0xC000) throw new InvalidDataException($"frame {i} points outside the window");
				a.Frames.Add(new Frame { X = d[f], Y = d[f + 1], Image = ImageAt(Res.FileOffset(bank, p), owner), Xo = d[f + 4], Yo = d[f + 5], Mode = d[f + 6], Kind = d[f + 7] });
			}
		}

		string Guess(string name, int o)
		{
			foreach(var role in new[] { "music", "speak", "animate", "img" }) {
				try {
					switch(role) {
						case "music": Music.Disassemble(d, o, name); break;
						case "speak": Speech.Parse(d, o, name); break;
						case "animate": {
							int n = d[o + 16] | (d[o + 17] & 0x3F) << 8;
							if(!(n > 0 && n < 512) || d[o..(o + 16)].Any(c => c > 0x3F)) continue;
							int bank = o / Res.Bank;
							for(int i = 0; i < n; i++) ImageObj.Parse(d, Res.FileOffset(bank, Obj.W16(d, o + 18 + 8 * i + 2)), "x");
							break;
						}
						case "img":
							if(o < 16 || d[(o - 16)..o].Any(c => c > 0x3F)) continue;
							ImageObj.Parse(d, o, name);
							break;
					}
					return role;
				} catch(Exception e) when(e is InvalidDataException || e is IndexOutOfRangeException || e is ArgumentException || e is InvalidOperationException) {
					continue;
				}
			}
			return null;
		}

		public ResReader Read()
		{
			var later = new List<(Action<Obj, int, string> fn, Obj obj, int o, string name, bool raw)>();
			foreach(var (name, o) in Entries) {
				string role = roles.GetValueOrDefault(name) ?? Guess(name, o);
				Obj obj = null;
				try {
					switch(role) {
						case "img": {
							var img = ImageObj.Parse(d, o, name + "_image");
							obj = new ImgRes(name, d[(o - 16)..o], img, o - 16);
							images[o] = img;
							break;
						}
						case "animate": {
							var a = new Anim(name + "_anim", 0);
							obj = new AnimRes(name, d[o..(o + 16)], a, o);
							anims[o + 16] = a;
							later.Add(((x, oo, nn) => FillAnim((Anim)x, oo, nn), a, o + 16, name, false));
							break;
						}
						case "act": {
							var act = new Act(name, d[o..(o + 25)], null, o);
							obj = act;
							if((d[o] & 4) != 0) later.Add(((x, oo, nn) => ((Act)x).Anim = AnimAt(oo, nn), act, Res.FileOffset(o / Res.Bank, Obj.W16(d, o + 7)), name, false));
							break;
						}
						case "speak": obj = Speech.Parse(d, o, name); break;
						case "music": obj = new MusicObj(name, Music.Disassemble(d, o, name), o); break;
					}
				} catch(Exception e) when(e is InvalidDataException || e is IndexOutOfRangeException || e is ArgumentException || e is InvalidOperationException) {
					Problems.Add($"{name} ({role}): {e.Message} - kept raw");
					obj = null;
				}
				if(obj == null) { later.Add((RawEntry, null, o, name, true)); continue; }
				Top.Add(obj);
				Named[name] = obj;
			}
			foreach(var l in later.Where(x => !x.raw)) l.fn(l.obj, l.o, l.name);
			foreach(var l in later.Where(x => x.raw)) l.fn(l.obj, l.o, l.name);
			return this;
		}

		void RawEntry(Obj _, int o, string name)
		{
			//Unknown: keep the bytes from the entry to the next object or the bank's end
			var starts = Top.Where(x => x.Offset != null && x.Offset > o).Select(x => x.Offset.Value)
				.Concat(Entries.Where(e => e.offset > o).Select(e => e.offset))
				.Append((o / Res.Bank + 1) * Res.Bank).Append(d.Length);
			int end = starts.Min();
			var data = d[o..end];
			int n = data.Length;
			while(n > 0 && data[n - 1] == 0xFF) n--;
			var obj = new RawObj(name, n > 0 ? data[..n] : new byte[] { 0xFF }, o);
			Top.Add(obj);
			Named[name] = obj;
		}

		public byte[] Coverage()
		{
			var mask = new byte[d.Length];
			for(int i = 0; i < Res.DirSize && i < d.Length; i++) mask[i] = 1;
			foreach(var o in Top)
				foreach(var (rel, n) in o.Covered())
					for(int i = o.Offset.Value + rel; i < o.Offset.Value + rel + n && i < d.Length; i++) mask[i] = 1;
			return mask;
		}
	}

	//--- images to and from PNG -------------------------------------------------------------

	static (int x, int y) CellRect(ImageObj img, byte[] hdr)
		=> img.Type == 1 ? ((hdr[0] >> 3) * 8, (hdr[1] >> 4) * 16) : (hdr[0], hdr[1]);

	static string Hex(byte[] b) => Convert.ToHexString(b).ToLowerInvariant();

	static JsonObject ImageMeta(ImageObj img, byte[] palette, string folder, string oid)
	{
		var meta = new JsonObject { ["type"] = img.Type, ["count"] = img.Count };
		if(img.Type == 1 || img.Type == 3) {
			var cells = img.Cells().ToList();
			var rects = cells.Select(c => CellRect(img, c.hdr)).ToList();
			bool overlap = false;
			for(int i = 0; i < rects.Count && !overlap; i++)
				for(int j = i + 1; j < rects.Count; j++)
					if(Math.Abs(rects[i].x - rects[j].x) < 8 && Math.Abs(rects[i].y - rects[j].y) < 16) { overlap = true; break; }
			int x0 = rects.Select(r => r.x).Append(0).Min(), y0 = rects.Select(r => r.y).Append(0).Min();
			int w = rects.Select(r => r.x + 8).Append(8).Max() - x0, h = rects.Select(r => r.y + 16).Append(16).Max() - y0;
			var idx = new byte[w * h];
			for(int k = 0; k < cells.Count; k++) {
				var rows = Gfx.DecodeCell(cells[k].cell, 0);
				var (rx, ry) = rects[k];
				for(int r = 0; r < 16; r++)
					for(int c = 0; c < 8; c++) idx[(ry - y0 + r) * w + rx - x0 + c] = (byte)((cells[k].hdr[2] & 3) * 4 + rows[r, c]);
			}
			Png.WriteIndexed(Path.Combine(folder, oid + ".png"), w, h, idx, Gfx.PaletteRgb(palette));
			meta["origin"] = new JsonArray(x0, y0);
			meta["cells"] = new JsonArray(cells.Select(c => (JsonNode)new JsonArray(c.hdr.Select(b => (JsonNode)(int)b).ToArray())).ToArray());
			if(overlap) {
				meta["cell_data"] = new JsonArray(cells.Select(c => (JsonNode)Hex(c.cell)).ToArray());
				meta["note"] = "cells overlap: the PNG is a preview, cell_data holds the pixels";
			}
		} else if(img.Type == 2) {
			var (w, h, idx, pal, hdr) = Gra.ToIndexed(img.Body);
			Png.WriteIndexed(Path.Combine(folder, oid + ".png"), w, h, idx, Gfx.PaletteRgb(pal));
			meta["palette"] = new JsonArray(pal.Select(b => (JsonNode)(int)b).ToArray());
			meta["header"] = Hex(hdr);
		} else if(img.Type == 0) {
			try {
				string t = Gbk.Decode(img.Body, strict: true);
				if(!Gbk.Encode(t).SequenceEqual(img.Body)) throw new DecoderFallbackException();
				meta["text"] = t;
			} catch(DecoderFallbackException) {
				meta["hex"] = Hex(img.Body);
			}
		} else {
			meta["chars"] = new JsonArray(Enumerable.Range(0, img.Count).Select(i => (JsonNode)new JsonArray(img.Body[(i * 7)..(i * 7 + 7)].Select(b => (JsonNode)(int)b).ToArray())).ToArray());
		}
		return meta;
	}

	static byte[] Bytes(JsonNode arr) => arr.AsArray().Select(x => (byte)x.GetValue<int>()).ToArray();

	static Picture ReadIndexed(string path)
	{
		var p = Png.Read(path);
		if(p.Index == null) throw new InvalidDataException(Path.GetFileName(path) + " must stay an indexed (palette) PNG: its indexes are the colours");
		return p;
	}

	static ImageObj ImageFromMeta(JsonNode meta, string folder, string oid, string name)
	{
		int t = meta["type"].GetValue<int>();
		if(t == 1 || t == 3) {
			var cells = meta["cells"].AsArray().Select(Bytes).ToList();
			var body = new List<byte>();
			if(meta["cell_data"] != null) {
				var data = meta["cell_data"].AsArray().Select(x => Convert.FromHexString(x.GetValue<string>())).ToList();
				for(int i = 0; i < cells.Count; i++) { body.AddRange(cells[i]); body.AddRange(data[i]); }
			} else {
				var im = ReadIndexed(Path.Combine(folder, oid + ".png"));
				int x0 = meta["origin"]?[0]?.GetValue<int>() ?? 0, y0 = meta["origin"]?[1]?.GetValue<int>() ?? 0;
				foreach(var hd in cells) {
					var (rx, ry) = t == 1 ? ((hd[0] >> 3) * 8, (hd[1] >> 4) * 16) : (hd[0], hd[1]);
					var rows = new int[16, 8];
					for(int r = 0; r < 16; r++) for(int c = 0; c < 8; c++) rows[r, c] = im.Index[(ry - y0 + r) * im.W + rx - x0 + c] & 3;
					body.AddRange(hd);
					body.AddRange(Gfx.EncodeCell(rows));
				}
			}
			return new ImageObj(name, t, cells.Count, body.ToArray());
		}
		if(t == 2) {
			var im = ReadIndexed(Path.Combine(folder, oid + ".png"));
			return new ImageObj(name, 2, meta["count"].GetValue<int>(), Gra.FromIndexed(im.W, im.H, im.Index, Bytes(meta["palette"]), Convert.FromHexString(meta["header"].GetValue<string>())));
		}
		if(t == 0) {
			var body = meta["text"] != null ? Gbk.Encode(meta["text"].GetValue<string>()) : Convert.FromHexString(meta["hex"].GetValue<string>());
			return new ImageObj(name, 0, body.Length, body);
		}
		var chars = meta["chars"].AsArray().Select(Bytes).ToList();
		return new ImageObj(name, 4, chars.Count, chars.SelectMany(c => c).ToArray());
	}

	//--- extract ------------------------------------------------------------------------------

	public static (ResReader r, int stale) ExtractRes(byte[] d, Dictionary<string, string> roles, string folder, Action<string> log)
	{
		Directory.CreateDirectory(folder);
		var r = new ResReader(d, roles).Read();
		foreach(var p in r.Problems) log(p);
		//A display palette for anonymous images: the palette of the first animation using them
		var view = new Dictionary<Obj, byte[]>(ReferenceEqualityComparer.Instance);
		foreach(var o in r.Top) {
			byte[] pal = o is ImgRes ir ? ir.Palette : o is AnimRes ar ? ar.Palette : null;
			Anim anim = o is AnimRes ar2 ? ar2.Anim : o is Act ac ? ac.Anim : o as Anim;
			if(o is Act act) pal = act.Raw[9..25];
			if(anim != null && pal != null && pal.Length > 0)
				foreach(var f in anim.Frames) view.TryAdd(f.Image, pal);
		}
		var ids = new Dictionary<Obj, string>(ReferenceEqualityComparer.Instance);
		foreach(var o in r.Top) {
			ids[o] = o.Name;
			if(o is ImgRes ir) ids[ir.Image] = o.Name;
			if(o is AnimRes ar) ids[ar.Anim] = o.Name;
		}
		var objs = new JsonObject();
		foreach(var o in r.Top) {
			string oid = o.Name;
			var m = new JsonObject { ["kind"] = o.Kind, ["offset"] = o.Offset };
			switch(o) {
				case ImgRes ir:
					m["palette"] = new JsonArray(ir.Palette.Select(b => (JsonNode)(int)b).ToArray());
					m["image"] = ImageMeta(ir.Image, ir.Palette, folder, oid);
					break;
				case ImageObj img:
					m["image"] = ImageMeta(img, view.GetValueOrDefault(img) ?? Enumerable.Range(0, 16).Select(i => (byte)i).ToArray(), folder, oid);
					break;
				case AnimRes or Anim: {
					var a = o is AnimRes ar ? ar.Anim : (Anim)o;
					if(o is AnimRes ar3) m["palette"] = new JsonArray(ar3.Palette.Select(b => (JsonNode)(int)b).ToArray());
					m["flags"] = a.Flags;
					m["frames"] = new JsonArray(a.Frames.Select(f => (JsonNode)new JsonArray(f.X, f.Y, ids[f.Image], f.Xo, f.Yo, f.Mode, f.Kind)).ToArray());
					break;
				}
				case Act act:
					m["raw"] = Hex(act.Raw);
					m["anim"] = act.Anim != null ? ids[act.Anim] : null;
					break;
				case Speech sp: {
					string sub = Path.Combine(folder, oid);
					Directory.CreateDirectory(sub);
					m["order"] = new JsonArray(sp.Order.Select(s => s.raw ? (JsonNode)new JsonArray("addr", s.v) : s.v).ToArray());
					m["places"] = sp.Places == null ? null : new JsonArray(sp.Places.Select(p => (JsonNode)p).ToArray());
					var files = new JsonArray();
					for(int i = 0; i < sp.Phrases.Count; i++) {
						//named by the word= that says it (the first table slot pointing at it), not the storage order:
						//a stale slot would otherwise shift every later file one below its word=
						int word = sp.Order.IndexOf((false, i));
						string fn = $"{word:D2}.lpc";
						File.WriteAllBytes(Path.Combine(sub, fn), sp.Phrases[i]);
						Lpc.WriteWav(Path.Combine(sub, $"{word:D2}.wav"), Lpc.Synth(sp.Phrases[i]));
						files.Add((JsonNode)(oid + "/" + fn));
					}
					m["phrases"] = files;
					break;
				}
				case MusicObj mu:
					//text files get the platform's line ends
					File.WriteAllText(Path.Combine(folder, oid + ".mus"), mu.Listing.Replace("\n", Environment.NewLine));
					break;
				case RawObj raw:
					File.WriteAllBytes(Path.Combine(folder, oid + ".bin"), raw.Data);
					break;
			}
			objs[oid] = m;
		}
		var entries = new JsonArray(r.Entries.Select(e => (JsonNode)new JsonArray(e.name, ids[r.Named[e.name]])).ToArray());
		var mask = r.Coverage();
		int stale = 0;
		for(int i = 0; i < d.Length; i++) if(mask[i] == 0 && d[i] != 0xFF) stale++;
		WriteJson(Path.Combine(folder, "objects.json"), new JsonObject { ["size"] = d.Length, ["entries"] = entries, ["objects"] = objs });
		return (r, stale);
	}

	/// <summary>disk: a FAT12 image, or a folder holding the title's files</summary>
	public static List<string> Extract(string disk, string outDir, Action<string> log)
	{
		Func<string, byte[]> get;
		List<string> names;
		if(Directory.Exists(disk)) {
			var files = Directory.GetFiles(disk).ToDictionary(f => Path.GetFileName(f).ToUpperInvariant());
			get = n => File.ReadAllBytes(files[n.ToUpperInvariant()]);
			names = files.Keys.ToList();
		} else {
			var fs = new Fat12(File.ReadAllBytes(disk));
			get = fs.Read;
			names = fs.Files();
		}
		var raw = get("SDOS.DSP");
		string text = DecodeScript(raw);
		if(!EncodeScript(text).SequenceEqual(raw)) throw new InvalidDataException("SDOS.DSP does not round-trip");
		Directory.CreateDirectory(outDir);
		File.WriteAllText(Path.Combine(outDir, "script.txt"), text, new UTF8Encoding(false));
		var (roles, bgs) = ScriptRoles(text);
		var proj = new JsonObject { ["script"] = "script.txt", ["res"] = new JsonObject(), ["backgrounds"] = new JsonObject() };
		var report = new List<string>();
		foreach(var resname in new[] { "SDOS.RES" }.Concat(roles.Keys.Where(n => n != "SDOS.RES").OrderBy(n => n, StringComparer.Ordinal))) {
			if(!names.Contains(resname)) { log("missing " + resname); continue; }
			string rel = "res/" + resname.Split('.')[0].ToLowerInvariant();
			var (r, stale) = ExtractRes(get(resname), roles.GetValueOrDefault(resname), Path.Combine(outDir, rel), log);
			proj["res"][resname] = rel;
			report.Add($"{resname}: {r.Top.Count} objects, {r.Entries.Count} entries, {stale} stale bytes");
		}
		Directory.CreateDirectory(Path.Combine(outDir, "backgrounds"));
		foreach(var b in bgs) {
			if(!names.Contains(b)) { log("missing background " + b); continue; }
			var (w, h, idx, pal, hdr) = Gra.ToIndexed(get(b));
			string stem = b.Split('.')[0].ToLowerInvariant();
			Png.WriteIndexed(Path.Combine(outDir, "backgrounds", stem + ".png"), w, h, idx, Gfx.PaletteRgb(pal));
			WriteJson(Path.Combine(outDir, "backgrounds", stem + ".json"), new JsonObject { ["palette"] = new JsonArray(pal.Select(x => (JsonNode)(int)x).ToArray()), ["header"] = Hex(hdr) });
			proj["backgrounds"][b] = "backgrounds/" + stem;
		}
		WriteJson(Path.Combine(outDir, "project.json"), proj);
		return report;
	}

	//--- build -----------------------------------------------------------------------------------

	/// <summary>The BG palette a background of the project will have (res folders sit at res/name)</summary>
	public static byte[] BackgroundPalette(string resFolder, string stem)
	{
		if(string.IsNullOrEmpty(stem)) throw new InvalidDataException("a glyph img needs \"palette\" or \"palette_from\": <background name>");
		string b = Path.Combine(resFolder, "..", "..", "backgrounds", stem.Split('.')[0].ToLowerInvariant());
		if(File.Exists(b + ".json")) return Bytes(ReadJson(b + ".json")["palette"]);
		return Author.Background(Png.Read(b + ".png")).pal16;
	}

	static (Picture, string) Pic(string folder, string name) => (Png.Read(Path.Combine(folder, name)), name);

	static ImageObj Sprites(string id, List<(byte[] hdr, byte[] cell)> cells)
		=> new(id, 3, cells.Count, cells.SelectMany(c => c.hdr.Concat(c.cell)).ToArray());

	/// <summary>skipAudio: leave recordings unencoded (an empty phrase each), and let music that
	/// does not assemble through as a stub - for previews</summary>
	public static (List<(string name, Obj obj)> entries, List<Obj> objects, JsonNode meta) LoadRes(string folder, bool skipAudio = false)
	{
		var meta = ReadJson(Path.Combine(folder, "objects.json"));
		var objects = meta["objects"].AsObject();
		var objs = new List<(string id, Obj o)>();
		var byId = new Dictionary<string, Obj>();
		void Put(string id, Obj o)
		{
			//a name assigned again keeps its place in the order
			int at = objs.FindIndex(x => x.id == id);
			if(at >= 0) objs[at] = (id, o); else objs.Add((id, o));
			byId[id] = o;
		}
		ImageObj GetImage(string id) => byId[id] is ImgRes ir ? ir.Image : (ImageObj)byId[id];
		var pending = new List<(Obj target, JsonNode spec)>();
		bool Has(JsonNode m, string k) => m is JsonObject jo && jo.ContainsKey(k);
		bool True(JsonNode m, string k) => Has(m, k) && m[k] is JsonValue v && v.TryGetValue(out bool b) && b;
		string Str(JsonNode n) => n is JsonValue v && v.TryGetValue(out string s) ? s : n?.ToString();

		//Pictures converted to sprites share one palette per pack; "own_palette": true or an
		//explicit "palette" converts an object on its own instead
		var sharedSrc = new List<string>();
		foreach(var (_, m) in objects) {
			if(True(m, "own_palette") || Has(m, "palette") || True(m, "glyphs")) continue;
			string k = m["kind"].GetValue<string>();
			if(k == "img" && Has(m, "source")) sharedSrc.Add(m["source"].GetValue<string>());
			else if((k == "animate" || k == "anim" || k == "act") && Has(m, "frames"))
				sharedSrc.AddRange(m["frames"].AsArray().Select(f => Str(f[2])).Where(s => s.ToLowerInvariant().EndsWith(".png")));
		}
		byte[] shared = null;
		if(sharedSrc.Count > 0) shared = Author.SpritePalette(sharedSrc.Distinct().OrderBy(s => s, StringComparer.Ordinal).Select(s => Pic(folder, s)).ToList());
		IList<byte> SpritePal(JsonNode m) => Has(m, "palette") ? Bytes(m["palette"]) : True(m, "own_palette") ? null : shared;

		foreach(var (oid, m) in objects) {
			string k = m["kind"].GetValue<string>();
			Obj o;
			if(k == "img" && Has(m, "source") && True(m, "glyphs")) {
				var pal = Has(m, "palette") ? Bytes(m["palette"]) : BackgroundPalette(folder, m["palette_from"]?.GetValue<string>());
				var cells = Author.GlyphCells(Png.Read(Path.Combine(folder, m["source"].GetValue<string>())), pal);
				o = new ImgRes(oid, pal.ToArray(), new ImageObj(oid + "_image", 1, cells.Count, cells.SelectMany(c => c.hdr.Concat(c.cell)).ToArray()));
			} else if(k == "img" && Has(m, "source")) {
				var (pal, sets) = Author.SpriteImages(new[] { Pic(folder, m["source"].GetValue<string>()) }, m["first_sprite"]?.GetValue<int>() ?? 0, SpritePal(m));
				o = new ImgRes(oid, pal, Sprites(oid + "_image", sets[0]));
			} else if(k == "act" && Has(m, "frames")) {
				//A hover effect made from pictures: XDOS's music and speech branches for acts misread
				//their own fields, so only the animation is offered
				var frames = m["frames"].AsArray().Select(f => f.AsArray().Select(x => x.DeepClone()).ToList()).ToList();
				var pngs = frames.Select(f => Str(f[2])).Distinct().OrderBy(s => s, StringComparer.Ordinal).ToList();
				var (pal, sets) = Author.SpriteImages(pngs.Select(p => Pic(folder, p)).ToList(), m["first_sprite"]?.GetValue<int>() ?? 40, SpritePal(m));
				for(int i = 0; i < pngs.Count; i++) Put($"{oid}:{pngs[i]}", Sprites($"{oid}:{pngs[i]}", sets[i]));
				var a = new Anim(oid + ":anim", 0);
				Put(oid + ":anim", a);
				var spec = new JsonArray(frames.Select(f => (JsonNode)new JsonArray(f[0].DeepClone(), f[1].DeepClone(), $"{oid}:{Str(f[2])}", f[3].DeepClone(), f[4].DeepClone(), f[5].DeepClone(), f[6].DeepClone())).ToArray());
				pending.Add((a, spec));
				o = new Act(oid, new byte[] { 4, 0, 0, 0, 0, 0, 0, 0, 0 }.Concat(pal).ToArray());
				pending.Add((o, JsonValue.Create(oid + ":anim")));
			} else if(k == "img") {
				o = new ImgRes(oid, Bytes(m["palette"]), ImageFromMeta(m["image"], folder, oid, oid + "_image"));
			} else if(k == "image") {
				o = ImageFromMeta(m["image"], folder, oid, oid);
			} else if(k == "animate" || k == "anim") {
				var frames = new JsonArray(m["frames"].AsArray().Select(f => f.DeepClone()).ToArray());
				var pal = Has(m, "palette") ? Bytes(m["palette"]) : null;
				var pngs = frames.Select(f => Str(f[2])).Where(s => s.ToLowerInvariant().EndsWith(".png")).Distinct().OrderBy(s => s, StringComparer.Ordinal).ToList();
				if(pngs.Count > 0) {
					var (p2, sets) = Author.SpriteImages(pngs.Select(p => Pic(folder, p)).ToList(), m["first_sprite"]?.GetValue<int>() ?? 0, SpritePal(m));
					pal = p2;
					for(int i = 0; i < pngs.Count; i++) Put($"{oid}:{pngs[i]}", Sprites($"{oid}:{pngs[i]}", sets[i]));
					foreach(var f in frames) if(pngs.Contains(Str(f[2]))) f[2] = $"{oid}:{Str(f[2])}";
				}
				var a = new Anim(k == "anim" ? oid : oid + "_anim", m["flags"]?.GetValue<int>() ?? 0);
				o = k == "animate" ? new AnimRes(oid, pal, a) : a;
				pending.Add((a, frames));
			} else if(k == "act") {
				o = new Act(oid, Convert.FromHexString(m["raw"].GetValue<string>()));
				if(m["anim"] != null) pending.Add((o, m["anim"]));
			} else if(k == "speak") {
				var phrases = new List<byte[]>();
				foreach(var pn in m["phrases"].AsArray()) {
					string p = pn.GetValue<string>();
					if(p.ToLowerInvariant().EndsWith(".wav"))
						phrases.Add(skipAudio ? new byte[] { 0, 1, 0xD6, 0xFF } : LpcEnc.EncodeWav(Path.Combine(folder, p), m["gain"]?.GetValue<double>() ?? 1.0));
					else phrases.Add(File.ReadAllBytes(Path.Combine(folder, p)));
				}
				var order = Has(m, "order")
					? m["order"].AsArray().Select(x => x is JsonArray ja ? (true, ja[1].GetValue<int>()) : (false, x.GetValue<int>())).ToList()
					: Enumerable.Range(0, phrases.Count).Select(i => (false, i)).ToList();
				var places = m["places"] is JsonArray pa ? pa.Select(x => x.GetValue<int>()).ToList() : null;
				o = new Speech(oid, order, phrases, null, places);
			} else if(k == "music") {
				try {
					o = new MusicObj(oid, File.ReadAllText(Path.Combine(folder, oid + ".mus")));
				} catch(Exception) when(skipAudio) {
					//a listing with a mistake must not keep the pictures from a preview
					o = new RawObj(oid, new byte[] { 0xFF });
				}
			} else {
				o = new RawObj(oid, File.ReadAllBytes(Path.Combine(folder, oid + ".bin")));
			}
			o.Offset = m["offset"]?.GetValue<int>();
			Put(oid, o);
		}
		foreach(var (target, spec) in pending) {
			if(target is Act act) {
				var a = byId[spec.GetValue<string>()];
				act.Anim = a is AnimRes ar ? ar.Anim : (Anim)a;
			} else {
				var an = (Anim)target;
				an.Frames = spec.AsArray().Select(f => new Frame {
					X = f[0].GetValue<int>(), Y = f[1].GetValue<int>(), Image = GetImage(Str(f[2])),
					Xo = f[3].GetValue<int>(), Yo = f[4].GetValue<int>(), Mode = f[5].GetValue<int>(), Kind = f[6].GetValue<int>()
				}).ToList();
			}
		}
		List<(string, Obj)> entries;
		if(meta["entries"] is JsonArray ea) entries = ea.Select(e => (e[0].GetValue<string>(), byId[e[1].GetValue<string>()])).ToList();
		else entries = objs.Where(x => x.o.Kind != "image" && x.o.Kind != "anim").Select(x => (x.id, x.o)).ToList();
		return (entries, objs.Select(x => x.o).ToList(), meta);
	}

	public static byte[] BuildRes(string folder, bool pinned = true)
	{
		var (entries, objs, _) = LoadRes(folder);
		Res.Place(objs, pinned);
		return Res.Assemble(entries, objs);
	}

	/// <summary>-> {file name: bytes} for every file of the title</summary>
	public static Dictionary<string, byte[]> Build(string project, bool pinned = true)
	{
		var proj = ReadJson(Path.Combine(project, "project.json"));
		var o = new Dictionary<string, byte[]>();
		string text = File.ReadAllText(Path.Combine(project, proj["script"].GetValue<string>()), Encoding.UTF8);
		o["SDOS.DSP"] = EncodeScript(text);
		foreach(var (name, folder) in proj["res"].AsObject()) o[name] = BuildRes(Path.Combine(project, folder.GetValue<string>()), pinned);
		foreach(var (name, stemNode) in proj["backgrounds"].AsObject()) {
			string stem = stemNode.GetValue<string>();
			string png = Path.Combine(project, stem + ".png");
			if(File.Exists(Path.Combine(project, stem + ".json"))) {
				var m = ReadJson(Path.Combine(project, stem + ".json"));
				var im = ReadIndexed(png);
				o[name] = Gra.FromIndexed(im.W, im.H, im.Index, Bytes(m["palette"]), Convert.FromHexString(m["header"].GetValue<string>()));
			} else {
				//A plain picture: converted to NES colours and attributes here
				var (idx, pal) = Author.Background(Png.Read(png));
				o[name] = Gra.FromIndexed(256, 240, idx, pal);
			}
		}
		return o;
	}

	/// <summary>Build a project onto a copy of a boot disk carrying XDOS. The template's own title
	/// (its script, and the packs and pictures that script names) is taken off first.</summary>
	public static Dictionary<string, byte[]> WriteDisk(string project, string template, string outPath, bool pinned, Action<string> log)
	{
		var files = Build(project, pinned);
		var fs = new Fat12(File.ReadAllBytes(template));
		if(fs.Files().Contains("SDOS.DSP")) {
			var (roles, bgs) = ScriptRoles(DecodeScript(fs.Read("SDOS.DSP")));
			foreach(var n in new[] { "SDOS.DSP", "SDOS.RES" }.Concat(roles.Keys).Concat(bgs)) fs.Delete(n);
		}
		foreach(var (name, data) in files) {
			fs.Write(name, data);
			log($"{name,-12} {data.Length,7} bytes");
		}
		File.WriteAllBytes(outPath, fs.D);
		log($"{fs.FreeBytes()} bytes free on the disk");
		return files;
	}

	/// <summary>Rebuild every file of a disk and compare: the script and pictures must match
	/// exactly, a .RES at every byte an object or the directory accounts for.</summary>
	public static bool Verify(string disk, Action<string> log)
	{
		Func<string, byte[]> get;
		if(Directory.Exists(disk)) get = n => File.ReadAllBytes(Path.Combine(disk, n));
		else get = new Fat12(File.ReadAllBytes(disk)).Read;
		string tmp = Path.Combine(Path.GetTempPath(), "easymake_verify_" + Guid.NewGuid().ToString("N"));
		bool ok = true;
		try {
			foreach(var line in Extract(disk, tmp, s => log("  note: " + s))) log(line);
			var built = Build(tmp);
			var roles = ScriptRoles(DecodeScript(get("SDOS.DSP"))).roles;
			foreach(var (name, data) in built) {
				var orig = get(name);
				if(!name.EndsWith(".RES")) {
					bool same = data.SequenceEqual(orig);
					log($"{name,-12} {(same ? "identical" : "DIFFERS")}");
					ok &= same;
					continue;
				}
				var mask = new ResReader(orig, roles.GetValueOrDefault(name)).Read().Coverage();
				var bad = Enumerable.Range(0, orig.Length).Where(i => mask[i] != 0 && (i >= data.Length || data[i] != orig[i])).ToList();
				bool extra = Enumerable.Range(orig.Length, Math.Max(0, data.Length - orig.Length)).Any(i => data[i] != 0xFF);
				log($"{name,-12} {mask.Count(b => b != 0)} of {orig.Length} bytes accounted for, {bad.Count} differ" + (bad.Count > 0 ? $" (first at {bad[0]:X})" : ""));
				ok &= bad.Count == 0 && !extra;
			}
		} finally {
			try { Directory.Delete(tmp, true); } catch(IOException) { }
		}
		log("VERIFY " + (ok ? "OK" : "FAILED"));
		return ok;
	}
}
