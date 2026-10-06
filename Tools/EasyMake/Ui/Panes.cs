using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input.Platform;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using AvaloniaEdit;
using AvaloniaEdit.Document;
using AvaloniaEdit.Rendering;
using EasyMake.Core;

namespace EasyMake.Ui;

/// <summary>Colours tags blue and their quoted values brown</summary>
sealed class TagColorizer : DocumentColorizingTransformer
{
	static readonly Regex Tag = new(@"<[^>\n]*>"), Str = new("\"[^\"\\n]*\"");
	static readonly IBrush TagBrush = new SolidColorBrush(Color.Parse("#1f5fbf")), StrBrush = new SolidColorBrush(Color.Parse("#a0522d"));

	protected override void ColorizeLine(DocumentLine line)
	{
		string text = CurrentContext.Document.GetText(line);
		foreach(Match m in Tag.Matches(text)) {
			ChangeLinePart(line.Offset + m.Index, line.Offset + m.Index + m.Length, e => e.TextRunProperties.SetForegroundBrush(TagBrush));
			foreach(Match s in Str.Matches(m.Value))
				ChangeLinePart(line.Offset + m.Index + s.Index, line.Offset + m.Index + s.Index + s.Length, e => e.TextRunProperties.SetForegroundBrush(StrBrush));
		}
	}
}

static class Ui
{
	public static Button Btn(string key, Action a)
	{
		var b = new Button { Content = S.Get(key) };
		b.Click += (_, _) => a();
		return b;
	}

	public static StackPanel Row(params Control[] c)
	{
		var p = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, Margin = new Thickness(0, 3) };
		foreach(var x in c) p.Children.Add(x);
		return p;
	}

	public static TextBlock Label(string text, bool grey = false)
	{
		var t = new TextBlock { Text = text, VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap };
		if(grey) t.Foreground = Brushes.Gray;  // left unset otherwise, so it inherits the theme's colour
		return t;
	}
}

/// <summary>The script with a live preview of the page under the cursor</summary>
public sealed class ScriptPane : DockPanel, MainWindow.IPane
{
	readonly MainWindow w;
	readonly TextEditor editor;
	readonly PreviewControl preview = new() { Interactive = true };
	readonly TextBlock coord = Ui.Label(S.Get("drag_hint"));
	readonly DispatcherTimer timer;
	(int l, int t, int r, int b)? lastRect;
	public bool Dirty;
	bool loading;

	public ScriptPane(MainWindow w)
	{
		this.w = w;
		editor = new TextEditor {
			FontFamily = new FontFamily("SimSun, NSimSun, Microsoft YaHei, Consolas, monospace"), FontSize = 15,
			ShowLineNumbers = true, WordWrap = false, HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Auto
		};
		editor.TextArea.TextView.LineTransformers.Add(new TagColorizer());
		loading = true;
		editor.Document = new TextDocument(File.ReadAllText(w.P.ScriptPath).Replace("\r\n", "\n"));
		loading = false;
		editor.TextChanged += (_, _) => { if(!loading) { Dirty = true; Schedule(); } };
		editor.TextArea.Caret.PositionChanged += (_, _) => Schedule();
		timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(400) };
		timer.Tick += (_, _) => { timer.Stop(); Refresh(); };
		preview.Hover += (x, y) => coord.Text = $"{S.Get("coords")}: {S.Get("pixel")} ({x}, {y})   {S.Get("cell")} ({x / 8}, {y / 16})";
		preview.Dragged += (l, t, r, b) => {
			lastRect = (l, t, r, b);
			string s = $"<menuitem left={l},top={t},right={r},bottom={b},href=\"\">";
			_ = TopLevel.GetTopLevel(this)?.Clipboard?.SetTextAsync(s);
			w.Status(S.Get("copied") + s);
		};
		var side = new StackPanel { Margin = new Thickness(6), Spacing = 4, Children = { preview, coord, Ui.Row(Ui.Btn("insert_item", InsertItem), Ui.Btn("refresh", Refresh)) } };
		DockPanel.SetDock(side, Dock.Right);
		Children.Add(side);
		Children.Add(editor);
		Dispatcher.UIThread.Post(Refresh);
	}

	void Schedule() { timer.Stop(); timer.Start(); }

	public void Refresh()
	{
		try {
			int line = editor.TextArea.Caret.Line - 1;
			preview.Show(PageLayout.Page(editor.Text, line, w.P.Background, w.P.Objects));
		} catch(Exception e) {
			w.Status($"{S.Get("error")}: {e.Message}");
		}
	}

	void InsertItem()
	{
		var (l, t, r, b) = lastRect ?? (176, 192, 232, 208);
		var at = editor.Document.GetLineByNumber(editor.TextArea.Caret.Line).Offset;
		editor.Document.Insert(at, $"<menuitem left={l},top={t},right={r},bottom={b},href=\"\">\n");
	}

	public void Save()
	{
		if(!Dirty) return;
		File.WriteAllText(w.P.ScriptPath, editor.Text.Replace("\r\n", "\n").Replace("\n", "\r\n"), new System.Text.UTF8Encoding(false));
		Dirty = false;
		w.RefreshUsage();
	}

	public void Discard() => Dirty = false;
	public void Leave() { timer.Stop(); Save(); }

	//for the screenshot test: put the cursor on a line
	public void GoTo(string text)
	{
		int at = editor.Text.IndexOf(text, StringComparison.Ordinal);
		if(at >= 0) { editor.TextArea.Caret.Offset = at; Refresh(); }
	}
}

public sealed class BackgroundPane : StackPanel, MainWindow.IPane
{
	public BackgroundPane(MainWindow w, string name)
	{
		Margin = new Thickness(8);
		Spacing = 6;
		var stem = w.P.Backgrounds.First(b => b.name == name).stem;
		bool exact = File.Exists(Path.Combine(w.P.Dir, stem + ".json"));
		Children.Add(Ui.Label($"{name}  →  {stem}.png" + (exact ? "   (" + S.Get("exact_note") + ")" : "")));
		var pic = w.P.Background(name);
		var pv = new PreviewControl { HorizontalAlignment = HorizontalAlignment.Left };
		if(pic != null) { var s = new Scene(); s.Back = pic; pv.Show(s); }
		Children.Add(pv);
		Children.Add(Ui.Row(
			Ui.Btn("replace_pic", async () => {
				var p = await Dialogs.OpenFile(S.Get("replace_pic"), Dialogs.Pictures, Dialogs.All);
				if(p == null) return;
				var im = Png.Read(p);
				Png.WriteRgba(Path.Combine(w.P.Dir, stem + ".png"), im.W == 256 && im.H == 240 ? im : im.Resize(256, 240));
				string js = Path.Combine(w.P.Dir, stem + ".json");
				if(File.Exists(js)) File.Move(js, js + ".old", true);  // a new picture is converted, not taken as exact data
				w.Select("bgs");
				w.Select("bg|" + name);
			}),
			Ui.Btn("del_bg", async () => {
				if(!await Dialogs.Confirm(S.F("confirm_del", name))) return;
				w.P.Pj["backgrounds"].AsObject().Remove(name);
				w.P.SavePj();
				w.FillTree();
			}),
			Ui.Btn("open_folder", () => Sound.OpenFolder(Path.GetDirectoryName(Path.Combine(w.P.Dir, stem))))));
	}

	public void Leave() { }
}

/// <summary>The editors for each kind of resource</summary>
public static class ResourcePanes
{
	public static Control Create(MainWindow w, string pack, string ename, string oid)
	{
		var meta = w.P.ResMeta(pack);
		var m = meta["objects"][oid].AsObject();
		string folder = w.P.ResFolder(pack);
		void Save()
		{
			meta["objects"][oid] = m.DeepClone();
			w.P.SaveResMeta(pack, meta);
			meta = w.P.ResMeta(pack);
		}
		var root = new DockPanel { Margin = new Thickness(8) };
		var head = new DockPanel();
		bool unused = w.Usage != null && !w.Usage.Uses(pack, oid);
		var title = new TextBlock { Text = $"{S.Get("name")}: {ename}     {S.Get("kind")}: {S.Get(MainWindow.KindKey(m))}" + (unused ? $"     ({S.Get("unused")})" : ""), FontWeight = FontWeight.Bold, FontSize = 14, VerticalAlignment = VerticalAlignment.Center };
		var buttons = Ui.Row(Ui.Btn("open_folder", () => Sound.OpenFolder(folder)), Ui.Btn("del_res", async () => {
			if(!await Dialogs.Confirm(S.F("confirm_del", ename))) return;
			var mt = w.P.ResMeta(pack);
			if(mt["entries"] is JsonArray ea) {
				foreach(var e in ea.Where(e => e[0].GetValue<string>() == ename).ToList()) ea.Remove(e);
			} else mt["objects"].AsObject().Remove(oid);
			w.P.SaveResMeta(pack, mt);
			w.FillTree();
		}));
		DockPanel.SetDock(buttons, Dock.Right);
		head.Children.Add(buttons);
		head.Children.Add(title);
		DockPanel.SetDock(head, Dock.Top);
		root.Children.Add(head);
		string k = m["kind"].GetValue<string>();
		Control body;
		Action play = null;
		if(k == "img" && m.ContainsKey("source")) body = Picture(w, pack, ename, m, folder, Save);
		else if((k == "animate" || k == "act") && m.ContainsKey("frames")) body = Frames(w, pack, ename, m, folder, Save, a => play = a);
		else if(k == "speak") body = Speech(w, m, folder, Save, a => play = a, w.Usage?.Spoken(pack, ename));
		else if(k == "music") body = MusicEditor(w, pack, oid, folder, a => play = a);
		else {
			var pv = new ContentControl { Content = ObjPreview(w, pack, ename) };
			body = new StackPanel { Children = { Ui.Label(S.Get("exact_note"), true), pv } };
			//exact animation data from a disk: step through its frames in the preview
			var o = w.P.Objects(pack)?.GetValueOrDefault(ename);
			int n = o is AnimRes ar ? ar.Anim?.Frames.Count ?? 0 : o is Act act ? act.Anim?.Frames.Count ?? 0 : o is Anim an ? an.Frames.Count : 0;
			if(n > 1) play = () => Animator.Play(n, i => pv.Content = ObjPreview(w, pack, ename, i));
		}
		root.Children.Add(new ScrollViewer { Content = body, Margin = new Thickness(0, 6, 0, 0) });
		return new Holder(root, () => play?.Invoke());
	}

	sealed class Holder : ContentControl, MainWindow.IPane
	{
		readonly Action play;
		public Holder(Control c, Action play = null) { Content = c; this.play = play; }
		public void Leave() { Animator.Stop(); Sound.Stop(); }
		public void Play() => play?.Invoke();
	}

	static class Animator
	{
		static DispatcherTimer t;
		public static void Stop() { t?.Stop(); t = null; }
		public static void Play(int frames, Action<int> show)
		{
			Stop();
			int i = 0;
			t = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(250) };
			t.Tick += (_, _) => { if(i >= frames) { Stop(); return; } show(i++); };
			t.Start();
			show(i++);
		}
	}

	static Control ObjPreview(MainWindow w, string pack, string ename, int frame = 0, int scale = 2)
	{
		try {
			var pv = new PreviewControl { Scale = scale, HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(0, 6) };
			pv.Show(PageLayout.Resource(w.P.Objects(pack)?.GetValueOrDefault(ename), frame));
			return pv;
		} catch(Exception e) {
			return new TextBlock { Text = $"{S.Get("error")}: {e.Message}", Foreground = Brushes.Red, TextWrapping = TextWrapping.Wrap };
		}
	}

	static Control Picture(MainWindow w, string pack, string ename, JsonObject m, string folder, Action save)
	{
		var panel = new StackPanel { Spacing = 4 };
		var holder = new ContentControl();
		void Redraw() => holder.Content = ObjPreview(w, pack, ename);
		var src = Ui.Label(m["source"].GetValue<string>());
		panel.Children.Add(Ui.Row(Ui.Label(S.Get("source")), src, Ui.Btn("replace_pic", async () => {
			var p = await Dialogs.OpenFile(S.Get("replace_pic"), Dialogs.Png);
			if(p == null) return;
			m["source"] = MainWindow.CopyIn(p, folder);
			src.Text = m["source"].GetValue<string>();
			save(); Redraw();
		})));
		if(m["glyphs"]?.GetValue<bool>() == true) {
			var stems = w.P.Backgrounds.Select(b => b.stem.Split('/')[^1]).ToList();
			var cb = new ComboBox { ItemsSource = stems, SelectedItem = m["palette_from"]?.GetValue<string>(), MinWidth = 140 };
			cb.SelectionChanged += (_, _) => { if(cb.SelectedItem is string s) { m["palette_from"] = s; save(); Redraw(); } };
			panel.Children.Add(Ui.Row(Ui.Label(S.Get("palette_from")), cb));
			panel.Children.Add(Ui.Label(S.Get("glyph_hint"), true));
		} else {
			var fs = new NumericUpDown { Minimum = 0, Maximum = 60, Value = m["first_sprite"]?.GetValue<int>() ?? 0, Increment = 1, FormatString = "0", Width = 120 };
			fs.ValueChanged += (_, _) => { m["first_sprite"] = (int)(fs.Value ?? 0); save(); Redraw(); };
			var own = new CheckBox { Content = S.Get("own_palette"), IsChecked = m["own_palette"]?.GetValue<bool>() == true };
			own.IsCheckedChanged += (_, _) => { if(own.IsChecked == true) m["own_palette"] = true; else m.Remove("own_palette"); save(); Redraw(); };
			panel.Children.Add(Ui.Row(Ui.Label(S.Get("first_sprite")), fs, own));
		}
		panel.Children.Add(holder);
		Redraw();
		return panel;
	}

	static Control Frames(MainWindow w, string pack, string ename, JsonObject m, string folder, Action save, Action<Action> setPlay)
	{
		bool act = m["kind"].GetValue<string>() == "act";
		var panel = new StackPanel { Spacing = 4 };
		panel.Children.Add(Ui.Label(act ? S.Get("act_hint") : S.Get("time_hint"), true));
		var frames = m["frames"].AsArray();
		if(m.ContainsKey("first_sprite") || frames.Any(f => f[2].ToString().EndsWith(".png", StringComparison.OrdinalIgnoreCase))) {
			var fs = new NumericUpDown { Minimum = 0, Maximum = 60, Value = m["first_sprite"]?.GetValue<int>() ?? (act ? 40 : 0), Increment = 1, FormatString = "0", Width = 120 };
			fs.ValueChanged += (_, _) => { m["first_sprite"] = (int)(fs.Value ?? 0); save(); };
			panel.Children.Add(Ui.Row(Ui.Label(S.Get("first_sprite")), fs));
		}
		var list = new ListBox { Height = 180, FontFamily = new FontFamily("Consolas, monospace") };
		var holder = new ContentControl();
		void Show(int i) => holder.Content = ObjPreview(w, pack, ename, i, 1);
		void Reload(int sel = -1)
		{
			list.ItemsSource = frames.Select((f, i) => $"{i,3}  x={f[0],-4} y={f[1],-4} {f[2],-24} {S.Get("x_origin")}={f[3]} {S.Get("y_origin")}={f[4]}").ToList();
			if(sel >= 0) list.SelectedIndex = sel;
		}
		var boxes = Enumerable.Range(0, 5).Select(_ => new TextBox { Width = 60 }).ToArray();
		var images = frames.Select(f => f[2].ToString()).Concat(Directory.GetFiles(folder, "*.png").Select(Path.GetFileName)).Distinct().OrderBy(s => s, StringComparer.Ordinal).ToList();
		var imageBox = new ComboBox { ItemsSource = images, Width = 220 };
		list.SelectionChanged += (_, _) => {
			int i = list.SelectedIndex;
			if(i < 0) return;
			var f = frames[i];
			boxes[0].Text = f[0].ToString(); boxes[1].Text = f[1].ToString(); boxes[3].Text = f[3].ToString(); boxes[4].Text = f[4].ToString();
			imageBox.SelectedItem = f[2].ToString();
			Show(i);
		};
		void Update()
		{
			int i = list.SelectedIndex;
			if(i < 0) return;
			if(!int.TryParse(boxes[0].Text, out int x) || !int.TryParse(boxes[1].Text, out int y) || !int.TryParse(boxes[3].Text, out int xo) || !int.TryParse(boxes[4].Text, out int yo)) return;
			var f = frames[i].AsArray();
			f[0] = x; f[1] = y; f[3] = xo; f[4] = yo;
			if(imageBox.SelectedItem is string img) f[2] = img;
			save(); Reload(i);
		}
		async void Add()
		{
			var p = await Dialogs.OpenFile(S.Get("add_frame"), Dialogs.Png);
			if(p == null) return;
			string name = MainWindow.CopyIn(p, folder);
			var last = frames.Count > 0 ? frames[^1] : null;
			int o = act ? 8 : 0;
			frames.Add((JsonNode)new JsonArray(last?[0]?.GetValue<int>() ?? 0, last?[1]?.GetValue<int>() ?? 0, name, last?[3]?.GetValue<int>() ?? o, last?[4]?.GetValue<int>() ?? o, 0, 0));
			save(); Reload(frames.Count - 1);
		}
		void Delete()
		{
			int i = list.SelectedIndex;
			if(i < 0 || frames.Count <= 1) return;
			frames.RemoveAt(i);
			save(); Reload();
		}
		void Move(int d)
		{
			int i = list.SelectedIndex;
			if(i < 0 || i + d < 0 || i + d >= frames.Count) return;
			var a = frames[i].DeepClone();
			frames[i] = frames[i + d].DeepClone();
			frames[i + d] = a;
			save(); Reload(i + d);
		}
		void PlayAll() => Animator.Play(frames.Count, Show);
		list.DoubleTapped += (_, _) => PlayAll();
		setPlay(PlayAll);
		panel.Children.Add(list);
		panel.Children.Add(Ui.Row(Ui.Label("x"), boxes[0], Ui.Label("y"), boxes[1], Ui.Label(S.Get("source")), imageBox,
			Ui.Label(S.Get("x_origin")), boxes[3], Ui.Label(S.Get("y_origin")), boxes[4]));
		panel.Children.Add(Ui.Row(Ui.Btn("update", Update), Ui.Btn("add_frame", Add), Ui.Btn("del_frame", Delete),
			Ui.Btn("up", () => Move(-1)), Ui.Btn("down", () => Move(1)), Ui.Btn("play", PlayAll)));
		panel.Children.Add(holder);
		Reload();
		Show(0);
		return panel;
	}

	/// <param name="spoken">word number -> the page that speaks it, with this pack loaded (null: unknown)</param>
	static Control Speech(MainWindow w, JsonObject m, string folder, Action save, Action<Action> setPlay, SortedDictionary<int, string> spoken)
	{
		var panel = new StackPanel { Spacing = 4 };
		var phrases = m["phrases"].AsArray();
		var list = new ListBox { Height = 220, FontFamily = new FontFamily("Consolas, monospace") };
		var stale = new TextBlock { Foreground = Brushes.Gray, TextWrapping = TextWrapping.Wrap };
		//The script's word=n is the speech table's slot n; "order" maps slots to phrases (a slot that
		//points at stale data holds ["addr", n]). Without it the slots are the phrases in order.
		void Reload()
		{
			var slots = m["order"] as JsonArray;
			var words = Enumerable.Range(0, phrases.Count).Select(_ => new List<int>()).ToList();
			var staleSlots = new List<int>();
			if(slots == null) for(int i = 0; i < phrases.Count; i++) words[i].Add(i);
			else for(int t = 0; t < slots.Count; t++) {
				if(slots[t] is JsonValue v && v.TryGetValue(out int pi) && pi >= 0 && pi < phrases.Count) words[pi].Add(t);
				else staleSlots.Add(t);
			}
			string Use(int word) => spoken == null ? "" : spoken.TryGetValue(word, out var page) ? S.F("spoken_on", page) : S.Get("never_spoken");
			//led by word= (what the script writes); the list keeps the storage order, which the selection indexes
			list.ItemsSource = phrases.Select((p, i) => $"{string.Join(", ", words[i].Select(n => $"word={n}")),-8}  {p,-20}  "
				+ (words[i].Count == 1 ? Use(words[i][0]) : string.Join("; ", words[i].Select(n => $"word={n} {Use(n)}")))).ToList();
			stale.Text = string.Join("\n", staleSlots.Select(t => S.F("stale_slot", t)));
			stale.IsVisible = staleSlots.Count > 0;
		}
		panel.Children.Add(Ui.Label(S.Get("phrases")));
		if(m["order"] is JsonArray ord && !ord.Select((x, i) => x is JsonValue v && v.TryGetValue(out int n) && n == i).All(b => b) || m["order"] is JsonArray o2 && o2.Count != phrases.Count)
			panel.Children.Add(Ui.Label(S.Get("exact_note"), true));
		panel.Children.Add(list);
		panel.Children.Add(stale);
		void Changed() { m.Remove("order"); m.Remove("places"); save(); Reload(); }
		panel.Children.Add(Ui.Row(
			Ui.Btn("add_rec", async () => {
				var ps = await Dialogs.OpenFiles(S.Get("add_rec"), true, Dialogs.Wav, Dialogs.Lpc);
				foreach(var p in ps) phrases.Add((JsonNode)MainWindow.CopyIn(p, folder));
				if(ps.Length > 0) Changed();
			}),
			Ui.Btn("delete", () => { if(list.SelectedIndex >= 0) { phrases.RemoveAt(list.SelectedIndex); Changed(); } }),
			Ui.Btn("listen", () => Listen(list.SelectedIndex)),
			Ui.Btn("stop", Sound.Stop)));
		async void Listen(int i)
		{
			if(i < 0 || i >= phrases.Count) return;
			string p = Path.Combine(folder, phrases[i].GetValue<string>());
			double gain = m["gain"]?.GetValue<double>() ?? 1.0;
			var outp = await w.Job(() => {
				byte[] phrase;
				if(p.EndsWith(".wav", StringComparison.OrdinalIgnoreCase)) {
					var src = LpcEnc.ReadWav(p);
					phrase = LpcEnc.EncodeSamples(src, gain);
					w.Say(S.F("speech_enc", Path.GetFileName(p), phrase.Length, src.Count / 10000.0, LpcEnc.Compare(phrase, src.Select(v => v * 32768).ToList())));
				} else phrase = File.ReadAllBytes(p);
				string o = Path.Combine(Path.GetTempPath(), "easymake_listen.wav");
				Lpc.WriteWav(o, Lpc.Synth(phrase));
				return o;
			});
			if(outp != null) Sound.Play(outp);
		}
		list.DoubleTapped += (_, _) => Listen(list.SelectedIndex);
		//from the tree: the selected phrase, or the first
		setPlay(() => Listen(Math.Max(0, list.SelectedIndex)));
		Reload();
		return panel;
	}

	static Control MusicEditor(MainWindow w, string pack, string oid, string folder, Action<Action> setPlay)
	{
		string path = Path.Combine(folder, oid + ".mus");
		var ed = new TextEditor { FontFamily = new FontFamily("Consolas, Menlo, monospace"), FontSize = 14, ShowLineNumbers = true, Height = 520 };
		ed.Document = new TextDocument(File.ReadAllText(path).Replace("\r\n", "\n"));
		var panel = new StackPanel { Spacing = 4 };
		panel.Children.Add(ed);
		panel.Children.Add(Ui.Row(
			Ui.Btn("save_mus", () => {
				File.WriteAllText(path, ed.Text.TrimEnd('\n').Replace("\n", Environment.NewLine) + Environment.NewLine);
				w.P.Invalidate(pack);
				w.Status(S.Get("mus_saved"));
			}),
			Ui.Btn("check", async () => {
				try { w.Status(S.F("music_ok", Music.Assemble(ed.Text, 0x8000).Length)); } catch(Exception e) { await Dialogs.Message(e.Message); }
			}),
			Ui.Btn("listen", Listen),
			Ui.Btn("stop", Sound.Stop)));
		async void Listen()
		{
			string text = ed.Text, o = Path.Combine(Path.GetTempPath(), "easymake_music.wav");
			if(await w.Job(() => { MusicPlay.WriteWav(o, MusicPlay.Render(text)); return true; })) Sound.Play(o);
		}
		setPlay(Listen);
		return panel;
	}
}
