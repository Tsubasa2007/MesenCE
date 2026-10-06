using System.IO.Compression;

namespace EasyMake.Core;

/// <summary>A picture as the tools use it: always RGBA, plus the palette indexes when the file
/// was an indexed PNG (exact projects keep sub-palette*4 + colour in the indexes, which no
/// RGBA decoder can give back - a palette may repeat a colour).</summary>
public sealed class Picture
{
	public int W, H;
	public byte[] Rgba;     // W*H*4
	public byte[] Index;    // W*H, or null
	public byte[] Palette;  // RGB triples, or null

	public Picture(int w, int h) { W = w; H = h; Rgba = new byte[w * h * 4]; }

	public (byte r, byte g, byte b, byte a) Get(int x, int y)
	{
		int o = (y * W + x) * 4;
		return (Rgba[o], Rgba[o + 1], Rgba[o + 2], Rgba[o + 3]);
	}

	public void Set(int x, int y, (byte r, byte g, byte b) c, byte a = 255)
	{
		int o = (y * W + x) * 4;
		Rgba[o] = c.r; Rgba[o + 1] = c.g; Rgba[o + 2] = c.b; Rgba[o + 3] = a;
	}

	public static Picture Indexed(int w, int h, byte[] index, byte[] paletteRgb)
	{
		var p = new Picture(w, h) { Index = index, Palette = paletteRgb };
		for(int i = 0; i < w * h; i++) {
			int c = index[i] * 3;
			p.Rgba[i * 4] = c + 2 < paletteRgb.Length ? paletteRgb[c] : (byte)0;
			p.Rgba[i * 4 + 1] = c + 2 < paletteRgb.Length ? paletteRgb[c + 1] : (byte)0;
			p.Rgba[i * 4 + 2] = c + 2 < paletteRgb.Length ? paletteRgb[c + 2] : (byte)0;
			p.Rgba[i * 4 + 3] = 255;
		}
		return p;
	}

	/// <summary>Bilinear resize (only used when a picture is not the size it has to be)</summary>
	public Picture Resize(int w, int h)
	{
		var o = new Picture(w, h);
		for(int y = 0; y < h; y++) {
			double sy = Math.Max(0, (y + 0.5) * H / h - 0.5);
			int y0 = Math.Min((int)sy, H - 1), y1 = Math.Min(y0 + 1, H - 1);
			double fy = sy - y0;
			for(int x = 0; x < w; x++) {
				double sx = Math.Max(0, (x + 0.5) * W / w - 0.5);
				int x0 = Math.Min((int)sx, W - 1), x1 = Math.Min(x0 + 1, W - 1);
				double fx = sx - x0;
				for(int c = 0; c < 4; c++) {
					double v = Rgba[(y0 * W + x0) * 4 + c] * (1 - fx) * (1 - fy) + Rgba[(y0 * W + x1) * 4 + c] * fx * (1 - fy)
						+ Rgba[(y1 * W + x0) * 4 + c] * (1 - fx) * fy + Rgba[(y1 * W + x1) * 4 + c] * fx * fy;
					o.Rgba[(y * w + x) * 4 + c] = (byte)Math.Clamp((int)Math.Round(v), 0, 255);
				}
			}
		}
		return o;
	}
}

/// <summary>A small PNG reader and writer (no interlacing). Other files - JPEG, BMP, interlaced
/// PNG - are read through the platform decoder the GUI supplies.</summary>
public static class Png
{
	public static Func<string, Picture> FallbackDecoder;

	public static Picture Read(string path)
	{
		byte[] d = File.ReadAllBytes(path);
		try {
			return Decode(d);
		} catch(NotSupportedException) when(FallbackDecoder != null) {
			return FallbackDecoder(path);
		}
	}

	static uint BE(byte[] d, int o) => (uint)(d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]);

	public static Picture Decode(byte[] d)
	{
		if(d.Length < 8 || d[0] != 0x89 || d[1] != 'P' || d[2] != 'N' || d[3] != 'G') {
			throw new NotSupportedException("not a PNG");
		}
		int pos = 8, w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
		byte[] plte = null, trns = null;
		var idat = new MemoryStream();
		while(pos + 8 <= d.Length) {
			int len = (int)BE(d, pos);
			string type = System.Text.Encoding.ASCII.GetString(d, pos + 4, 4);
			int body = pos + 8;
			switch(type) {
				case "IHDR":
					w = (int)BE(d, body); h = (int)BE(d, body + 4);
					depth = d[body + 8]; ctype = d[body + 9]; interlace = d[body + 12];
					break;
				case "PLTE": plte = d[body..(body + len)]; break;
				case "tRNS": trns = d[body..(body + len)]; break;
				case "IDAT": idat.Write(d, body, len); break;
			}
			if(type == "IEND") break;
			pos = body + len + 4;
		}
		if(interlace != 0 || depth == 16) {
			throw new NotSupportedException("interlaced or 16-bit PNG");
		}
		int channels = ctype switch { 0 => 1, 2 => 3, 3 => 1, 4 => 2, 6 => 4, _ => throw new NotSupportedException("colour type") };
		int bitsPerPixel = channels * depth;
		int stride = (w * bitsPerPixel + 7) / 8;
		int bpp = Math.Max(1, bitsPerPixel / 8);
		idat.Position = 0;
		byte[] raw;
		using(var z = new ZLibStream(idat, CompressionMode.Decompress)) {
			var ms = new MemoryStream();
			z.CopyTo(ms);
			raw = ms.ToArray();
		}
		var rows = new byte[h * stride];
		var prev = new byte[stride];
		int rp = 0;
		for(int y = 0; y < h; y++) {
			int filter = raw[rp++];
			var cur = new byte[stride];
			for(int i = 0; i < stride; i++) {
				int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
				int x = raw[rp + i];
				cur[i] = (byte)(filter switch {
					0 => x,
					1 => x + a,
					2 => x + b,
					3 => x + (a + b) / 2,
					4 => x + Paeth(a, b, c),
					_ => throw new NotSupportedException("filter")
				});
			}
			rp += stride;
			Array.Copy(cur, 0, rows, y * stride, stride);
			prev = cur;
		}
		int Sample(int y, int i)
		{
			//the i-th sample of row y at the image's bit depth
			int bit = i * depth;
			byte v = rows[y * stride + bit / 8];
			if(depth == 8) return v;
			return (v >> (8 - depth - bit % 8)) & ((1 << depth) - 1);
		}
		var p = new Picture(w, h);
		if(ctype == 3) {
			p.Index = new byte[w * h];
			p.Palette = plte ?? new byte[0];
		}
		int scale = depth == 8 ? 1 : 255 / ((1 << depth) - 1);
		for(int y = 0; y < h; y++) {
			for(int x = 0; x < w; x++) {
				int o = (y * w + x) * 4;
				switch(ctype) {
					case 3: {
						int idx = Sample(y, x);
						p.Index[y * w + x] = (byte)idx;
						if(idx * 3 + 2 < p.Palette.Length) {
							p.Rgba[o] = p.Palette[idx * 3]; p.Rgba[o + 1] = p.Palette[idx * 3 + 1]; p.Rgba[o + 2] = p.Palette[idx * 3 + 2];
						}
						p.Rgba[o + 3] = trns != null && idx < trns.Length ? trns[idx] : (byte)255;
						break;
					}
					case 0: {
						int v = Sample(y, x);
						byte g = (byte)(v * scale);
						p.Rgba[o] = p.Rgba[o + 1] = p.Rgba[o + 2] = g;
						p.Rgba[o + 3] = trns != null && trns.Length >= 2 && (trns[0] << 8 | trns[1]) == v ? (byte)0 : (byte)255;
						break;
					}
					case 2: {
						byte r = (byte)Sample(y, x * 3), g = (byte)Sample(y, x * 3 + 1), b = (byte)Sample(y, x * 3 + 2);
						p.Rgba[o] = r; p.Rgba[o + 1] = g; p.Rgba[o + 2] = b;
						p.Rgba[o + 3] = trns != null && trns.Length >= 6 && trns[1] == r && trns[3] == g && trns[5] == b ? (byte)0 : (byte)255;
						break;
					}
					case 4:
						p.Rgba[o] = p.Rgba[o + 1] = p.Rgba[o + 2] = (byte)Sample(y, x * 2);
						p.Rgba[o + 3] = (byte)Sample(y, x * 2 + 1);
						break;
					case 6:
						for(int c = 0; c < 4; c++) p.Rgba[o + c] = (byte)Sample(y, x * 4 + c);
						break;
				}
			}
		}
		return p;
	}

	static int Paeth(int a, int b, int c)
	{
		int p = a + b - c, pa = Math.Abs(p - a), pb = Math.Abs(p - b), pc = Math.Abs(p - c);
		return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
	}

	static readonly uint[] CrcTable = Enumerable.Range(0, 256).Select(n => {
		uint c = (uint)n;
		for(int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
		return c;
	}).ToArray();

	static void Chunk(Stream s, string type, byte[] data)
	{
		var head = new byte[] { (byte)(data.Length >> 24), (byte)(data.Length >> 16), (byte)(data.Length >> 8), (byte)data.Length };
		s.Write(head);
		var t = System.Text.Encoding.ASCII.GetBytes(type);
		s.Write(t);
		s.Write(data);
		uint crc = 0xFFFFFFFF;
		foreach(byte b in t.Concat(data)) crc = CrcTable[(crc ^ b) & 0xFF] ^ (crc >> 8);
		crc ^= 0xFFFFFFFF;
		s.Write(new byte[] { (byte)(crc >> 24), (byte)(crc >> 16), (byte)(crc >> 8), (byte)crc });
	}

	static byte[] Compress(byte[] raw)
	{
		var ms = new MemoryStream();
		using(var z = new ZLibStream(ms, CompressionLevel.Optimal, true)) z.Write(raw);
		return ms.ToArray();
	}

	public static void WriteIndexed(string path, int w, int h, byte[] index, byte[] paletteRgb)
	{
		var raw = new byte[h * (w + 1)];
		for(int y = 0; y < h; y++) Array.Copy(index, y * w, raw, y * (w + 1) + 1, w);
		using var f = File.Create(path);
		f.Write(new byte[] { 0x89, (byte)'P', (byte)'N', (byte)'G', 13, 10, 26, 10 });
		Chunk(f, "IHDR", new byte[] { (byte)(w >> 24), (byte)(w >> 16), (byte)(w >> 8), (byte)w, (byte)(h >> 24), (byte)(h >> 16), (byte)(h >> 8), (byte)h, 8, 3, 0, 0, 0 });
		Chunk(f, "PLTE", paletteRgb);
		Chunk(f, "IDAT", Compress(raw));
		Chunk(f, "IEND", new byte[0]);
	}

	public static void WriteRgba(string path, Picture p)
	{
		var raw = new byte[p.H * (p.W * 4 + 1)];
		for(int y = 0; y < p.H; y++) Array.Copy(p.Rgba, y * p.W * 4, raw, y * (p.W * 4 + 1) + 1, p.W * 4);
		using var f = File.Create(path);
		f.Write(new byte[] { 0x89, (byte)'P', (byte)'N', (byte)'G', 13, 10, 26, 10 });
		int w = p.W, h = p.H;
		Chunk(f, "IHDR", new byte[] { (byte)(w >> 24), (byte)(w >> 16), (byte)(w >> 8), (byte)w, (byte)(h >> 24), (byte)(h >> 16), (byte)(h >> 8), (byte)h, 8, 6, 0, 0, 0 });
		Chunk(f, "IDAT", Compress(raw));
		Chunk(f, "IEND", new byte[0]);
	}
}
