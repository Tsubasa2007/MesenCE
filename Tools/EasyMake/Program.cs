using System.Runtime.InteropServices;
using EasyMake.Core;

namespace EasyMake;

public static class Program
{
	const string Usage = @"EasyMake - authoring tools for the BBK XDOS hypertext player

  EasyMake [<project>]                                     the editor
  EasyMake new <project>                                   start from a small demo title
  EasyMake extract <disk.img | folder> <project>           unpack a title into a project
  EasyMake build <project> <template.img> <out.img> [--repack]
                                     pack a project onto a boot disk (--repack: move every
                                     object to a fresh layout instead of where it was)
  EasyMake verify <disk.img | folder>                      extract + rebuild, compare bytes
  EasyMake speech <in.wav> <out.lpc>                       encode a recording for the speech
                                     chip (and write <out>.preview.wav, decoded back)
  EasyMake pictures <disk.img | folder> <out folder> [--inside] [--all]
                                     save every picture on the disks as PNG (--inside: also
                                     pictures inside programs; --all: subfolders too)
  EasyMake picture <in.png> <out.gra|grp|pct> [--tiles] [--stretch] [--dither]
                                     convert a picture (--tiles: a tile-map GRA; --stretch:
                                     fill the screen instead of keeping the shape; --dither:
                                     dot patterns for in-between colours, for photographs)";

	[DllImport("kernel32.dll")]
	static extern bool AttachConsole(int pid);

	static readonly string[] Commands = { "new", "extract", "build", "verify", "speech", "pictures", "picture", "help", "--help", "-h" };

	[STAThread]
	public static int Main(string[] args)
	{
		if(args.Length > 0 && Commands.Contains(args[0])) {
			if(OperatingSystem.IsWindows()) AttachConsole(-1);
			try {
				return Cli(args);
			} catch(Exception e) {
				Console.Error.WriteLine("error: " + e.Message);
				if(Environment.GetEnvironmentVariable("EASYMAKE_DEBUG") != null) Console.Error.WriteLine(e);
				return 1;
			}
		}
		return Ui.App.Start(args);
	}

	static int Cli(string[] argv)
	{
		bool repack = argv.Contains("--repack");
		var a = argv.Where(x => !x.StartsWith("--")).ToArray();
		switch(a[0]) {
			case "new" when a.Length == 2:
				Demo.NewProject(a[1]);
				Console.WriteLine("made a demo project in " + a[1]);
				return 0;
			case "extract" when a.Length == 3:
				foreach(var line in Project.Extract(a[1], a[2], Console.WriteLine)) Console.WriteLine(line);
				return 0;
			case "build" when a.Length == 4:
				Project.WriteDisk(a[1], a[2], a[3], !repack, Console.WriteLine);
				foreach(var (page, pics) in Core.Usage.ForProject(a[1]).ActHidesSprites)
					Console.WriteLine($"warning: page {page} has a hover effect (act=); when the pointer leaves that menu item XDOS hides sprites 0-58, so {string.Join(", ", pics)} will vanish - use glyph pictures, or put them on another page");
				return 0;
			case "verify" when a.Length == 2:
				return Project.Verify(a[1], Console.WriteLine) ? 0 : 2;
			case "speech" when a.Length == 3: {
				var src = LpcEnc.ReadWav(a[1]);
				var phrase = LpcEnc.EncodeSamples(src);
				File.WriteAllBytes(a[2], phrase);
				string preview = Path.ChangeExtension(a[2], null) + ".preview.wav";
				Lpc.WriteWav(preview, Lpc.Synth(phrase));
				Console.WriteLine($"{phrase.Length} bytes ({src.Count / 10000.0:F1} s of speech, {phrase.Length / Math.Max(0.1, src.Count / 10000.0):F0} bytes/s); " +
					$"level envelope match {LpcEnc.Compare(phrase, src.Select(v => v * 32768).ToList()):F2}; preview in {preview}");
				return 0;
			}
			case "pictures" when a.Length == 3: {
				var disks = Directory.Exists(a[1]) ? PictureScan.Disks(a[1], argv.Contains("--all")) : new List<string> { a[1] };
				int total = 0;
				foreach(var disk in disks) {
					var (pics, error) = PictureScan.Read(disk, argv.Contains("--inside"));
					if(error != null) { Console.WriteLine($"{disk}: {error}"); continue; }
					if(pics.Count == 0) continue;
					string dir = Path.Combine(a[2], Path.GetFileNameWithoutExtension(disk));
					Directory.CreateDirectory(dir);
					foreach(var (name, f) in pics)
						Png.WriteIndexed(Path.Combine(dir, PictureScan.SafeName(name) + ".png"), f.Pic.W, f.Pic.H, f.Pic.Index, Gfx.PaletteRgb(f.Pic.Pal16));
					Console.WriteLine($"{Path.GetFileName(disk)}: {pics.Count} ({string.Join(", ", pics.GroupBy(p => p.pic.Kind).Select(g => $"{g.Count()} {Pictures.Name(g.Key)}"))})");
					total += pics.Count;
				}
				Console.WriteLine($"{total} pictures from {disks.Count} disks");
				return 0;
			}
			case "picture" when a.Length == 3: {
				var format = Path.GetExtension(a[2]).ToUpperInvariant() switch {
					".GRP" => Pictures.Format.Grp, ".PCT" => Pictures.Format.Pct,
					".GRA" => argv.Contains("--tiles") ? Pictures.Format.GraTiles : Pictures.Format.Gra,
					_ => throw new ArgumentException("the output must be a .gra, .grp or .pct file")
				};
				var pic = Pictures.Convert(Png.Read(a[1]), format, !argv.Contains("--stretch"), argv.Contains("--dither"));
				var bytes = Pictures.Encode(format, pic);
				File.WriteAllBytes(a[2], bytes);
				Console.WriteLine($"{Pictures.Name(format)} {pic.W} x {pic.H}, {bytes.Length} bytes");
				return 0;
			}
		}
		Console.WriteLine(Usage);
		return 1;
	}
}
