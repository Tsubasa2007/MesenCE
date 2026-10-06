namespace EasyMake.Core;

/// <summary>NES palette, 2bpp planar tiles and 8x16 cells</summary>
public static class Gfx
{
	//Mesen's default 2C02 palette, used only to show NES colour numbers as RGB
	static readonly uint[] NesRgbTable = {
		0x666666, 0x002A88, 0x1412A7, 0x3B00A4, 0x5C007E, 0x6E0040, 0x6C0600, 0x561D00,
		0x333500, 0x0B4800, 0x005200, 0x004F08, 0x00404D, 0x000000, 0x000000, 0x000000,
		0xADADAD, 0x155FD9, 0x4240FF, 0x7527FE, 0xA01ACC, 0xB71E7B, 0xB53120, 0x994E00,
		0x6B6D00, 0x388700, 0x0C9300, 0x008F32, 0x007C8D, 0x000000, 0x000000, 0x000000,
		0xFFFEFF, 0x64B0FF, 0x9290FF, 0xC676FF, 0xF36AFF, 0xFE6ECC, 0xFE8170, 0xEA9E22,
		0xBCBE00, 0x88D800, 0x5CE430, 0x45E082, 0x48CDDE, 0x4F4F4F, 0x000000, 0x000000,
		0xFFFEFF, 0xC0DFFF, 0xD3D2FF, 0xE8C8FF, 0xFBC2FF, 0xFEC4EA, 0xFECCC5, 0xF7D8A5,
		0xE4E594, 0xCFEF96, 0xBDF4AB, 0xB3F3CC, 0xB5EBF2, 0xB8B8B8, 0x000000, 0x000000,
	};

	public static (byte r, byte g, byte b) NesRgb(int c)
	{
		uint v = NesRgbTable[c & 0x3F];
		return ((byte)(v >> 16), (byte)(v >> 8), (byte)v);
	}

	public static int Dist((byte r, byte g, byte b) a, (byte r, byte g, byte b) b)
		=> (a.r - b.r) * (a.r - b.r) * 3 + (a.g - b.g) * (a.g - b.g) * 4 + (a.b - b.b) * (a.b - b.b) * 2;

	static readonly HashSet<int> Skip = new() { 0x0D, 0x0E, 0x1D, 0x1E, 0x1F, 0x2E, 0x2F, 0x3E, 0x3F, 0x20 };
	static readonly Dictionary<int, int> NearestCache = new();

	/// <summary>NES colour number closest to an RGB triple (black is $0F). The duplicate blacks
	/// and $20 (the same white as $30 here; XDOS's text is $30) are never chosen.</summary>
	public static int NearestNes((byte r, byte g, byte b) rgb)
	{
		int key = rgb.r << 16 | rgb.g << 8 | rgb.b;
		lock(NearestCache) {
			if(NearestCache.TryGetValue(key, out int hit)) return hit;
		}
		int best = 0x0F, bestd = -1;
		for(int c = 0; c < 64; c++) {
			if(Skip.Contains(c)) continue;
			int d = Dist(NesRgb(c), rgb);
			if(bestd < 0 || d < bestd) { best = c; bestd = d; }
		}
		lock(NearestCache) NearestCache[key] = best;
		return best;
	}

	public static int[,] DecodeTile(byte[] b, int o)
	{
		var rows = new int[8, 8];
		for(int r = 0; r < 8; r++)
			for(int c = 0; c < 8; c++)
				rows[r, c] = (b[o + r] >> (7 - c) & 1) | (b[o + r + 8] >> (7 - c) & 1) << 1;
		return rows;
	}

	/// <summary>32 bytes = an 8x16 cell: two tiles, top then bottom -> [16, 8] pixel values</summary>
	public static int[,] DecodeCell(byte[] b, int o)
	{
		var rows = new int[16, 8];
		for(int half = 0; half < 2; half++) {
			var t = DecodeTile(b, o + half * 16);
			for(int r = 0; r < 8; r++) for(int c = 0; c < 8; c++) rows[half * 8 + r, c] = t[r, c];
		}
		return rows;
	}

	public static byte[] EncodeTile(Func<int, int, int> px)
	{
		var o = new byte[16];
		for(int r = 0; r < 8; r++)
			for(int c = 0; c < 8; c++) {
				int v = px(r, c);
				o[r] |= (byte)((v & 1) << (7 - c));
				o[r + 8] |= (byte)((v >> 1 & 1) << (7 - c));
			}
		return o;
	}

	public static byte[] EncodeCell(int[,] rows)
		=> EncodeTile((r, c) => rows[r, c]).Concat(EncodeTile((r, c) => rows[r + 8, c])).ToArray();

	/// <summary>The PNG palette for an indexed picture: 4 sub-palettes x 4 colours as RGB,
	/// entry p*4+0 showing the backdrop colour as on the NES</summary>
	public static byte[] PaletteRgb(IList<byte> pal16)
	{
		var o = new byte[48];
		for(int i = 0; i < 16; i++) {
			var c = NesRgb(i % 4 == 0 ? pal16[0] : pal16[i]);
			o[i * 3] = c.r; o[i * 3 + 1] = c.g; o[i * 3 + 2] = c.b;
		}
		return o;
	}
}

/// <summary>GR pictures: the .GRA backgrounds and type-2 image objects (BIOS call $42).
/// 16-byte header ("GR", 0, 0, width LE16, height LE16, 8 zeros), 16-byte BG palette, the
/// attribute table ((w/32) x ceil(h/32) bytes, NES layout), then (w/8) x (h/8) NES 2bpp tiles.</summary>
public static class Gra
{
	public static int GrSize(byte[] d, int o = 0)
	{
		int w = d[o + 4] | d[o + 5] << 8, h = d[o + 6] | d[o + 7] << 8;
		return 32 + (w / 32) * ((h + 31) / 32) + w * h / 4;
	}

	static (int idx, int shift) AttrIndex(int w, int x, int y)
		=> ((y / 32) * (w / 32) + x / 32, ((y / 16) & 1) * 4 + ((x / 16) & 1) * 2);

	/// <summary>-> (w, h, indexes = sub-palette*4 + pixel, palette, header)</summary>
	public static (int w, int h, byte[] index, byte[] pal, byte[] header) ToIndexed(byte[] d, int o = 0)
	{
		int w = d[o + 4] | d[o + 5] << 8, h = d[o + 6] | d[o + 7] << 8;
		var pal = d[(o + 16)..(o + 32)];
		int na = (w / 32) * ((h + 31) / 32);
		int tiles = o + 32 + na;
		var idx = new byte[w * h];
		int tw = w / 8;
		for(int ty = 0; ty < h / 8; ty++)
			for(int tx = 0; tx < tw; tx++) {
				var t = Gfx.DecodeTile(d, tiles + (ty * tw + tx) * 16);
				for(int r = 0; r < 8; r++)
					for(int c = 0; c < 8; c++) {
						int x = tx * 8 + c, y = ty * 8 + r;
						var (ai, sh) = AttrIndex(w, x, y);
						idx[y * w + x] = (byte)((d[o + 32 + ai] >> sh & 3) * 4 + t[r, c]);
					}
			}
		return (w, h, idx, pal, d[o..(o + 16)]);
	}

	/// <summary>Inverse of ToIndexed: every 16x16 quarter must use one sub-palette</summary>
	public static byte[] FromIndexed(int w, int h, byte[] idx, IList<byte> pal, byte[] header = null)
	{
		if(w % 32 != 0 || h % 8 != 0) throw new ArgumentException("a GR picture must be a multiple of 32 pixels wide and 8 high");
		var hdr = header != null ? (byte[])header.Clone() : new byte[16];
		hdr[0] = (byte)'G'; hdr[1] = (byte)'R';
		hdr[4] = (byte)w; hdr[5] = (byte)(w >> 8); hdr[6] = (byte)h; hdr[7] = (byte)(h >> 8);
		int na = (w / 32) * ((h + 31) / 32);
		var attr = new byte[na];
		for(int y = 0; y < h; y += 16)
			for(int x = 0; x < w; x += 16) {
				var subs = new HashSet<int>();
				for(int r = 0; r < Math.Min(16, h - y); r++)
					for(int c = 0; c < 16; c++) subs.Add(idx[(y + r) * w + x + c] >> 2);
				if(subs.Count > 1) throw new ArgumentException($"the 16x16 area at ({x},{y}) uses more than one sub-palette");
				var (ai, sh) = AttrIndex(w, x, y);
				attr[ai] |= (byte)(subs.First() << sh);
			}
		var outp = new List<byte>(hdr);
		outp.AddRange(pal.Take(16));
		outp.AddRange(attr);
		for(int ty = 0; ty < h / 8; ty++)
			for(int tx = 0; tx < w / 8; tx++)
				outp.AddRange(Gfx.EncodeTile((r, c) => idx[(ty * 8 + r) * w + tx * 8 + c] & 3));
		return outp.ToArray();
	}
}
