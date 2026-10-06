namespace EasyMake.Core;

/// <summary>Minimal FAT12 floppy image access: list, read, delete and add root-directory files.
/// Used to swap a boot disk's EasyMake files; the boot sector and the files that stay are not
/// moved.</summary>
public sealed class Fat12
{
	public readonly byte[] D;
	readonly int bps, spc, res, nfats, nroot, total, spf, fatOff, rootOff, dataOff, csize, nclusters;

	public Fat12(byte[] data)
	{
		D = (byte[])data.Clone();
		bps = BitConverter.ToUInt16(D, 11); spc = D[13]; res = BitConverter.ToUInt16(D, 14);
		nfats = D[16]; nroot = BitConverter.ToUInt16(D, 17); total = BitConverter.ToUInt16(D, 19);
		spf = BitConverter.ToUInt16(D, 22);
		if((bps != 512 && bps != 1024) || spc == 0 || D.Length < total * bps)
			throw new InvalidDataException("not a FAT12 floppy image (a compressed .imz must be unpacked first)");
		fatOff = res * bps;
		rootOff = (res + nfats * spf) * bps;
		dataOff = rootOff + nroot * 32;
		csize = spc * bps;
		nclusters = (total * bps - dataOff) / csize + 2;
	}

	int Fat(int c)
	{
		int o = fatOff + c * 3 / 2;
		int v = D[o] | D[o + 1] << 8;
		return (c & 1) != 0 ? v >> 4 : v & 0xFFF;
	}

	void SetFat(int c, int val)
	{
		for(int f = 0; f < nfats; f++) {
			int o = fatOff + f * spf * bps + c * 3 / 2;
			int v = D[o] | D[o + 1] << 8;
			v = (c & 1) != 0 ? (v & 0x000F) | (val << 4) : (v & 0xF000) | val;
			D[o] = (byte)v; D[o + 1] = (byte)(v >> 8);
		}
	}

	IEnumerable<(int off, string name, int cluster, int size, int attr)> Entries()
	{
		for(int i = 0; i < nroot; i++) {
			int o = rootOff + i * 32;
			if(D[o] == 0) yield break;
			if(D[o] == 0xE5 || (D[o + 11] & 0x08) != 0) continue;
			string name = System.Text.Encoding.Latin1.GetString(D, o, 8).TrimEnd();
			string ext = System.Text.Encoding.Latin1.GetString(D, o + 8, 3).TrimEnd();
			yield return (o, (ext.Length > 0 ? name + "." + ext : name).ToUpperInvariant(),
				BitConverter.ToUInt16(D, o + 26), BitConverter.ToInt32(D, o + 28), D[o + 11]);
		}
	}

	public List<string> Files() => Entries().Where(e => (e.attr & 0x10) == 0).Select(e => e.name).ToList();

	List<int> Chain(int c)
	{
		var o = new List<int>();
		while(c >= 2 && c < 0xFF8 && o.Count < nclusters) { o.Add(c); c = Fat(c); }
		return o;
	}

	public byte[] Read(string name)
	{
		foreach(var e in Entries())
			if(e.name == name.ToUpperInvariant()) {
				var ms = new MemoryStream();
				foreach(int c in Chain(e.cluster)) ms.Write(D, dataOff + (c - 2) * csize, csize);
				return ms.ToArray()[..Math.Min(e.size, (int)ms.Length)];
			}
		throw new FileNotFoundException(name);
	}

	public bool Delete(string name)
	{
		foreach(var e in Entries().ToList())
			if(e.name == name.ToUpperInvariant()) {
				foreach(int c in Chain(e.cluster)) SetFat(c, 0);
				D[e.off] = 0xE5;
				return true;
			}
		return false;
	}

	public int FreeBytes() => Enumerable.Range(2, nclusters - 2).Count(c => Fat(c) == 0) * csize;

	public void Write(string name, byte[] data)
	{
		Delete(name);
		var parts = name.ToUpperInvariant().Split('.', 2);
		string b = parts[0], ext = parts.Length > 1 ? parts[1] : "";
		if(b.Length > 8 || ext.Length > 3) throw new InvalidDataException("not an 8.3 name: " + name);
		int need = Math.Max(1, (data.Length + csize - 1) / csize);
		var free = Enumerable.Range(2, nclusters - 2).Where(c => Fat(c) == 0).Take(need).ToList();
		if(free.Count < need) throw new InvalidDataException($"disk full: {name} needs {(need - free.Count) * csize} bytes more");
		for(int i = 0; i < free.Count; i++) {
			SetFat(free[i], i + 1 < free.Count ? free[i + 1] : 0xFFF);
			int o = dataOff + (free[i] - 2) * csize;
			Array.Clear(D, o, csize);
			Array.Copy(data, i * csize, D, o, Math.Min(csize, data.Length - i * csize));
		}
		for(int i = 0; i < nroot; i++) {
			int o = rootOff + i * 32;
			if(D[o] == 0 || D[o] == 0xE5) {
				Array.Clear(D, o, 32);
				System.Text.Encoding.ASCII.GetBytes(b.PadRight(8) + ext.PadRight(3)).CopyTo(D, o);
				D[o + 11] = 0x20;
				D[o + 24] = 0x21; D[o + 25] = 0x2C;
				int fc = data.Length > 0 ? free[0] : 0;
				D[o + 26] = (byte)fc; D[o + 27] = (byte)(fc >> 8);
				BitConverter.GetBytes(data.Length).CopyTo(D, o + 28);
				return;
			}
		}
		throw new InvalidDataException("root directory full");
	}
}
