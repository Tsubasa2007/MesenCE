namespace EasyMake.Core;

/// <summary>The .RES container: 100 directory entries of 16 bytes (12-byte name, 0, address in
/// the $8000-$BFFF window low byte first, 16KB bank from the file start). Resources never cross
/// a bank, and every pointer inside one is a CPU address in its own bank, so objects that
/// point at each other must share a bank. Free space and unused entries are $FF.</summary>
public static class Res
{
	public const int DirEntries = 100, DirSize = DirEntries * 16, Bank = 0x4000;

	public static List<(string name, int offset)> ParseDirectory(byte[] d)
	{
		var o = new List<(string, int)>();
		for(int i = 0; i < DirEntries; i++) {
			int e = i * 16;
			if(d[e] == 0xFF || d[e] == 0) continue;
			int n = 0;
			while(n < 12 && d[e + n] != 0) n++;
			int addr = d[e + 13] | d[e + 14] << 8;
			o.Add((System.Text.Encoding.Latin1.GetString(d, e, n), d[e + 15] * Bank + addr - 0x8000));
		}
		return o;
	}

	public static int CpuAddr(int offset) => 0x8000 + offset % Bank;
	public static int FileOffset(int bank, int addr) => bank * Bank + addr - 0x8000;

	public static byte[] BuildDirectory(List<(string name, int offset)> entries)
	{
		if(entries.Count > DirEntries) throw new InvalidDataException($"a .RES holds at most {DirEntries} entries");
		var o = new byte[DirSize];
		Array.Fill(o, (byte)0xFF);
		for(int i = 0; i < entries.Count; i++) {
			var nm = System.Text.Encoding.ASCII.GetBytes(entries[i].name);
			if(nm.Length > 12) throw new InvalidDataException("resource name longer than 12 characters: " + entries[i].name);
			Array.Clear(o, i * 16, 16);
			nm.CopyTo(o, i * 16);
			int a = CpuAddr(entries[i].offset);
			o[i * 16 + 13] = (byte)a; o[i * 16 + 14] = (byte)(a >> 8); o[i * 16 + 15] = (byte)(entries[i].offset / Bank);
		}
		return o;
	}

	static Obj Top(Obj r)
	{
		while(r.Parent != null) r = r.Parent.Value.obj;
		return r;
	}

	/// <summary>Group objects that reference each other (they must share a bank), in the order
	/// their first members appear</summary>
	public static List<List<Obj>> Components(List<Obj> objects)
	{
		var link = new Dictionary<Obj, Obj>(ReferenceEqualityComparer.Instance);
		foreach(var o in objects) link[o] = o;
		Obj Find(Obj x)
		{
			while(!ReferenceEquals(link[x], x)) { link[x] = link[link[x]]; x = link[x]; }
			return x;
		}
		foreach(var o in objects)
			foreach(var r0 in o.Refs()) {
				var r = Top(r0);
				if(link.ContainsKey(r)) link[Find(o)] = Find(r);
			}
		var groups = new Dictionary<Obj, List<Obj>>(ReferenceEqualityComparer.Instance);
		var order = new List<Obj>();
		foreach(var o in objects) {
			var root = Find(o);
			if(!groups.TryGetValue(root, out var g)) { groups[root] = g = new List<Obj>(); order.Add(root); }
			g.Add(o);
		}
		return order.Select(r => groups[r]).ToList();
	}

	/// <summary>Give every object a file offset. Objects keep their recorded offset when
	/// 'pinned' and all of them fit there; otherwise whole reference groups are packed
	/// first-fit, largest first, never crossing a bank.</summary>
	public static void Place(List<Obj> objects, bool pinned = true)
	{
		if(pinned && objects.All(o => o.Offset != null)) {
			var spans = objects.Select(o => (s: o.Offset.Value, e: o.Offset.Value + o.Size())).OrderBy(x => x.s).ThenBy(x => x.e).ToList();
			bool ok = true;
			for(int i = 0; i + 1 < spans.Count; i++) ok &= spans[i].e <= spans[i + 1].s;
			ok &= spans.Count == 0 || spans[0].s >= DirSize;
			ok &= spans.All(x => x.s / Bank == (x.e - 1) / Bank);
			if(ok) return;
		}
		foreach(var o in objects) o.Offset = null;
		var groups = Components(objects).OrderBy(g => -g.Sum(o => o.Size())).ToList();
		var free = new Dictionary<int, int>();
		foreach(var g in groups) {
			int need = g.Sum(o => o.Size());
			if(need > Bank) throw new InvalidDataException("objects that reference each other exceed one 16KB bank: " + string.Join(", ", g.Select(o => o.Name)));
			int b = 0, pos;
			while(true) {
				pos = free.TryGetValue(b, out int f) ? f : (b == 0 ? DirSize : b * Bank);
				if(pos + need <= (b + 1) * Bank) break;
				b++;
			}
			foreach(var o in g) { o.Offset = pos; pos += o.Size(); }
			free[b] = pos;
		}
	}

	public static byte[] Assemble(List<(string name, Obj obj)> entries, List<Obj> objects)
	{
		int size = Math.Max(DirSize, objects.Count == 0 ? 0 : objects.Max(o => o.Offset.Value + o.Size()));
		var o = new byte[size];
		Array.Fill(o, (byte)0xFF);
		foreach(var ob in objects) {
			var data = ob.Encode(CpuAddr(ob.Offset.Value));
			if(data.Length != ob.Size()) throw new InvalidOperationException($"{ob.Name} encoded to {data.Length} bytes, not {ob.Size()}");
			data.CopyTo(o, ob.Offset.Value);
		}
		BuildDirectory(entries.Select(e => (e.name, e.obj.Offset.Value + e.obj.Entry)).ToList()).CopyTo(o, 0);
		return o;
	}
}
