namespace EasyMake.Core;

/// <summary>A counter that remembers the order keys first appeared in: MostCommon is a stable
/// sort by count, so equal counts keep that order (and a conversion always picks the same
/// colours).</summary>
public sealed class Counter<T>
{
	readonly Dictionary<T, int> index = new();
	public readonly List<T> Keys = new();
	public readonly List<int> Counts = new();

	public void Add(T k, int n = 1)
	{
		if(index.TryGetValue(k, out int i)) Counts[i] += n;
		else { index[k] = Keys.Count; Keys.Add(k); Counts.Add(n); }
	}

	public bool Empty => Keys.Count == 0;
	public IEnumerable<(T key, int n)> Items() => Keys.Zip(Counts);
	public IEnumerable<(T key, int n)> MostCommon() => Items().OrderByDescending(x => x.n);
}

/// <summary>Turning ordinary pictures into the machine's graphics. Every choice is
/// deterministic, so the same pictures always build the same bytes.</summary>
public static class Author
{
	public const int Black = 0x0F, TextColour = 0x30, None = -1;

	static int Nes((byte r, byte g, byte b) c) => Gfx.NearestNes(c);
	static int D(int a, int b) => Gfx.Dist(Gfx.NesRgb(a), Gfx.NesRgb(b));

	/// <summary>Pick n_pal sets of up to 'per' colours that cover the most pixels when every block
	/// uses its best set; seed: palette index -> a colour that palette must hold first.
	/// Unused places hold 'backdrop' (None for sprites).</summary>
	static List<List<int>> ChoosePalettes(List<Counter<int>> blocks, int backdrop, int nPal = 4, int per = 3, Dictionary<int, int> seed = null)
	{
		var want = new Counter<string>();
		var combos = new Dictionary<string, int[]>();
		foreach(var b in blocks) {
			var top = b.MostCommon().Take(per + 1).Select(x => x.key).Where(c => c != backdrop).OrderBy(c => c).Take(per).ToArray();
			string key = string.Join(",", top);
			combos[key] = top;
			want.Add(key, b.Items().Where(x => top.Contains(x.key)).Sum(x => x.n));
		}
		seed ??= new Dictionary<int, int>();
		var pals = new List<HashSet<int>>();
		for(int i = 0; i < nPal; i++) pals.Add(seed.TryGetValue(i, out int s) ? new HashSet<int> { s } : null);
		foreach(var (key, _) in want.MostCommon()) {
			var combo = combos[key];
			if(combo.Length == 0) continue;
			var fit = pals.FirstOrDefault(p => p != null && (combo.All(p.Contains) || p.Union(combo).Count() <= per));
			if(fit != null) fit.UnionWith(combo);
			else {
				int free = pals.IndexOf(null);
				if(free >= 0) pals[free] = new HashSet<int>(combo);
			}
		}
		var o = new List<List<int>>();
		for(int i = 0; i < nPal; i++) {
			var p = (pals[i] ?? new HashSet<int>()).OrderBy(c => c).ToList();
			if(seed.TryGetValue(i, out int s)) { p.Remove(s); p.Insert(0, s); }
			while(p.Count < per) p.Add(backdrop);
			o.Add(p);
		}
		return o;
	}

	static int BestPalette(Counter<int> block, List<List<int>> pals)
	{
		int best = 0; long bestc = -1;
		for(int i = 0; i < pals.Count; i++) {
			long cost = 0;
			foreach(var (c, n) in block.Items()) cost += (long)n * pals[i].Min(q => D(c, q));
			if(bestc < 0 || cost < bestc) { best = i; bestc = cost; }
		}
		return best;
	}

	static int ArgMin(int count, Func<int, int> f)
	{
		int best = 0, bv = f(0);
		for(int k = 1; k < count; k++) { int v = f(k); if(v < bv) { best = k; bv = v; } }
		return best;
	}

	/// <summary>Any RGB picture -> (indexes = sub-palette*4 + colour, 16-byte palette) following
	/// XDOS's conventions: black backdrop, sub-palette 3's first colour is the text colour, and
	/// colours shared with sub-palette 3 keep its index (printing switches nearby areas to it).</summary>
	public static (byte[] index, byte[] pal16) Background(Picture src)
	{
		if(src.W != 256 || src.H != 240) src = src.Resize(256, 240);
		var col = new int[240, 256];
		for(int y = 0; y < 240; y++)
			for(int x = 0; x < 256; x++) {
				var c = src.Get(x, y);
				col[y, x] = Nes((c.r, c.g, c.b));
			}
		int backdrop = Black;
		var blocks = new List<Counter<int>>();
		for(int by = 0; by < 240; by += 16)
			for(int bx = 0; bx < 256; bx += 16) {
				var b = new Counter<int>();
				for(int y = by; y < Math.Min(by + 16, 240); y++) for(int x = bx; x < bx + 16; x++) b.Add(col[y, x]);
				blocks.Add(b);
			}
		var pals = ChoosePalettes(blocks, backdrop, seed: new Dictionary<int, int> { [3] = TextColour });
		for(int pi = 0; pi < 3; pi++) {
			var p = pals[pi];
			for(int j = 0; j < pals[3].Count; j++) {
				int c = pals[3][j];
				if(p.Contains(c) && c != backdrop && p[j] != pals[3][j]) {
					int i = p.IndexOf(c);
					(p[i], p[j]) = (p[j], p[i]);
				}
			}
		}
		var pal16 = new List<byte>();
		foreach(var p in pals) { pal16.Add((byte)backdrop); pal16.AddRange(p.Select(c => (byte)c)); }
		var withBack = pals.Select(p => new List<int> { backdrop }.Concat(p).ToList()).ToList();
		var idx = new byte[256 * 240];
		int bi = 0;
		for(int by = 0; by < 240; by += 16)
			for(int bx = 0; bx < 256; bx += 16) {
				int sp = BestPalette(blocks[bi], withBack);
				var choices = withBack[sp];
				for(int y = by; y < Math.Min(by + 16, 240); y++)
					for(int x = bx; x < bx + 16; x++) {
						int c = col[y, x];
						idx[y * 256 + x] = (byte)(sp * 4 + ArgMin(4, k => D(c, choices[k])));
					}
				bi++;
			}
		return (idx, pal16.ToArray());
	}

	/// <summary>A picture -> BG glyph cells (image type 1) in a given BG palette. One sub-palette per
	/// 16x16 area; cells that are all black are dropped.</summary>
	public static List<(byte[] hdr, byte[] cell)> GlyphCells(Picture src, IList<byte> pal16)
	{
		if(src.W > 256 || src.H > 240) throw new InvalidDataException("a glyph picture must fit on the screen");
		int w16 = (src.W + 15) / 16 * 16, h16 = (src.H + 15) / 16 * 16;
		var col = new int[h16, w16];
		for(int y = 0; y < h16; y++)
			for(int x = 0; x < w16; x++) {
				if(x < src.W && y < src.H) { var c = src.Get(x, y); col[y, x] = Nes((c.r, c.g, c.b)); }
				else col[y, x] = Black;
			}
		var subs = Enumerable.Range(0, 4).Select(i => new List<int> { pal16[0], pal16[i * 4 + 1], pal16[i * 4 + 2], pal16[i * 4 + 3] }).ToList();
		var cells = new List<(byte[], byte[])>();
		for(int by = 0; by < h16; by += 16)
			for(int bx = 0; bx < w16; bx += 16) {
				var blk = new Counter<int>();
				for(int y = by; y < by + 16; y++) for(int x = bx; x < bx + 16; x++) blk.Add(col[y, x]);
				int sp = BestPalette(blk, subs);
				foreach(int cx in new[] { bx, bx + 8 }) {
					var rows = new int[16, 8];
					bool any = false;
					for(int y = 0; y < 16; y++)
						for(int x = 0; x < 8; x++) {
							int c = col[by + y, cx + x];
							rows[y, x] = ArgMin(4, k => D(c, subs[sp][k]));
							any |= rows[y, x] != 0;
						}
					if(any) cells.Add((new[] { (byte)cx, (byte)by, (byte)sp, (byte)(cells.Count & 0xFF) }, Gfx.EncodeCell(rows)));
				}
			}
		return cells;
	}

	sealed class Grid
	{
		public int[,] Col;  // None = transparent
		public List<Counter<int>> Blocks = new();
		public List<(int x, int y)> Places = new();
	}

	static Grid SpriteGrid(Picture src, string name)
	{
		if(src.W > 256 || src.H > 256) throw new InvalidDataException(name + ": sprite pictures must fit in 256x256");
		int w8 = (src.W + 7) / 8 * 8, h16 = (src.H + 15) / 16 * 16;
		bool hasAlpha = false;
		for(int i = 3; i < src.Rgba.Length; i += 4) if(src.Rgba[i] < 255) { hasAlpha = true; break; }
		var key = src.Get(0, 0);
		var g = new Grid { Col = new int[256, 256] };
		for(int y = 0; y < 256; y++)
			for(int x = 0; x < 256; x++) {
				int c = None;
				if(x < src.W && y < src.H) {
					var p = src.Get(x, y);
					bool clear = hasAlpha ? p.a < 128 : (p.r == key.r && p.g == key.g && p.b == key.b);
					if(!clear) c = Nes((p.r, p.g, p.b));
				}
				g.Col[y, x] = c;
			}
		for(int cy = 0; cy < h16; cy += 16)
			for(int cx = 0; cx < w8; cx += 8) {
				var b = new Counter<int>();
				for(int y = cy; y < cy + 16; y++) for(int x = cx; x < cx + 8; x++) if(g.Col[y, x] != None) b.Add(g.Col[y, x]);
				if(!b.Empty) { g.Blocks.Add(b); g.Places.Add((cx, cy)); }
			}
		if(g.Places.Count > 64) throw new InvalidDataException($"{name} needs {g.Places.Count} sprites; the machine has 64");
		return g;
	}

	/// <summary>Sprite sub-palette 3 belongs to the mouse pointer (sprite 61): colour 1 its fill, 2
	/// its shading, 3 its outline. Every picture loads its own palette, so each must carry these;
	/// these are the ones disk 004's first page uses.</summary>
	public static readonly byte[] PointerColours = { 0x0F, 0x37, 0x27, 0x0F };

	/// <summary>One sprite palette for a set of pictures: the machine has a single sprite palette,
	/// and every img, animation and hover effect loads its own over it. The pictures get
	/// sub-palettes 0-2; 3 keeps the pointer's colours.</summary>
	public static byte[] SpritePalette(IList<(Picture pic, string name)> pics)
	{
		var blocks = pics.SelectMany(p => SpriteGrid(p.pic, p.name).Blocks).ToList();
		var pals = ChoosePalettes(blocks, None, nPal: 3);
		var o = new List<byte>();
		foreach(var p in pals) { o.Add(0x0F); o.AddRange(p.Select(c => (byte)(c == None ? 0x0F : c))); }
		o.AddRange(PointerColours);
		return o.ToArray();
	}

	/// <summary>Pictures in one sprite palette (given, or chosen from them). Several pictures are an
	/// animation's frames: they get the same cells with the same sprite numbers, so a frame with
	/// fewer non-empty cells still covers the sprites of the one before it.</summary>
	public static (byte[] pal16, List<List<(byte[] hdr, byte[] cell)>> cells) SpriteImages(IList<(Picture pic, string name)> pics, int firstSprite = 0, IList<byte> palette = null)
	{
		var grids = pics.Select(p => SpriteGrid(p.pic, p.name)).ToList();
		var pal16 = palette != null ? palette.ToArray() : SpritePalette(pics);
		//sub-palette 3 is the pointer's (see PointerColours)
		var pals = Enumerable.Range(0, 3).Select(i => new List<int> { pal16[i * 4 + 1], pal16[i * 4 + 2], pal16[i * 4 + 3] }).ToList();
		if(grids.Count > 1) {
			var union = grids.SelectMany(g => g.Places).Distinct().OrderBy(p => p.y).ThenBy(p => p.x).ToList();
			if(union.Count > 64) throw new InvalidDataException($"the frames together need {union.Count} sprites; the machine has 64");
			foreach(var g in grids) {
				var bl = new Dictionary<(int, int), Counter<int>>();
				for(int i = 0; i < g.Places.Count; i++) bl[g.Places[i]] = g.Blocks[i];
				g.Blocks = union.Select(p => {
					if(bl.TryGetValue(p, out var b)) return b;
					var filler = new Counter<int>();
					filler.Add(pals[0][0]);
					return filler;
				}).ToList();
				g.Places = union;
			}
		}
		var outp = new List<List<(byte[], byte[])>>();
		foreach(var g in grids) {
			var cells = new List<(byte[], byte[])>();
			for(int n = 0; n < g.Places.Count; n++) {
				var (cx, cy) = g.Places[n];
				int sp = BestPalette(g.Blocks[n], pals);
				var rows = new int[16, 8];
				for(int y = 0; y < 16; y++)
					for(int x = 0; x < 8; x++) {
						int c = g.Col[cy + y, cx + x];
						rows[y, x] = c == None ? 0 : 1 + ArgMin(3, k => D(c, pals[sp][k]));
					}
				cells.Add((new[] { (byte)cx, (byte)cy, (byte)sp, (byte)((firstSprite + n) & 0x3F) }, Gfx.EncodeCell(rows)));
			}
			outp.Add(cells);
		}
		return (pal16, outp);
	}
}
