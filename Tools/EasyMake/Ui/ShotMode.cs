using Avalonia;
using Avalonia.Controls;
using Avalonia.Media.Imaging;
using Avalonia.Threading;

namespace EasyMake.Ui;

/// <summary>A self-test: --shots &lt;project&gt; &lt;out folder&gt; [lang] opens the project, selects
/// every item in the tree and renders the window to a PNG each time (every 'step'-th item), then
/// writes a report of any errors and quits. Settings go to a file in the out folder.</summary>
public static class ShotMode
{
	public static bool Active(string[] args) => args.Length >= 3 && args[0] is "--shots" or "--actions" or "--pictures";

	public static async Task Run(MainWindow w, string[] args)
	{
		string proj = args[1], outDir = args[2];
		int step = args.Length > 4 ? int.Parse(args[4]) : 1;
		Directory.CreateDirectory(outDir);
		var errors = new List<string>();
		await Task.Delay(500);
		w.OpenProject(proj);
		var items = w.AllItems().ToList();
		int n = 0;
		foreach(var it in items) {
			try {
				w.Select((string)it.Tag);
				await Settle();
				if(n % step == 0 || n < 4) Grab(w, Path.Combine(outDir, $"{n:D3}.png"));
			} catch(Exception e) {
				errors.Add($"{it.Tag}: {e}");
			}
			n++;
		}
		try {
			w.Select("script");
			await Settle();
			if(w.CurrentPane is ScriptPane sp) { sp.GoTo("name=\"page2\""); await Settle(); Grab(w, Path.Combine(outDir, "page2.png")); }
		} catch(Exception e) {
			errors.Add("page2: " + e);
		}
		//Double-clicking a music or speech item in the tree plays it
		Sound.Muted = new List<string>();
		foreach(var kind in new[] { "music", "speech" }) {
			try {
				var it = items.FirstOrDefault(i => ((string)i.Header).Contains($"[{S.Get(kind == "music" ? "k_music" : "k_speak")}]"));
				if(it == null) continue;
				w.Select((string)it.Tag);
				await Settle();
				int before = Sound.Muted.Count;
				w.CurrentPane?.Play();
				for(int i = 0; i < 100 && Sound.Muted.Count == before; i++) await Task.Delay(100);
				if(Sound.Muted.Count == before || !File.Exists(Sound.Muted[^1]) || new FileInfo(Sound.Muted[^1]).Length < 1000)
					errors.Add($"play {kind}: nothing played");
			} catch(Exception e) {
				errors.Add($"play {kind}: {e}");
			}
		}
		Sound.Muted = null;
		try {
			var anim = items.FirstOrDefault(i => ((string)i.Header).Contains($"[{S.Get("k_animate")}]"));
			if(anim != null) {
				w.Select((string)anim.Tag);
				await Settle();
				w.CurrentPane?.Play();
				await Task.Delay(800);
				Grab(w, Path.Combine(outDir, "anim_play.png"));
			}
		} catch(Exception e) {
			errors.Add("play animation: " + e);
		}
		try {
			w.Select("script");
			await Settle();
			await w.SetLang(1 - S.Lang);
			await Settle();
			Grab(w, Path.Combine(outDir, "lang.png"));
			if(w.Title != S.Get("title") + " - " + proj) errors.Add("lang: title not switched: " + w.Title);
		} catch(Exception e) {
			errors.Add("lang: " + e);
		}
		File.WriteAllText(Path.Combine(outDir, "report.txt"), $"{items.Count} items, {errors.Count} errors\n" + string.Join("\n", errors) + "\nLOG\n" + w.LogText);
		w.ForceClose();
	}

	/// <summary>--actions &lt;project&gt; &lt;out folder&gt; &lt;welcome.wav&gt;: add a resource of every kind
	/// (pictures from the project's own demo files), build a disk, and "run" it with the launch
	/// recorded instead of started</summary>
	public static async Task Actions(MainWindow w, string[] args)
	{
		string proj = args[1], outDir = args[2], wav = args[3];
		var errors = new List<string>();
		await Task.Delay(500);
		w.OpenProject(proj);
		string res = Path.Combine(proj, "res", "sdos");
		w.Cfg.Template = args[4];
		w.Cfg.Bios = args[5];
		w.Cfg.Mesen = args[6];
		var launched = new List<string>();
		w.Launch = (exe, arg) => launched.Add(exe + " " + arg);
		foreach(var (kind, name, files) in new[] {
			("img", "pic2", new[] { Path.Combine(res, "robot.png") }),
			("glyph", "board2", new[] { Path.Combine(res, "sign.png") }),
			("animate", "anim2", new[] { Path.Combine(res, "robot.png"), Path.Combine(res, "robot_up.png") }),
			("act", "act2", new[] { Path.Combine(res, "star_small.png") }),
			("speak", "say2", new[] { wav }),
			("music", "song2", Array.Empty<string>()) }) {
			try {
				await w.AddResource(new AddResourceDialog { Pack = "SDOS.RES", Kind = kind, Name = name, Files = files, Background = "sky" });
				await Settle();
				Grab(w, Path.Combine(outDir, $"add_{kind}.png"));
			} catch(Exception e) {
				errors.Add($"{kind}: {e}");
			}
		}
		string img = Path.Combine(outDir, "actions.img");
		bool built = await w.BuildTo(img);
		await w.RunNow();
		File.WriteAllText(Path.Combine(outDir, "report.txt"),
			$"built {built} {(File.Exists(img) ? new FileInfo(img).Length : 0)}\nlaunched: {string.Join(" | ", launched)}\n{errors.Count} errors\n" + string.Join("\n", errors) + "\nLOG\n" + w.LogText);
		w.ForceClose();
	}

	/// <summary>--pictures &lt;folder of disks&gt; &lt;out folder&gt; &lt;a picture&gt;: the picture browser on
	/// the folder, then the converter in every format, with and without dithering</summary>
	public static async Task Pictures(MainWindow w, string[] args)
	{
		string folder = args[1], outDir = args[2], picture = args[3];
		var errors = new List<string>();
		await Task.Delay(500);
		try {
			w.Cfg.PictureFolder = folder;
			var pb = new PictureBrowser(w.Cfg);
			pb.Show(w);
			await pb.FirstLoad;
			await Settle();
			Grab(pb, Path.Combine(outDir, "browser.png"));
			pb.Close();
		} catch(Exception e) {
			errors.Add("browser: " + e);
		}
		try {
			var cv = new ConvertPicture();
			cv.Show(w);
			await cv.Open(picture);
			for(int f = 0; f < 4; f++)
				foreach(bool dots in new[] { false, true }) {
					await cv.Choose(f, dots);
					await Settle();
					Grab(cv, Path.Combine(outDir, $"convert{f}{(dots ? "d" : "")}.png"));
				}
			cv.Close();
		} catch(Exception e) {
			errors.Add("convert: " + e);
		}
		File.WriteAllText(Path.Combine(outDir, "report.txt"), $"{errors.Count} errors\n" + string.Join("\n", errors));
		w.ForceClose();
	}

	static async Task Settle()
	{
		for(int i = 0; i < 4; i++) {
			await Task.Delay(60);
			Dispatcher.UIThread.RunJobs();
		}
	}

	static void Grab(Window w, string path)
	{
		var size = new PixelSize((int)w.Bounds.Width, (int)w.Bounds.Height);
		using var rtb = new RenderTargetBitmap(size, new Vector(96, 96));
		rtb.Render(w);
		using var f = File.Create(path);
#pragma warning disable CS0618  // the replacement overload takes encoder options this test does not need
		rtb.Save(f);
#pragma warning restore CS0618
	}
}
