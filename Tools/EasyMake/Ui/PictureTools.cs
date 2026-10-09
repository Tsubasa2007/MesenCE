using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;
using EasyMake.Core;

namespace EasyMake.Ui;

static class Bitmaps
{
	public static WriteableBitmap From(Picture p)
	{
		var bmp = new WriteableBitmap(new PixelSize(p.W, p.H), new Vector(96, 96), PixelFormat.Rgba8888, AlphaFormat.Unpremul);
		using var fb = bmp.Lock();
		for(int y = 0; y < p.H; y++)
			System.Runtime.InteropServices.Marshal.Copy(p.Rgba, y * p.W * 4, fb.Address + y * fb.RowBytes, p.W * 4);
		return bmp;
	}

	/// <summary>A picture shown at a whole scale, without smoothing - the machine's pixels stay square</summary>
	public static Image View(Picture p, double scale)
	{
		var img = new Image { Source = From(p), Width = p.W * scale, Height = p.H * scale, Stretch = Stretch.Fill };
		RenderOptions.SetBitmapInterpolationMode(img, BitmapInterpolationMode.None);
		return img;
	}
}

/// <summary>Every picture on the disk images in a folder, by disk, under the names the files have
/// on the disk</summary>
public sealed class PictureBrowser : Window
{
	readonly Config cfg;
	readonly TextBox folder = new() { Width = 520 };
	readonly CheckBox subfolders = new() { Content = S.Get("pb_subfolders") };
	readonly CheckBox inside = new() { Content = S.Get("pb_inside"), IsChecked = true };
	readonly ComboBox zoom = new() { ItemsSource = new[] { "50%", "100%", "200%" }, SelectedIndex = 1 };
	readonly TextBlock status = new() { Margin = new Thickness(6, 2) };
	readonly StackPanel list = new() { Spacing = 10, Margin = new Thickness(8) };
	readonly List<(string disk, List<(string name, Pictures.Found pic)> pics)> loaded = new();
	CancellationTokenSource cts;

	public PictureBrowser(Config cfg)
	{
		this.cfg = cfg;
		Title = S.Get("pb_title");
		Width = 1180; Height = 820;
		folder.Text = cfg.PictureFolder;
		var browse = new Button { Content = S.Get("browse") };
		browse.Click += async (_, _) => {
			var f = await Dialogs.Folder(S.Get("pb_title"));
			if(f != null) { folder.Text = f; await Load(); }
		};
		var load = new Button { Content = S.Get("pb_load") };
		load.Click += async (_, _) => await Load();
		var export = new Button { Content = S.Get("pb_export") };
		export.Click += async (_, _) => await Export();
		zoom.SelectionChanged += (_, _) => Rebuild();
		var bar = new WrapPanel { Margin = new Thickness(6) };
		foreach(var c in new Control[] { new TextBlock { Text = S.Get("pb_folder"), VerticalAlignment = VerticalAlignment.Center }, folder, browse, load,
			subfolders, inside, new TextBlock { Text = S.Get("pb_zoom"), VerticalAlignment = VerticalAlignment.Center }, zoom, export }) {
			c.Margin = new Thickness(3);
			bar.Children.Add(c);
		}
		var root = new DockPanel();
		DockPanel.SetDock(bar, Dock.Top); root.Children.Add(bar);
		DockPanel.SetDock(status, Dock.Bottom); root.Children.Add(status);
		root.Children.Add(new ScrollViewer { Content = list });
		Content = root;
		Opened += async (_, _) => {
			if(Directory.Exists(folder.Text)) await Load();
			loadedOnce.TrySetResult();
		};
		Closed += (_, _) => cts?.Cancel();
	}

	readonly TaskCompletionSource loadedOnce = new();

	/// <summary>Finishes when the folder shown on opening has been read (for the self-test)</summary>
	public Task FirstLoad => loadedOnce.Task;

	double Scale => zoom.SelectedIndex switch { 0 => 0.5, 2 => 2, _ => 1 };

	async Task Load()
	{
		string dir = folder.Text?.Trim() ?? "";
		if(!Directory.Exists(dir)) { status.Text = S.Get("pb_nofolder"); return; }
		cfg.PictureFolder = dir;
		cfg.Save();
		cts?.Cancel();
		var token = (cts = new CancellationTokenSource()).Token;
		loaded.Clear();
		list.Children.Clear();
		bool sub = subfolders.IsChecked == true, ins = inside.IsChecked == true;
		var disks = PictureScan.Disks(dir, sub);
		int done = 0, total = 0, empty = 0;
		var bad = new List<string>();
		await Task.Run(() => {
			foreach(var disk in disks) {
				if(token.IsCancellationRequested) return;
				var (pics, error) = PictureScan.Read(disk, ins);
				var pictures = pics.Select(p => p.pic.Pic.ToPicture()).ToList();
				Dispatcher.UIThread.Post(() => {
					if(token.IsCancellationRequested) return;
					done++;
					if(error != null) bad.Add(Path.GetFileName(disk));
					else if(pics.Count == 0) empty++;
					else {
						loaded.Add((disk, pics));
						total += pics.Count;
						list.Children.Add(DiskGroup(disk, pics, pictures));
					}
					status.Text = S.F("pb_status", done, disks.Count, total, empty) + (bad.Count > 0 ? "  " + S.F("pb_bad", string.Join(", ", bad)) : "");
				});
			}
		}, token);
		if(disks.Count == 0) status.Text = S.Get("pb_nodisks");
	}

	void Rebuild()
	{
		list.Children.Clear();
		foreach(var (disk, pics) in loaded) list.Children.Add(DiskGroup(disk, pics, pics.Select(p => p.pic.Pic.ToPicture()).ToList()));
	}

	Control DiskGroup(string disk, List<(string name, Pictures.Found pic)> pics, List<Picture> pictures)
	{
		var tiles = new WrapPanel();
		for(int i = 0; i < pics.Count; i++) tiles.Children.Add(Tile(disk, pics[i].name, pics[i].pic, pictures[i]));
		string where = Path.GetRelativePath(cfg.PictureFolder ?? "", disk);
		return new StackPanel {
			Spacing = 4,
			Children = {
				new TextBlock { Text = $"{where}  ({pics.Count})", FontWeight = FontWeight.Bold, FontSize = 15 },
				tiles
			}
		};
	}

	Control Tile(string disk, string name, Pictures.Found f, Picture p)
	{
		var panel = new StackPanel { Margin = new Thickness(4), Spacing = 2 };
		panel.Children.Add(Bitmaps.View(p, Scale));
		panel.Children.Add(new TextBlock { Text = name, FontWeight = FontWeight.SemiBold, MaxWidth = Math.Max(120, p.W * Scale), TextTrimming = TextTrimming.CharacterEllipsis });
		panel.Children.Add(new TextBlock { Text = $"{Pictures.Name(f.Kind)}  {p.W} × {p.H}", Foreground = Brushes.Gray, FontSize = 11 });
		var b = new Button { Content = panel, Padding = new Thickness(2), Background = Brushes.Transparent, VerticalAlignment = VerticalAlignment.Top };
		ToolTip.SetTip(b, $"{Path.GetFileName(disk)}\n{name}");
		b.Click += (_, _) => new PictureViewer(Path.GetFileNameWithoutExtension(disk), name, f).Show(this);
		return b;
	}

	async Task Export()
	{
		if(loaded.Count == 0) return;
		var dir = await Dialogs.Folder(S.Get("pb_export"));
		if(dir == null) return;
		int n = 0;
		await Task.Run(() => {
			foreach(var (disk, pics) in loaded) {
				string sub = Path.Combine(dir, PictureScan.SafeName(Path.GetFileNameWithoutExtension(disk)));
				Directory.CreateDirectory(sub);
				foreach(var (name, f) in pics) {
					PictureScan.SavePng(Path.Combine(sub, PictureScan.SafeName(name) + ".png"), f);
					n++;
				}
			}
		});
		status.Text = S.F("pb_exported", n, dir);
	}
}

/// <summary>One picture, larger, with its palette and a way to save it</summary>
public sealed class PictureViewer : Window
{
	public PictureViewer(string disk, string name, Pictures.Found f)
	{
		Title = $"{disk} - {name}";
		SizeToContent = SizeToContent.WidthAndHeight;
		var p = f.Pic.ToPicture();
		var save = new Button { Content = S.Get("pv_save") };
		save.Click += async (_, _) => {
			var path = await Dialogs.SaveFile(S.Get("pv_save"), PictureScan.SafeName(name) + ".png", "png", "PNG");
			if(path != null) PictureScan.SavePng(path, f);
		};
		var colours = string.Join("  ", Enumerable.Range(0, 4).Select(i => string.Join(" ", f.Pic.Pal16.Skip(i * 4).Take(4).Select(c => c.ToString("X2")))));
		Content = new StackPanel {
			Margin = new Thickness(10), Spacing = 6,
			Children = {
				Bitmaps.View(p, p.W <= 256 && p.H <= 240 ? 2 : 1),
				new TextBlock { Text = $"{name}   {Pictures.Name(f.Kind)}  {p.W} × {p.H}   {f.Length} {S.Get("bytes")}" },
				new TextBlock { Text = S.Get("pv_palette") + colours, FontFamily = new FontFamily("Consolas, Menlo, monospace") },
				save
			}
		};
	}
}

/// <summary>Turn an ordinary picture into one of the machine's picture files, with a preview of
/// how it will look; save it as a file or put it straight onto a disk image</summary>
public sealed class ConvertPicture : Window
{
	static readonly Pictures.Format[] Formats = { Pictures.Format.Grp, Pictures.Format.Pct, Pictures.Format.Gra, Pictures.Format.GraTiles };
	readonly TextBox source = new() { Width = 480, IsReadOnly = true };
	readonly ComboBox format = new() { ItemsSource = Formats.Select(f => S.Get("fmt_" + f)).ToList(), SelectedIndex = 0 };
	readonly CheckBox keepAspect = new() { Content = S.Get("cv_aspect"), IsChecked = true };
	readonly CheckBox dither = new() { Content = S.Get("cv_dither") };
	readonly ContentControl before = new(), after = new();
	readonly TextBlock info = new();
	readonly Button save = new() { Content = S.Get("cv_save"), IsEnabled = false };
	readonly Button toDisk = new() { Content = S.Get("cv_disk"), IsEnabled = false };
	Picture input;
	byte[] result;
	int generation;

	Pictures.Format Format => Formats[Math.Max(0, format.SelectedIndex)];

	/// <summary>Pick a format and dithering and wait for the preview (for the self-test)</summary>
	public async Task Choose(int formatIndex, bool dots)
	{
		format.SelectedIndex = formatIndex;
		dither.IsChecked = dots;
		await Update();
	}

	public ConvertPicture()
	{
		Title = S.Get("cv_title");
		SizeToContent = SizeToContent.WidthAndHeight;
		var browse = new Button { Content = S.Get("browse") };
		browse.Click += async (_, _) => {
			var f = await Dialogs.OpenFile(S.Get("cv_title"), Dialogs.Pictures);
			if(f != null) await Open(f);
		};
		format.SelectionChanged += async (_, _) => await Update();
		keepAspect.IsCheckedChanged += async (_, _) => await Update();
		dither.IsCheckedChanged += async (_, _) => await Update();
		save.Click += async (_, _) => await Save();
		toDisk.Click += async (_, _) => await ToDisk();
		var close = new Button { Content = S.Get("close") };
		close.Click += (_, _) => Close();
		Content = new StackPanel {
			Margin = new Thickness(12), Spacing = 8,
			Children = {
				new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, Children = { new TextBlock { Text = S.Get("source"), VerticalAlignment = VerticalAlignment.Center }, source, browse } },
				new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12, Children = { new TextBlock { Text = S.Get("cv_format"), VerticalAlignment = VerticalAlignment.Center }, format, keepAspect, dither } },
				new TextBlock { Text = S.Get("cv_note"), Foreground = Brushes.Gray, TextWrapping = TextWrapping.Wrap, MaxWidth = 1040 },
				new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12, Children = {
					new StackPanel { Spacing = 2, Children = { new TextBlock { Text = S.Get("cv_before") }, new Border { Width = 512, Height = 480, Background = Brushes.DimGray, Child = before } } },
					new StackPanel { Spacing = 2, Children = { new TextBlock { Text = S.Get("cv_after") }, new Border { Width = 512, Height = 480, Background = Brushes.DimGray, Child = after } } } } },
				info,
				new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, HorizontalAlignment = HorizontalAlignment.Right, Children = { save, toDisk, close } }
			}
		};
	}

	public async Task Open(string path)
	{
		try {
			input = Png.Read(path);
		} catch(Exception e) {
			info.Text = e.Message;
			return;
		}
		source.Text = path;
		double s = Math.Min(1, Math.Min(512.0 / input.W, 480.0 / input.H));
		var img = new Image { Source = Bitmaps.From(input), Width = input.W * s, Height = input.H * s };
		before.Content = img;
		await Update();
	}

	async Task Update()
	{
		if(input == null) return;
		int gen = ++generation;
		var f = Format;
		bool aspect = keepAspect.IsChecked == true, dots = dither.IsChecked == true;
		info.Text = S.Get("working");
		try {
			var (bytes, pic) = await Task.Run(() => {
				var ix = Pictures.Convert(input, f, aspect, dots);
				var data = Pictures.Encode(f, ix);
				//Shown from the bytes written, read back the way the browser reads a disk
				return (data, Pictures.TryDecode(data, 0).Pic.ToPicture());
			});
			if(gen != generation) return;
			result = bytes;
			after.Content = Bitmaps.View(pic, Math.Min(2, Math.Min(512.0 / pic.W, 480.0 / pic.H)));
			info.Text = S.F("cv_info", Pictures.Name(f), pic.W, pic.H, bytes.Length);
			save.IsEnabled = toDisk.IsEnabled = true;
		} catch(Exception e) {
			if(gen != generation) return;
			result = null;
			save.IsEnabled = toDisk.IsEnabled = false;
			info.Text = e.Message;
		}
	}

	string SuggestedName()
	{
		var stem = new string(Path.GetFileNameWithoutExtension(source.Text ?? "").Where(char.IsAsciiLetterOrDigit).Take(8).ToArray()).ToUpperInvariant();
		return (stem.Length > 0 ? stem : "PICTURE") + "." + Pictures.Extension(Format);
	}

	async Task Save()
	{
		if(result == null) return;
		var ext = Pictures.Extension(Format).ToLowerInvariant();
		var path = await Dialogs.SaveFile(S.Get("cv_save"), SuggestedName(), ext, ext.ToUpperInvariant());
		if(path == null) return;
		File.WriteAllBytes(path, result);
		info.Text = S.Get("cv_saved") + path;
	}

	async Task ToDisk()
	{
		if(result == null) return;
		var disk = await Dialogs.OpenFile(S.Get("cv_disk"), Dialogs.Disk);
		if(disk == null) return;
		var name = await Dialogs.Input(S.Get("cv_name_q"), SuggestedName());
		if(string.IsNullOrEmpty(name)) return;
		name = name.ToUpperInvariant();
		var parts = name.Split('.');
		if(parts.Length != 2 || parts[0].Length is 0 or > 8 || parts[1].Length is 0 or > 3 || !name.All(c => char.IsAsciiLetterOrDigit(c) || c is '.' or '_' or '-')) {
			await Dialogs.Message(S.Get("bad_name"));
			return;
		}
		try {
			var fat = new Fat12(File.ReadAllBytes(disk));
			if(fat.Files().Contains(name) && !await Dialogs.Confirm(S.F("cv_replace", name))) return;
			fat.Write(name, result);
			File.WriteAllBytes(disk, fat.D);
			info.Text = S.F("cv_written", name, Path.GetFileName(disk), fat.FreeBytes());
		} catch(Exception e) {
			await Dialogs.Message(e.Message);
		}
	}
}
