namespace EasyMake.Core;

/// <summary>An object in a .RES. 'Entry' is how far into the object the directory pointer
/// lands (the 16-byte palette in front of an img). An object embedded in another at a fixed
/// place has a Parent and moves with it.</summary>
public abstract class Obj
{
	public string Name;
	public int? Offset;
	public (Obj obj, int rel)? Parent;
	public virtual string Kind => "raw";
	public virtual int Entry => 0;

	public virtual IEnumerable<Obj> Refs() => Array.Empty<Obj>();
	/// <summary>The (relative offset, length) ranges this object's bytes account for</summary>
	public virtual IEnumerable<(int rel, int len)> Covered() { yield return (0, Size()); }
	public abstract int Size();
	public abstract byte[] Encode(int baseAddr);

	public int Addr()
	{
		if(Parent != null) return Parent.Value.obj.Addr() + Parent.Value.rel;
		return 0x8000 + Offset.Value % Res.Bank;
	}

	public static int W16(byte[] d, int o) => d[o] | d[o + 1] << 8;
}

public sealed class RawObj : Obj
{
	public byte[] Data;
	public RawObj(string name, byte[] data, int? offset = null) { Name = name; Data = data; Offset = offset; }
	public override int Size() => Data.Length;
	public override byte[] Encode(int baseAddr) => Data;
}

/// <summary>An image object (XDOS $DBB5): type, count, items. 0 text (count GBK bytes),
/// 1 BG glyphs and 3 sprites (count x 36: x, y, attribute, sprite number, 32-byte 8x16 cell),
/// 2 a GR picture, 4 coloured characters (count x 7).</summary>
public sealed class ImageObj : Obj
{
	public int Type, Count;
	public byte[] Body;
	public override string Kind => "image";

	public ImageObj(string name, int type, int count, byte[] body, int? offset = null)
	{
		Name = name; Type = type; Count = count; Body = body; Offset = offset;
	}

	public static ImageObj Parse(byte[] d, int o, string name)
	{
		int t = d[o], n = d[o + 1], size;
		switch(t) {
			case 0: size = n; break;
			case 1: case 3: size = 36 * n; break;
			case 2: size = Gra.GrSize(d, o + 2); break;
			case 4: size = 7 * n; break;
			default: throw new InvalidDataException($"not an image object (type {t})");
		}
		if(o + 2 + size > d.Length || (o % Res.Bank) + 2 + size > Res.Bank) throw new InvalidDataException("image object runs past its bank");
		return new ImageObj(name, t, n, d[(o + 2)..(o + 2 + size)], o);
	}

	public IEnumerable<(byte[] hdr, byte[] cell)> Cells()
	{
		for(int i = 0; i < Count; i++) yield return (Body[(i * 36)..(i * 36 + 4)], Body[(i * 36 + 4)..(i * 36 + 36)]);
	}

	public override int Size() => 2 + Body.Length;
	public override byte[] Encode(int baseAddr) => new byte[] { (byte)Type, (byte)Count }.Concat(Body).ToArray();
}

/// <summary>An 'img src' resource: 16-byte palette, then the image object the entry points at</summary>
public sealed class ImgRes : Obj
{
	public byte[] Palette;
	public ImageObj Image;
	public override string Kind => "img";
	public override int Entry => 16;

	public ImgRes(string name, byte[] palette, ImageObj image, int? offset = null)
	{
		Name = name; Palette = palette; Image = image; Offset = offset;
		image.Parent = (this, 16);
	}

	public override int Size() => 16 + Image.Size();
	public override byte[] Encode(int baseAddr) => Palette.Concat(Image.Encode(baseAddr + 16)).ToArray();
}

public sealed class Frame
{
	public int X, Y, Xo, Yo, Mode, Kind;
	public ImageObj Image;
}

/// <summary>Animation data: a 14-bit frame count (top bits of the second byte are flags; bit 7 =
/// load the BG palette as well), then 8-byte frames: x, y, image pointer, x origin, y origin,
/// mode, kind.</summary>
public sealed class Anim : Obj
{
	public int Flags;
	public List<Frame> Frames = new();
	public override string Kind => "anim";

	public Anim(string name, int flags, int? offset = null) { Name = name; Flags = flags; Offset = offset; }

	public override IEnumerable<Obj> Refs() => Frames.Select(f => (Obj)f.Image);
	public override int Size() => 2 + 8 * Frames.Count;

	public override byte[] Encode(int baseAddr)
	{
		int n = Frames.Count;
		var o = new List<byte> { (byte)n, (byte)((n >> 8 & 0x3F) | Flags) };
		foreach(var f in Frames) {
			int a = f.Image.Addr();
			o.AddRange(new[] { (byte)f.X, (byte)f.Y, (byte)a, (byte)(a >> 8), (byte)f.Xo, (byte)f.Yo, (byte)f.Mode, (byte)f.Kind });
		}
		return o.ToArray();
	}
}

/// <summary>An 'animate src' resource: 16-byte palette, then the animation data</summary>
public sealed class AnimRes : Obj
{
	public byte[] Palette;
	public Anim Anim;
	public override string Kind => "animate";

	public AnimRes(string name, byte[] palette, Anim anim, int? offset = null)
	{
		Name = name; Palette = palette; Anim = anim; Offset = offset;
		anim.Parent = (this, 16);
	}

	public override IEnumerable<Obj> Refs() => Anim.Refs();
	public override int Size() => 16 + Anim.Size();
	public override byte[] Encode(int baseAddr) => Palette.Concat(Anim.Encode(baseAddr + 16)).ToArray();
}

/// <summary>A menu item's hover effect (XDOS $C6DD), 25 bytes: flags (bit 0 music, bit 1 speech,
/// bit 2 animation), music header (4), speech pointer (2), animation pointer (2), the
/// animation's sprite palette (16). Fields whose flag is clear are kept as raw bytes.</summary>
public sealed class Act : Obj
{
	public byte[] Raw;
	public Anim Anim;
	public override string Kind => "act";

	public Act(string name, byte[] raw, Anim anim = null, int? offset = null) { Name = name; Raw = raw; Anim = anim; Offset = offset; }

	public override IEnumerable<Obj> Refs() => Anim != null ? new Obj[] { Anim } : Array.Empty<Obj>();
	public override int Size() => 25;

	public override byte[] Encode(int baseAddr)
	{
		var o = (byte[])Raw.Clone();
		if(Anim != null) { int a = Anim.Addr(); o[7] = (byte)a; o[8] = (byte)(a >> 8); }
		return o;
	}
}

/// <summary>A 'speak src' resource: a table of phrase pointers, then the phrases. A phrase is a
/// big-endian length, the $D6 sync byte and that many bytes of the BBK chip's LPC-10 stream.
/// A table slot either names a phrase, or keeps a raw address (a stale table entry).</summary>
public sealed class Speech : Obj
{
	public List<(bool raw, int v)> Order;
	public List<byte[]> Phrases;
	public List<int> Places;  // where each phrase sat, relative to the start; null = packed
	public override string Kind => "speak";

	public Speech(string name, List<(bool raw, int v)> order, List<byte[]> phrases, int? offset = null, List<int> places = null)
	{
		Name = name; Order = order; Phrases = phrases; Offset = offset; Places = places;
		if(places != null && !PlacesFit()) Places = null;
	}

	bool PlacesFit()
	{
		int end = 2 * Order.Count;
		foreach(var (rel, p) in Places.Zip(Phrases).OrderBy(x => x.First)) {
			if(rel < end) return false;
			end = rel + p.Length;
		}
		return true;
	}

	public static Speech Parse(byte[] d, int o, string name)
	{
		int bank = o / Res.Bank;
		var ptrs = new List<int>();
		int i = o;
		while(true) {
			int p = W16(d, i);
			if(p < 0x8000 || p >= 0xC000) break;
			if(ptrs.Count > 0 && i >= ptrs.Min(q => Res.FileOffset(bank, q))) break;
			ptrs.Add(p);
			i += 2;
		}
		if(ptrs.Count == 0 || Res.FileOffset(bank, ptrs.Min()) != i) throw new InvalidDataException("no phrase table");
		var good = new List<(int p, byte[] data)>();
		foreach(int p in ptrs.Distinct().OrderBy(x => x)) {
			int a = Res.FileOffset(bank, p);
			int n = d[a] << 8 | d[a + 1];
			if(d[a + 2] == 0xD6 && a + 3 + n <= (bank + 1) * Res.Bank) {
				if(good.Count > 0 && Res.FileOffset(bank, good[^1].p) + good[^1].data.Length > a) continue;
				good.Add((p, d[a..(a + 3 + n)]));
			}
		}
		if(good.Count == 0) throw new InvalidDataException("no $D6 phrases");
		var starts = good.Select(g => g.p).ToList();
		var order = ptrs.Select(p => starts.Contains(p) ? (false, starts.IndexOf(p)) : (true, p)).ToList();
		int baseCpu = 0x8000 + o % Res.Bank;
		return new Speech(name, order, good.Select(g => g.data).ToList(), o, starts.Select(p => p - baseCpu).ToList());
	}

	public override IEnumerable<(int rel, int len)> Covered()
	{
		if(Places == null) { yield return (0, Size()); yield break; }
		yield return (0, 2 * Order.Count);
		for(int i = 0; i < Phrases.Count; i++) yield return (Places[i], Phrases[i].Length);
	}

	public override int Size()
	{
		if(Places != null) return Places.Zip(Phrases).Max(x => x.First + x.Second.Length);
		return 2 * Order.Count + Phrases.Sum(p => p.Length);
	}

	public override byte[] Encode(int baseAddr)
	{
		var o = new byte[Size()];
		Array.Fill(o, (byte)0xFF);
		var rel = Places ?? new List<int>();
		if(Places == null) {
			int a = 2 * Order.Count;
			foreach(var p in Phrases) { rel.Add(a); a += p.Length; }
		}
		for(int k = 0; k < Phrases.Count; k++) Phrases[k].CopyTo(o, rel[k]);
		for(int k = 0; k < Order.Count; k++) {
			int v = Order[k].raw ? Order[k].v : baseAddr + rel[Order[k].v];
			o[2 * k] = (byte)v; o[2 * k + 1] = (byte)(v >> 8);
		}
		return o;
	}
}

/// <summary>A 'backsound' resource for the INSTALL.CMD music driver, held as a listing</summary>
public sealed class MusicObj : Obj
{
	public string Listing;
	readonly int size;
	public override string Kind => "music";

	public MusicObj(string name, string listing, int? offset = null)
	{
		Name = name; Listing = listing; Offset = offset;
		size = Music.Assemble(listing, 0x8000).Length;
	}

	public override int Size() => size;
	public override byte[] Encode(int baseAddr) => Music.Assemble(Listing, baseAddr);
}
