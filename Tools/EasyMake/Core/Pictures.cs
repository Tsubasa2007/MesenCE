namespace EasyMake.Core;

/// <summary>The machine's picture files, read and written:
/// <list type="bullet">
/// <item>GR (.GRA): BIOS call $42's pictures - see Gra. Version 0 holds every tile; version 1
/// holds each distinct tile once and a tile map, in horizontal bands of up to 256 tiles each.</item>
/// <item>.Grp / .Pct: the paint program's full pictures and cut-outs - 8x16 cells, each with its
/// own sub-palette.</item>
/// </list>
/// Formats are told apart by their headers, never by the file name: disks carry pictures with no
/// extension, or inside programs and resource packs.</summary>
public static class Pictures
{
	public enum Format { Gra, GraTiles, Grp, Pct }

	public static string Name(Format f) => f switch {
		Format.Gra => "GRA", Format.GraTiles => "GRA (tiles)", Format.Grp => "Grp", _ => "Pct"
	};

	/// <summary>The paint program's canvas; a .Grp is always this size. The program shows only
	/// the first ShownW columns (its pointer stops at x 215; the last 8-pixel column is left
	/// blank on screen), so pictures are converted into that width and a .Pct fits in it. The
	/// tools still show all of a file's columns.</summary>
	public const int CanvasW = 224, CanvasH = 208, ShownW = 216;

	/// <summary>A picture as indexes = sub-palette * 4 + colour, with a 16-byte palette</summary>
	public sealed record Indexed(int W, int H, byte[] Index, byte[] Pal16)
	{
		public Picture ToPicture() => Picture.Indexed(W, H, Index, Gfx.PaletteRgb(Pal16));
	}

	/// <summary>A picture in a file: where it starts, how many bytes it takes and what kind it is</summary>
	public sealed record Found(int Offset, int Length, Format Kind, Indexed Pic);

	static bool ColoursOk(byte[] d, int o, int n)
	{
		for(int i = 0; i < n; i++) if(d[o + i] >= 0x40) return false;
		return true;
	}

	/// <summary>The picture whose header is at o, or null when there is none (or the bytes do not
	/// hold together as one - a header found inside a program has to prove itself)</summary>
	public static Found TryDecode(byte[] d, int o)
	{
		if(o + 32 > d.Length) return null;
		if(d[o] == 'G' && d[o + 1] == 'R' && d[o + 2] <= 1 && d[o + 3] == 0) return TryGr(d, o);
		if(d[o] == '.' && (d[o + 1] == 'G' && d[o + 2] == 'r' && d[o + 3] == 'p' || d[o + 1] == 'P' && d[o + 2] == 'c' && d[o + 3] == 't'))
			return TryCells(d, o);
		return null;
	}

	static (int ai, int shift) AttrIndex(int w, int x, int y)
		=> ((y / 32) * (w / 32) + x / 32, ((y / 16) & 1) * 4 + ((x / 16) & 1) * 2);

	static Found TryGr(byte[] d, int o)
	{
		int w = d[o + 4] | d[o + 5] << 8, h = d[o + 6] | d[o + 7] << 8;
		if(w == 0 || h == 0 || w > 256 || h > 256 || w % 32 != 0 || h % 8 != 0) return null;
		for(int i = 8; i < 16; i++) if(d[o + i] != 0) return null;
		if(!ColoursOk(d, o + 16, 16)) return null;
		int tw = w / 8, na = (w / 32) * ((h + 31) / 32), p = o + 32 + na;
		if(d[o + 2] == 0) {
			int end = p + tw * (h / 8) * 16;
			if(end > d.Length) return null;
			var g = Gra.ToIndexed(d, o);
			return new Found(o, end - o, Format.Gra, new Indexed(w, h, g.index, g.pal));
		}
		//Version 1: bands of (lines, tile count, the tiles, the band's tile map), down to the height.
		//Each band brings its own tiles, so a picture with more than 256 distinct ones splits.
		var idx = new byte[w * h];
		for(int y0 = 0; y0 < h;) {
			if(p + 2 > d.Length) return null;
			int lines = d[p], count = d[p + 1] == 0 ? 256 : d[p + 1];
			if(lines == 0 || lines % 8 != 0 || y0 + lines > h) return null;
			int tiles = p + 2, map = tiles + count * 16, end = map + lines / 8 * tw;
			if(end > d.Length) return null;
			for(int ty = 0; ty < lines / 8; ty++)
				for(int tx = 0; tx < tw; tx++) {
					int t = d[map + ty * tw + tx];
					if(t >= count) return null;
					var px = Gfx.DecodeTile(d, tiles + t * 16);
					for(int r = 0; r < 8; r++)
						for(int c = 0; c < 8; c++) {
							int x = tx * 8 + c, y = y0 + ty * 8 + r;
							var (ai, sh) = AttrIndex(w, x, y);
							idx[y * w + x] = (byte)((d[o + 32 + ai] >> sh & 3) * 4 + px[r, c]);
						}
				}
			y0 += lines;
			p = end;
		}
		return new Found(o, p - o, Format.GraTiles, new Indexed(w, h, idx, d[(o + 16)..(o + 32)]));
	}

	/// <summary>.Grp / .Pct: magic, palette length (4, 8 or 12), rows of 16 pixels, columns of 8,
	/// 0, a 12-byte palette (three sub-palettes), then the cells row by row: a sub-palette byte
	/// and an 8x16 cell (two NES tiles, top then bottom).
	/// Each cell stores a sub-palette, but the machine colours 16x16 areas: two neighbouring
	/// cells share one, and the right one's (written last) is what shows - a left cell whose
	/// own differs comes out in its neighbour's colours. The picture is returned as shown, pairs
	/// counted from its left edge (where the paint program's canvas puts them).</summary>
	static Found TryCells(byte[] d, int o)
	{
		int npal = d[o + 4], rows = d[o + 5], cols = d[o + 6];
		if(npal is not (4 or 8 or 12) || d[o + 7] != 0 || rows == 0 || cols == 0 || rows > 15 || cols > 32) return null;
		if(!ColoursOk(d, o + 8, 12)) return null;
		int end = o + 20 + rows * cols * 33;
		if(end > d.Length) return null;
		var pal = new byte[16];
		Array.Copy(d, o + 8, pal, 0, 12);
		pal[12] = pal[0]; pal[13] = pal[14] = pal[15] = Author.Black;
		int w = cols * 8, h = rows * 16;
		var idx = new byte[w * h];
		for(int i = 0; i < rows * cols; i++) {
			int c = o + 20 + i * 33, shown = i % cols % 2 == 0 && i % cols + 1 < cols ? c + 33 : c, sub = d[shown] & 3;
			var cell = Gfx.DecodeCell(d, c + 1);
			int x0 = i % cols * 8, y0 = i / cols * 16;
			for(int y = 0; y < 16; y++)
				for(int x = 0; x < 8; x++) idx[(y0 + y) * w + x0 + x] = (byte)(sub * 4 + cell[y, x]);
		}
		return new Found(o, end - o, d[o + 1] == 'G' ? Format.Grp : Format.Pct, new Indexed(w, h, idx, pal));
	}

	/// <summary>The pictures in a file. Without inside, only a picture file's own: a header at the
	/// start, or after the two-byte load address some resources carry. With inside, every picture
	/// embedded anywhere in it as well (programs carry their screens).</summary>
	public static List<Found> Scan(byte[] d, bool inside)
	{
		var o = new List<Found>();
		for(int pos = 0; pos + 32 <= d.Length;) {
			if(!inside && pos > 2) break;
			if(d[pos] == 'G' || d[pos] == '.') {
				var f = TryDecode(d, pos);
				if(f != null) {
					o.Add(f);
					pos += f.Length;
					continue;
				}
			}
			pos++;
		}
		return o;
	}

	/// <summary>Every picture on a disk image, as (path on the disk, picture); a picture inside a
	/// file that holds more than one is named path@offset</summary>
	public static IEnumerable<(string name, Found pic)> OnDisk(byte[] image, bool inside)
	{
		var fat = new Fat12(image);
		foreach(var (path, data) in fat.AllFiles()) {
			var found = Scan(data, inside);
			foreach(var f in found)
				yield return (found.Count == 1 && f.Offset <= 2 ? path : $"{path}@{f.Offset:X}", f);
		}
	}

	//--- writing -------------------------------------------------------------------------------

	public static string Extension(Format f) => f switch { Format.Grp => "GRP", Format.Pct => "PCT", _ => "GRA" };

	public static byte[] Encode(Format f, Indexed p) => f switch {
		Format.Gra => Gra.FromIndexed(p.W, p.H, p.Index, p.Pal16),
		Format.GraTiles => EncodeTiles(p),
		_ => EncodeCells(f, p)
	};

	/// <summary>Version 1 GR: the version 0 picture's header, palette and attributes, then bands of
	/// 16 lines gathered while their distinct tiles still fit in one band's 255 (tiles numbered
	/// in the order they first appear)</summary>
	static byte[] EncodeTiles(Indexed p)
	{
		var full = Gra.FromIndexed(p.W, p.H, p.Index, p.Pal16);
		int tw = p.W / 8, th = p.H / 8, tiles = 32 + (p.W / 32) * ((p.H + 31) / 32);
		var o = new List<byte>(full[..tiles]);
		o[2] = 1;
		string Tile(int t) => System.Convert.ToHexString(full, tiles + t * 16, 16);
		for(int row = 0; row < th;) {
			var ids = new Dictionary<string, int>();
			var order = new List<int>();
			int end = row;
			while(end < th) {
				int step = Math.Min(2, th - end);
				var add = new HashSet<string>();
				for(int t = end * tw; t < (end + step) * tw; t++)
					if(!ids.ContainsKey(Tile(t))) add.Add(Tile(t));
				if(ids.Count + add.Count > 255 || (end - row + step) * 8 > 255) break;
				for(int t = end * tw; t < (end + step) * tw; t++)
					if(!ids.ContainsKey(Tile(t))) { ids[Tile(t)] = ids.Count; order.Add(t); }
				end += step;
			}
			if(end == row) throw new InvalidDataException("a band of the picture has more than 255 different tiles");
			o.Add((byte)((end - row) * 8));
			o.Add((byte)order.Count);
			foreach(int t in order) o.AddRange(full[(tiles + t * 16)..(tiles + t * 16 + 16)]);
			for(int t = row * tw; t < end * tw; t++) o.Add((byte)ids[Tile(t)]);
			row = end;
		}
		return o.ToArray();
	}

	static byte[] EncodeCells(Format f, Indexed p)
	{
		if(p.W % 8 != 0 || p.H % 16 != 0 || p.W / 8 > 32 || p.H / 16 > 15)
			throw new ArgumentException("a Grp/Pct picture is up to 256 x 240, in 8 x 16 cells");
		int rows = p.H / 16, cols = p.W / 8;
		var subs = new byte[rows * cols];
		var cells = new List<byte[]>();
		for(int i = 0; i < rows * cols; i++) {
			int x0 = i % cols * 8, y0 = i / cols * 16;
			var px = new int[16, 8];
			for(int y = 0; y < 16; y++)
				for(int x = 0; x < 8; x++) px[y, x] = p.Index[(y0 + y) * p.W + x0 + x] & 3;
			cells.Add(Gfx.EncodeCell(px));
		}
		//One sub-palette per 16x16 area (see TryCells): both cells of a pair get it, a blank cell
		//too - its own would otherwise be the one that shows
		for(int r = 0; r < rows; r++)
			for(int c = 0; c < cols; c += 2) {
				int x0 = c * 8, y0 = r * 16, x1 = Math.Min(p.W, x0 + 16);
				var used = new HashSet<int>();
				for(int y = y0; y < y0 + 16; y++)
					for(int x = x0; x < x1; x++) {
						int v = p.Index[y * p.W + x];
						if((v & 3) != 0) used.Add(v >> 2);
					}
				if(used.Count > 1) throw new ArgumentException($"the 16x16 area at ({x0},{y0}) uses more than one sub-palette");
				int sub = used.Count == 1 ? used.First() : 0;
				if(sub > 2) throw new ArgumentException("a Grp/Pct picture has three sub-palettes");
				subs[r * cols + c] = (byte)sub;
				if(c + 1 < cols) subs[r * cols + c + 1] = (byte)sub;
			}
		int npal = (subs.DefaultIfEmpty().Max() + 1) * 4;
		var o = new List<byte>(System.Text.Encoding.ASCII.GetBytes(f == Format.Grp ? ".Grp" : ".Pct")) {
			(byte)npal, (byte)rows, (byte)cols, 0 };
		o.AddRange(p.Pal16.Take(12));
		for(int i = 0; i < cells.Count; i++) { o.Add(subs[i]); o.AddRange(cells[i]); }
		return o.ToArray();
	}

	/// <summary>Any picture -> the format's picture: GRA fills the 256 x 240 screen and a Grp the
	/// 216 x 208 of the paint program's canvas it shows (the hidden last column takes the
	/// backdrop); a Pct keeps its size (made smaller to fit, and padded to whole cells). Grp and
	/// Pct get 16x16 colour areas, as the machine shows them. keepAspect pads with the backdrop
	/// rather than stretching; dither
	/// trades flat areas for a dot pattern that is closer in colour (good for photographs).</summary>
	public static Indexed Convert(Picture src, Format f, bool keepAspect, bool dither = false)
	{
		int w, h;
		if(f == Format.Pct) {
			double s = Math.Min(1.0, Math.Min((double)ShownW / src.W, (double)CanvasH / src.H));
			w = (int)Math.Round(src.W * s); h = (int)Math.Round(src.H * s);
			src = Fit(src, Math.Max(1, w), Math.Max(1, h), false);
			w = (w + 7) / 8 * 8; h = (h + 15) / 16 * 16;
			src = Pad(src, w, h);
		} else if(f == Format.Grp) {
			(w, h) = (CanvasW, CanvasH);
			var shown = Fit(src, ShownW, CanvasH, keepAspect);
			src = new Picture(w, h);
			for(int y = 0; y < h; y++) Array.Copy(shown.Rgba, y * ShownW * 4, src.Rgba, y * w * 4, ShownW * 4);
		} else {
			(w, h) = (256, 240);
			src = Fit(src, w, h, keepAspect);
		}
		bool cells = f is Format.Grp or Format.Pct;
		var (idx, pal) = Author.Paint(src, 16, 16, cells ? 3 : 4, dither);
		return new Indexed(w, h, idx, pal);
	}

	/// <summary>The picture on a w x h canvas: stretched, or (keepAspect) scaled to fit and
	/// centred, the rest transparent</summary>
	static Picture Fit(Picture src, int w, int h, bool keepAspect)
	{
		if(src.W == w && src.H == h) return src;
		if(!keepAspect) return src.Resize(w, h);
		double s = Math.Min((double)w / src.W, (double)h / src.H);
		int iw = Math.Max(1, (int)Math.Round(src.W * s)), ih = Math.Max(1, (int)Math.Round(src.H * s));
		return Pad(iw == src.W && ih == src.H ? src : src.Resize(iw, ih), w, h);
	}

	/// <summary>The picture centred on a larger w x h canvas, the rest transparent</summary>
	static Picture Pad(Picture src, int w, int h)
	{
		if(src.W == w && src.H == h) return src;
		var o = new Picture(w, h);
		int ox = (w - src.W) / 2, oy = (h - src.H) / 2;
		for(int y = 0; y < src.H; y++) Array.Copy(src.Rgba, y * src.W * 4, o.Rgba, ((oy + y) * w + ox) * 4, src.W * 4);
		return o;
	}
}

/// <summary>Finding the pictures on a folder of disk images</summary>
public static class PictureScan
{
	public static List<string> Disks(string folder, bool subfolders)
		=> Directory.EnumerateFiles(folder, "*", subfolders ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly)
			.Where(f => Path.GetExtension(f).ToLowerInvariant() is ".img" or ".ima")
			.OrderBy(f => f, StringComparer.OrdinalIgnoreCase).ToList();

	/// <summary>-> the disk's pictures, or an error when it is not a disk image that can be read</summary>
	public static (List<(string name, Pictures.Found pic)> pics, string error) Read(string disk, bool inside)
	{
		try {
			return (Pictures.OnDisk(File.ReadAllBytes(disk), inside).ToList(), null);
		} catch(Exception e) when(e is InvalidDataException or IOException or ArgumentException or IndexOutOfRangeException) {
			return (new(), e.Message);
		}
	}

	/// <summary>A picture as an indexed PNG</summary>
	public static void SavePng(string path, Pictures.Found f)
		=> Png.WriteIndexed(path, f.Pic.W, f.Pic.H, f.Pic.Index, Gfx.PaletteRgb(f.Pic.Pal16));

	/// <summary>A disk path as a file name here: folders and offsets joined with _</summary>
	public static string SafeName(string name)
		=> string.Join("_", name.Replace('/', '_').Replace('@', '_').Split(Path.GetInvalidFileNameChars()));
}
