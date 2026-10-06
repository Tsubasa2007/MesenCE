using System.Diagnostics;
using System.IO.Compression;
using System.Text.Json.Nodes;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using EasyMake.Core;

namespace EasyMake.Ui;

/// <summary>The open project: its files, and the objects and pictures built from them for the
/// previews (cached until a file changes)</summary>
public sealed class ProjectState
{
	public readonly string Dir;
	public JsonNode Pj;
	readonly Dictionary<string, (DateTime stamp, Dictionary<string, Obj> objs)> cache = new();
	readonly Dictionary<string, ((DateTime, DateTime) stamp, Picture pic)> bgCache = new();

	public ProjectState(string dir)
	{
		Dir = dir;
		Pj = Project.ReadJson(Path.Combine(dir, "project.json"));
	}

	public void SavePj() => Project.WriteJson(Path.Combine(Dir, "project.json"), Pj);
	public string ScriptPath => Path.Combine(Dir, Pj["script"].GetValue<string>());
	public IEnumerable<string> Packs => Pj["res"].AsObject().Select(x => x.Key);
	public IEnumerable<(string name, string stem)> Backgrounds => Pj["backgrounds"].AsObject().Select(x => (x.Key, x.Value.GetValue<string>()));
	public string ResFolder(string pack) => Path.Combine(Dir, Pj["res"][pack].GetValue<string>());
	public JsonNode ResMeta(string pack) => Project.ReadJson(Path.Combine(ResFolder(pack), "objects.json"));

	public void SaveResMeta(string pack, JsonNode meta)
	{
		Project.WriteJson(Path.Combine(ResFolder(pack), "objects.json"), meta);
		cache.Remove(pack);
	}

	public static List<(string name, string oid)> EntryNames(JsonNode meta)
	{
		if(meta["entries"] is JsonArray ea) return ea.Select(e => (e[0].GetValue<string>(), e[1].GetValue<string>())).ToList();
		return meta["objects"].AsObject().Where(x => x.Value["kind"].GetValue<string>() is not ("image" or "anim")).Select(x => (x.Key, x.Key)).ToList();
	}

	public Dictionary<string, Obj> Objects(string pack)
	{
		if(!Packs.Contains(pack)) return null;
		string folder = ResFolder(pack);
		var stamp = Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories).Select(File.GetLastWriteTimeUtc).DefaultIfEmpty().Max();
		if(cache.TryGetValue(pack, out var hit) && hit.stamp == stamp) return hit.objs;
		var (entries, _, _) = Project.LoadRes(folder, skipAudio: true);
		var d = new Dictionary<string, Obj>();
		foreach(var (n, o) in entries) d[n] = o;
		cache[pack] = (stamp, d);
		return d;
	}

	public void Invalidate(string pack) => cache.Remove(pack);

	/// <summary>A background as the machine will show it (exact data, or the converted picture)</summary>
	public Picture Background(string name)
	{
		var bg = Backgrounds.FirstOrDefault(b => b.name == name.ToUpperInvariant());
		if(bg.stem == null) return null;
		string png = Path.Combine(Dir, bg.stem + ".png"), js = Path.Combine(Dir, bg.stem + ".json");
		if(!File.Exists(png)) return null;
		var stamp = (File.GetLastWriteTimeUtc(png), File.Exists(js) ? File.GetLastWriteTimeUtc(js) : default);
		if(bgCache.TryGetValue(name, out var hit) && hit.stamp == stamp) return hit.pic;
		byte[] idx, pal;
		if(File.Exists(js)) {
			var p = Png.Read(png);
			idx = p.Index ?? throw new InvalidDataException(png + " must stay an indexed PNG");
			pal = Project.ReadJson(js)["palette"].AsArray().Select(x => (byte)x.GetValue<int>()).ToArray();
		} else (idx, pal) = Author.Background(Png.Read(png));
		var pic = IndexedToRgb(idx, pal, 256, 240);
		bgCache[name] = (stamp, pic);
		return pic;
	}

	public static Picture IndexedToRgb(byte[] idx, byte[] pal16, int w, int h)
	{
		var p = new Picture(w, h);
		for(int i = 0; i < w * h; i++) {
			int v = idx[i];
			var c = Gfx.NesRgb(v % 4 == 0 || v >= 16 ? pal16[0] : pal16[v]);
			p.Rgba[i * 4] = c.r; p.Rgba[i * 4 + 1] = c.g; p.Rgba[i * 4 + 2] = c.b; p.Rgba[i * 4 + 3] = 255;
		}
		return p;
	}
}

public sealed partial class MainWindow : Window
{
	public readonly Config Cfg;
	public ProjectState P;
	TreeView tree;
	readonly ContentControl right = new();
	readonly TextBox log = new() { IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.NoWrap, FontFamily = new FontFamily("Consolas, Menlo, monospace") };
	readonly TextBlock status = new() { Margin = new Thickness(6, 2) };
	IPane current;

	public interface IPane
	{
		/// <summary>Called before the pane is replaced: save what it holds</summary>
		void Leave();

		/// <summary>Double-clicking the item in the tree: play it (music, speech, an animation)</summary>
		void Play() { }
	}

	public IPane CurrentPane => current;
	public string LogText => log.Text;
	public void ForceClose() { closingConfirmed = true; Close(); }

	public MainWindow(string[] args)
	{
		Dialogs.Owner = this;
		bool shots = ShotMode.Active(args);
		if(shots) {
			Directory.CreateDirectory(args[2]);
			Config.OverridePath = Path.Combine(args[2], "settings.json");
		}
		Cfg = Config.Load();
		S.Lang = shots && args[0] == "--shots" && args.Length > 3 ? int.Parse(args[3]) : Cfg.Lang;
		Width = 1280; Height = 860;
		BuildUi();
		if(shots) {
			Opened += async (_, _) => {
				if(args[0] == "--actions") await ShotMode.Actions(this, args);
				else await ShotMode.Run(this, args);
			};
		} else {
			string p = args.FirstOrDefault(a => !a.StartsWith("--")) ?? Cfg.Recent;
			if(!string.IsNullOrEmpty(p) && File.Exists(Path.Combine(p, "project.json"))) Dispatcher.UIThread.Post(() => OpenProject(p));
		}
		Closing += async (s, e) => {
			if(closingConfirmed) return;
			e.Cancel = true;
			if(await LeaveCurrent()) { closingConfirmed = true; Close(); }
		};
	}

	bool closingConfirmed;

	//--- frame ---------------------------------------------------------------------------------
	void BuildUi()
	{
		Title = P == null ? S.Get("title") : $"{S.Get("title")} - {P.Dir}";
		MenuItem Item(string key, Action a, string gesture = null)
		{
			var mi = new MenuItem { Header = S.Get(key) };
			mi.Click += (_, _) => a();
			if(gesture != null) mi.InputGesture = KeyGesture.Parse(gesture);
			return mi;
		}
		var menu = new Menu {
			ItemsSource = new[] {
				new MenuItem { Header = S.Get("file"), ItemsSource = new Control[] {
					Item("new", () => _ = CmdNew()), Item("open", () => _ = CmdOpen()), Item("extract", () => _ = CmdExtract()),
					new Separator(), Item("save", SaveAll, "Ctrl+S"), new Separator(), Item("quit", Close) } },
				new MenuItem { Header = S.Get("make"), ItemsSource = new Control[] {
					Item("build", () => _ = CmdBuild()), Item("run", () => _ = CmdRun(), "F5"), new Separator(), Item("verify", () => _ = CmdVerify()) } },
				new MenuItem { Header = S.Get("tools"), ItemsSource = new Control[] { Item("speechtool", () => _ = CmdSpeech()) } },
				new MenuItem { Header = S.Get("settings"), ItemsSource = new Control[] {
					Item("paths", () => _ = PathsDialog.Show(Cfg)),
					new MenuItem { Header = S.Get("lang"), ItemsSource = new Control[] {
						new MenuItem { Header = "中文", Command = new Cmd(() => _ = SetLang(0)) },
						new MenuItem { Header = "English", Command = new Cmd(() => _ = SetLang(1)) } } } } },
			}
		};
		KeyBindings.Clear();
		KeyBindings.Add(new KeyBinding { Gesture = KeyGesture.Parse("Ctrl+S"), Command = new Cmd(SaveAll) });
		KeyBindings.Add(new KeyBinding { Gesture = KeyGesture.Parse("F5"), Command = new Cmd(() => _ = CmdRun()) });

		var bar = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4, Margin = new Thickness(4) };
		foreach(var (key, a) in new (string, Action)[] { ("new", () => _ = CmdNew()), ("open", () => _ = CmdOpen()), ("save", SaveAll),
			("build", () => _ = CmdBuild()), ("run", () => _ = CmdRun()), ("refresh", () => (current as ScriptPane)?.Refresh()) }) {
			var b = new Button { Content = S.Get(key).TrimEnd('…') };
			b.Click += (_, _) => a();
			bar.Children.Add(b);
		}
		tree = new TreeView();
		tree.SelectionChanged += async (_, _) => await OnSelect();
		tree.DoubleTapped += (_, _) => current?.Play();
		var addRes = new Button { Content = S.Get("add_res") };
		addRes.Click += async (_, _) => await CmdAddRes();
		var addBg = new Button { Content = S.Get("add_bg") };
		addBg.Click += async (_, _) => await CmdAddBg();
		var left = new DockPanel();
		var lb = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4, Margin = new Thickness(2), Children = { addRes, addBg } };
		DockPanel.SetDock(lb, Dock.Bottom);
		left.Children.Add(lb);
		left.Children.Add(new ScrollViewer { Content = tree });

		var main = new Grid { ColumnDefinitions = new ColumnDefinitions("240,4,*"), RowDefinitions = new RowDefinitions("*,4,140") };
		main.Children.Add(left);
		var vs = new GridSplitter { ResizeDirection = GridResizeDirection.Columns };
		Grid.SetColumn(vs, 1);
		main.Children.Add(vs);
		Grid.SetColumn(right, 2);
		main.Children.Add(right);
		var hs = new GridSplitter { ResizeDirection = GridResizeDirection.Rows };
		Grid.SetRow(hs, 1); Grid.SetColumnSpan(hs, 3);
		main.Children.Add(hs);
		Grid.SetRow(log, 2); Grid.SetColumnSpan(log, 3);
		main.Children.Add(log);

		var root = new DockPanel();
		DockPanel.SetDock(menu, Dock.Top); root.Children.Add(menu);
		DockPanel.SetDock(bar, Dock.Top); root.Children.Add(bar);
		DockPanel.SetDock(status, Dock.Bottom); root.Children.Add(status);
		root.Children.Add(main);
		Content = root;
		current = null;
		FillTree();
		ShowPlaceholder();
	}

	sealed class Cmd : System.Windows.Input.ICommand
	{
		readonly Action a;
		public Cmd(Action a) { this.a = a; }
		public event EventHandler CanExecuteChanged { add { } remove { } }
		public bool CanExecute(object p) => true;
		public void Execute(object p) => a();
	}

	/// <summary>Settings - Language (public for the self-test)</summary>
	public async Task SetLang(int i)
	{
		if(!await LeaveCurrent()) return;
		string at = (tree.SelectedItem as TreeViewItem)?.Tag as string;
		S.Lang = Cfg.Lang = i;
		Cfg.Save();
		//The frame is built again around the same pane host, log and status line
		foreach(var c in new Control[] { right, log, status }) (c.Parent as Panel)?.Children.Remove(c);
		BuildUi();
		if(at != null) Select(at);
	}

	public void Say(string s)
	{
		if(!Dispatcher.UIThread.CheckAccess()) { Dispatcher.UIThread.Post(() => Say(s)); return; }
		log.Text += s + "\n";
		log.CaretIndex = log.Text.Length;
	}

	public void Status(string s) => status.Text = s;

	async Task<bool> LeaveCurrent()
	{
		if(current is ScriptPane sp && sp.Dirty) {
			var r = await Dialogs.YesNoCancel(S.Get("unsaved"));
			if(r == null) return false;
			if(r == true) sp.Save();
			else sp.Discard();
		}
		current?.Leave();
		return true;
	}

	void Show(Control c)
	{
		current?.Leave();
		current = c as IPane;
		right.Content = c;
	}

	void ShowPlaceholder()
	{
		current = null;
		right.Content = new TextBlock { Text = P == null ? S.Get("noproject") : P.Dir, Margin = new Thickness(20) };
	}

	//--- project and tree --------------------------------------------------------------------------
	public void OpenProject(string dir)
	{
		try {
			P = new ProjectState(dir);
		} catch(Exception e) {
			_ = Dialogs.Message(e.Message);
			return;
		}
		Cfg.Recent = dir;
		Cfg.Save();
		Title = $"{S.Get("title")} - {dir}";
		current = null;
		FillTree();
		Select("script");
	}

	public static string KindKey(JsonNode m)
	{
		string k = m["kind"].GetValue<string>();
		if(m["glyphs"]?.GetValue<bool>() == true || (k == "img" && m["image"]?["type"]?.GetValue<int>() == 1)) return "k_glyph";
		return "k_" + k;
	}

	/// <summary>What the script uses (null without a project or when the script cannot be read)</summary>
	public Usage Usage { get; private set; }

	void ComputeUsage()
	{
		Usage = null;
		if(P == null) return;
		try {
			var metas = new Dictionary<string, JsonNode>();
			foreach(var pack in P.Packs) metas[pack] = P.ResMeta(pack);
			Usage = EasyMake.Core.Usage.Analyze(File.ReadAllText(P.ScriptPath), metas);
			if(Usage.FixedAddressPacks.Count > 0) Say(S.F("fixed_note", string.Join(", ", Usage.FixedAddressPacks)));
			foreach(var (page, pics) in Usage.ActHidesSprites) Say(S.F("act_note", page, string.Join(", ", pics)));
		} catch(Exception e) {
			Say($"{S.Get("error")}: {e.Message}");
		}
	}

	bool Unused(string tag)
	{
		if(Usage == null) return false;
		var f = tag.Split('|');
		return f[0] switch {
			"bg" => !Usage.Backgrounds.Contains(f[1]),
			"obj" => !Usage.Uses(f[1], f[3]),
			_ => false,
		};
	}

	TreeViewItem Item(string header, string tag)
	{
		bool unused = Unused(tag);
		var it = new TreeViewItem { Header = unused ? $"{header}  ({S.Get("unused")})" : header, Tag = tag };
		if(unused) it.Foreground = Brushes.Gray;
		return it;
	}

	public void FillTree()
	{
		ComputeUsage();
		var items = new List<TreeViewItem>();
		if(P != null) {
			items.Add(new TreeViewItem { Header = $"{S.Get("script")}  ({P.Pj["script"]})", Tag = "script" });
			var bgs = new TreeViewItem { Header = S.Get("backgrounds"), Tag = "bgs", IsExpanded = true };
			bgs.ItemsSource = P.Backgrounds.Select(b => Item(b.name, "bg|" + b.name)).ToList();
			items.Add(bgs);
			foreach(var pack in P.Packs) {
				var node = new TreeViewItem { Header = $"{S.Get("pack")}  {pack}", Tag = "res|" + pack, IsExpanded = true };
				try {
					var meta = P.ResMeta(pack);
					node.ItemsSource = ProjectState.EntryNames(meta).Select(e =>
						Item($"{e.name}  [{S.Get(KindKey(meta["objects"][e.oid]))}]", $"obj|{pack}|{e.name}|{e.oid}")).ToList();
				} catch(Exception e) {
					Say($"{pack}: {e.Message}");
				}
				items.Add(node);
			}
		}
		tree.ItemsSource = items;
	}

	/// <summary>After the script changes: recompute what it uses and re-mark the tree in place</summary>
	public void RefreshUsage()
	{
		ComputeUsage();
		foreach(var it in AllItems()) {
			if(it.Tag is not string tag || !(tag.StartsWith("bg|") || tag.StartsWith("obj|"))) continue;
			string h = ((string)it.Header).Replace($"  ({S.Get("unused")})", "");
			bool unused = Unused(tag);
			it.Header = unused ? $"{h}  ({S.Get("unused")})" : h;
			it.Foreground = unused ? Brushes.Gray : null;
		}
	}

	public IEnumerable<TreeViewItem> AllItems()
	{
		IEnumerable<TreeViewItem> Walk(IEnumerable<TreeViewItem> xs)
		{
			foreach(var x in xs) {
				yield return x;
				if(x.ItemsSource is IEnumerable<TreeViewItem> kids) foreach(var k in Walk(kids)) yield return k;
			}
		}
		return tree.ItemsSource is IEnumerable<TreeViewItem> top ? Walk(top) : Enumerable.Empty<TreeViewItem>();
	}

	public void Select(string tag)
	{
		var it = AllItems().FirstOrDefault(i => (string)i.Tag == tag);
		if(it != null) tree.SelectedItem = it;
	}

	async Task OnSelect()
	{
		if(P == null || tree.SelectedItem is not TreeViewItem it) return;
		string key = (string)it.Tag;
		if(current is ScriptPane sp && sp.Dirty) sp.Save();
		try {
			if(key == "script") Show(new ScriptPane(this));
			else if(key.StartsWith("bg|")) Show(new BackgroundPane(this, key[3..]));
			else if(key.StartsWith("obj|")) {
				var parts = key.Split('|', 4);
				Show(ResourcePanes.Create(this, parts[1], parts[2], parts[3]));
			} else ShowPlaceholder();
		} catch(Exception e) {
			Say(e.ToString());
			await Dialogs.Message(e.Message);
		}
	}

	public void SaveAll()
	{
		if(current is ScriptPane sp) sp.Save();
		Status(S.Get("done"));
	}

	//--- commands ---------------------------------------------------------------------------------
	/// <summary>Run work off the UI thread; results and errors come back to it</summary>
	public async Task<T> Job<T>(Func<T> work)
	{
		Status(S.Get("working"));
		try {
			var r = await Task.Run(work);
			Status(S.Get("done"));
			return r;
		} catch(Exception e) {
			Say(e.ToString());
			Status(S.Get("error"));
			await Dialogs.Message(e.Message);
			return default;
		}
	}

	async Task CmdNew()
	{
		var dir = await Dialogs.Folder(S.Get("new_folder"));
		if(dir == null) return;
		if(Directory.EnumerateFileSystemEntries(dir).Any()) { await Dialogs.Message(S.Get("not_empty")); return; }
		if(!await LeaveCurrent()) return;
		Demo.NewProject(dir);
		OpenProject(dir);
	}

	async Task CmdOpen()
	{
		var dir = await Dialogs.Folder(S.Get("pick_folder"));
		if(dir == null || !File.Exists(Path.Combine(dir, "project.json"))) return;
		if(!await LeaveCurrent()) return;
		OpenProject(dir);
	}

	async Task CmdExtract()
	{
		var disk = await Dialogs.OpenFile(S.Get("pick_disk"), Dialogs.Disk, Dialogs.All);
		if(disk == null) return;
		var dir = await Dialogs.Folder(S.Get("new_folder"));
		if(dir == null) return;
		if(Directory.EnumerateFileSystemEntries(dir).Any()) { await Dialogs.Message(S.Get("not_empty")); return; }
		if(!await LeaveCurrent()) return;
		var ok = await Job(() => { foreach(var l in Project.Extract(disk, dir, Say)) Say(l); return true; });
		if(ok) OpenProject(dir);
	}

	async Task<bool> Need(params string[] keys)
	{
		bool Have() => keys.All(k => k switch { "mesen" => File.Exists(Cfg.Mesen), "bios" => File.Exists(Cfg.Bios), _ => File.Exists(Cfg.Template) });
		if(Have()) return true;
		await Dialogs.Message(S.Get("need_paths"));
		await PathsDialog.Show(Cfg);
		return Have();
	}

	async Task CmdBuild()
	{
		if(P == null || !await Need("template")) return;
		SaveAll();
		var outp = await Dialogs.SaveFile(S.Get("build"), Path.GetFileName(P.Dir.TrimEnd('/', '\\')) + ".img", "img", "Disk");
		if(outp != null) await BuildTo(outp);
	}

	/// <summary>The disk last written with Build disk, and for which project: Run uses it</summary>
	(string dir, string path)? lastDisk;

	public async Task<bool> BuildTo(string outp)
	{
		string dir = P.Dir, tpl = Cfg.Template;
		bool ok = await Job(() => { Project.WriteDisk(dir, tpl, outp, true, Say); return true; });
		if(ok) { Say(S.Get("built") + outp); lastDisk = (dir, outp); }
		return ok;
	}

	/// <summary>Started processes are handed here (tests replace it)</summary>
	public Action<string, string> Launch = (exe, arg) => Process.Start(new ProcessStartInfo(exe) {
		ArgumentList = { arg }, WorkingDirectory = Path.GetDirectoryName(exe), UseShellExecute = false });

	async Task CmdRun()
	{
		if(P == null || !await Need("template", "mesen", "bios")) return;
		await RunNow();
	}

	public async Task RunNow()
	{
		SaveAll();
		string dir = P.Dir, tpl = Cfg.Template, bios = Cfg.Bios, mesen = Cfg.Mesen;
		//MesenCE opens a disk image itself, booting a BBK BIOS from its own folder: then the disk
		//saved with Build disk is rebuilt and run
		string saved = lastDisk?.dir == dir ? lastDisk?.path : null;
		bool direct = saved != null && bios.EndsWith(".nes", StringComparison.OrdinalIgnoreCase)
			&& string.Equals(Path.GetFullPath(Path.GetDirectoryName(bios)), Path.GetFullPath(Path.GetDirectoryName(mesen)), StringComparison.OrdinalIgnoreCase);
		var img = await Job(() => {
			if(direct) {
				Project.WriteDisk(dir, tpl, saved, true, Say);
				Launch(mesen, saved);
				return saved;
			}
			string d = Path.Combine(Path.GetTempPath(), "easymake_run");
			Directory.CreateDirectory(d);
			string image = Path.Combine(d, "EasyMake.img"), nes = Path.Combine(d, "EasyMake.nes");
			Project.WriteDisk(dir, tpl, image, true, Say);
			if(bios.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)) {
				using var z = ZipFile.OpenRead(bios);
				var e = z.Entries.First(x => x.Name.EndsWith(".nes", StringComparison.OrdinalIgnoreCase));
				e.ExtractToFile(nes, true);
			} else File.Copy(bios, nes, true);
			//MesenCE mounts <rom name>.img from beside the ROM when it starts
			Launch(mesen, nes);
			return image;
		});
		if(img != null) Say(S.Get("built") + img);
	}

	async Task CmdVerify()
	{
		var disk = await Dialogs.OpenFile(S.Get("pick_disk"), Dialogs.Disk, Dialogs.All);
		if(disk == null) return;
		bool? ok = await Job<bool?>(() => Project.Verify(disk, Say));
		if(ok != null) await Dialogs.Message(ok == true ? S.Get("verify_ok") : S.Get("verify_fail"));
	}

	async Task CmdSpeech()
	{
		var wav = await Dialogs.OpenFile(S.Get("speechtool"), Dialogs.Wav);
		if(wav == null) return;
		var outp = await Dialogs.SaveFile(S.Get("speechtool"), Path.GetFileNameWithoutExtension(wav) + ".lpc", "lpc", "LPC");
		if(outp == null) return;
		var prev = await Job(() => {
			var src = LpcEnc.ReadWav(wav);
			var phrase = LpcEnc.EncodeSamples(src);
			File.WriteAllBytes(outp, phrase);
			string p = Path.ChangeExtension(outp, null) + ".preview.wav";
			Lpc.WriteWav(p, Lpc.Synth(phrase));
			Say(S.F("speech_enc", Path.GetFileName(wav), phrase.Length, src.Count / 10000.0, LpcEnc.Compare(phrase, src.Select(v => v * 32768).ToList())));
			return p;
		});
		if(prev != null) Sound.Play(prev);
	}

	public static string CopyIn(string path, string folder)
	{
		string name = Path.GetFileName(path), dst = Path.Combine(folder, name);
		if(Path.GetFullPath(path) != Path.GetFullPath(dst)) {
			string b = Path.GetFileNameWithoutExtension(name), ext = Path.GetExtension(name);
			for(int i = 1; File.Exists(dst); i++) { name = $"{b}_{i}{ext}"; dst = Path.Combine(folder, name); }
			File.Copy(path, dst);
		}
		return name;
	}

	async Task CmdAddRes()
	{
		if(P == null) return;
		var r = await AddResourceDialog.Show(P.Packs.ToList(), P.Backgrounds.Select(b => b.stem.Split('/')[^1]).ToList());
		if(r != null) await AddResource(r);
	}

	public async Task AddResource(AddResourceDialog r)
	{
		var meta = P.ResMeta(r.Pack);
		var objects = meta["objects"].AsObject();
		var names = ProjectState.EntryNames(meta).Select(e => e.name).Concat(objects.Select(x => x.Key)).ToHashSet();
		if(!System.Text.RegularExpressions.Regex.IsMatch(r.Name, "^[A-Za-z0-9_]{1,12}$") || names.Contains(r.Name)) { await Dialogs.Message(S.Get("bad_name")); return; }
		string folder = P.ResFolder(r.Pack);
		var copied = r.Files.Select(f => CopyIn(f, folder)).ToList();
		JsonObject m;
		switch(r.Kind) {
			case "img": m = new JsonObject { ["kind"] = "img", ["source"] = copied[0], ["first_sprite"] = 0 }; break;
			case "glyph": m = new JsonObject { ["kind"] = "img", ["source"] = copied[0], ["glyphs"] = true, ["palette_from"] = r.Background }; break;
			case "animate": case "act": {
				int o = r.Kind == "act" ? 8 : 0;
				var frames = new JsonArray(copied.Select(c => (JsonNode)new JsonArray(r.Kind == "act" ? 0 : 64, r.Kind == "act" ? 0 : 120, c, o, o, 0, 0)).ToArray());
				m = new JsonObject { ["kind"] = r.Kind, ["first_sprite"] = r.Kind == "act" ? 40 : 20, ["frames"] = frames };
				break;
			}
			case "speak": m = new JsonObject { ["kind"] = "speak", ["phrases"] = new JsonArray(copied.Select(c => (JsonNode)c).ToArray()) }; break;
			default:
				m = new JsonObject { ["kind"] = "music" };
				File.WriteAllText(Path.Combine(folder, r.Name + ".mus"), Demo.Tune.Replace("\n", Environment.NewLine));
				break;
		}
		objects[r.Name] = m;
		if(meta["entries"] is JsonArray ea) ea.Add((JsonNode)new JsonArray(r.Name, r.Name));
		P.SaveResMeta(r.Pack, meta);
		FillTree();
		Select($"obj|{r.Pack}|{r.Name}|{r.Name}");
	}

	async Task CmdAddBg()
	{
		if(P == null) return;
		var pic = await Dialogs.OpenFile(S.Get("add_bg"), Dialogs.Pictures, Dialogs.All);
		if(pic == null) return;
		string guess = new string(Path.GetFileNameWithoutExtension(pic).Where(char.IsAsciiLetterOrDigit).Take(8).ToArray()).ToLowerInvariant();
		var name = await Dialogs.Input(S.Get("bg_name_q"), guess);
		if(name == null) return;
		if(!System.Text.RegularExpressions.Regex.IsMatch(name, "^[A-Za-z0-9_]{1,8}$") || P.Backgrounds.Any(b => b.name == name.ToUpperInvariant() + ".GRA")) {
			await Dialogs.Message(S.Get("bad_name"));
			return;
		}
		Directory.CreateDirectory(Path.Combine(P.Dir, "backgrounds"));
		var p = Png.Read(pic);
		Png.WriteRgba(Path.Combine(P.Dir, "backgrounds", name.ToLowerInvariant() + ".png"), p.W == 256 && p.H == 240 ? p : p.Resize(256, 240));
		P.Pj["backgrounds"][name.ToUpperInvariant() + ".GRA"] = "backgrounds/" + name.ToLowerInvariant();
		P.SavePj();
		FillTree();
		Select("bg|" + name.ToUpperInvariant() + ".GRA");
	}
}
