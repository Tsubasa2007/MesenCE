using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Platform.Storage;

namespace EasyMake.Ui;

/// <summary>Small modal dialogs and the file pickers</summary>
public static class Dialogs
{
	public static Window Owner;

	static Window Frame(string title, Control body, params (string text, object value)[] buttons)
	{
		var w = new Window {
			Title = title, SizeToContent = SizeToContent.WidthAndHeight, CanResize = false,
			WindowStartupLocation = WindowStartupLocation.CenterOwner, MinWidth = 320, ShowInTaskbar = false
		};
		var row = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Spacing = 6, Margin = new Thickness(0, 12, 0, 0) };
		foreach(var (text, value) in buttons) {
			var b = new Button { Content = text, MinWidth = 72, HorizontalContentAlignment = HorizontalAlignment.Center };
			b.Click += (_, _) => w.Close(value);
			row.Children.Add(b);
		}
		w.Content = new StackPanel { Margin = new Thickness(14), Children = { body, row } };
		return w;
	}

	public static Task Message(string text)
		=> Frame(S.Get("title"), new TextBlock { Text = text, TextWrapping = Avalonia.Media.TextWrapping.Wrap, MaxWidth = 520 }, (S.Get("ok"), true)).ShowDialog<object>(Owner);

	public static async Task<bool> Confirm(string text)
		=> await Frame(S.Get("title"), new TextBlock { Text = text, TextWrapping = Avalonia.Media.TextWrapping.Wrap, MaxWidth = 520 },
			(S.Get("yes"), true), (S.Get("no"), false)).ShowDialog<object>(Owner) is true;

	/// <summary>-> true (yes), false (no), null (cancel)</summary>
	public static async Task<bool?> YesNoCancel(string text)
		=> await Frame(S.Get("title"), new TextBlock { Text = text }, (S.Get("yes"), true), (S.Get("no"), false), (S.Get("cancel"), null)).ShowDialog<object>(Owner) as bool?;

	public static async Task<string> Input(string prompt, string initial)
	{
		var box = new TextBox { Text = initial, Width = 300 };
		var w = Frame(S.Get("title"), new StackPanel { Spacing = 6, Children = { new TextBlock { Text = prompt }, box } }, (S.Get("ok"), true), (S.Get("cancel"), false));
		w.Opened += (_, _) => box.Focus();
		return await w.ShowDialog<object>(Owner) is true ? box.Text?.Trim() : null;
	}

	static List<FilePickerFileType> Types(params (string name, string[] patterns)[] types)
		=> types.Select(t => new FilePickerFileType(t.name) { Patterns = t.patterns }).ToList();

	public static async Task<string[]> OpenFiles(string title, bool multiple, params (string name, string[] patterns)[] types)
	{
		var r = await Owner.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions { Title = title, AllowMultiple = multiple, FileTypeFilter = Types(types) });
		return r.Select(f => f.TryGetLocalPath()).Where(p => p != null).ToArray();
	}

	public static async Task<string> OpenFile(string title, params (string name, string[] patterns)[] types)
		=> (await OpenFiles(title, false, types)).FirstOrDefault();

	public static async Task<string> Folder(string title)
	{
		var r = await Owner.StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = title });
		return r.Count > 0 ? r[0].TryGetLocalPath() : null;
	}

	public static async Task<string> SaveFile(string title, string suggested, string ext, string typeName)
	{
		var r = await Owner.StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions {
			Title = title, SuggestedFileName = suggested, DefaultExtension = ext, FileTypeChoices = Types((typeName, new[] { "*." + ext }))
		});
		return r?.TryGetLocalPath();
	}

	public static readonly (string, string[]) Png = ("PNG", new[] { "*.png" });
	public static readonly (string, string[]) Pictures = ("PNG / JPG / BMP", new[] { "*.png", "*.jpg", "*.jpeg", "*.bmp" });
	public static readonly (string, string[]) Wav = ("WAV", new[] { "*.wav" });
	public static readonly (string, string[]) Lpc = ("LPC", new[] { "*.lpc" });
	public static readonly (string, string[]) Disk = ("Disk", new[] { "*.img", "*.ima" });
	public static readonly (string, string[]) All = ("*", new[] { "*.*" });
}

/// <summary>Add a resource: pack, kind, name, files</summary>
public sealed class AddResourceDialog
{
	public string Pack, Kind, Name, Background;
	public string[] Files = Array.Empty<string>();

	static readonly (string kind, string key)[] Kinds = {
		("img", "k_img"), ("glyph", "k_glyph"), ("animate", "k_animate"), ("act", "k_act"), ("speak", "k_speak"), ("music", "k_music") };

	public static async Task<AddResourceDialog> Show(IList<string> packs, IList<string> backgrounds)
	{
		var r = new AddResourceDialog { Pack = packs[0], Kind = "img", Background = backgrounds.FirstOrDefault() ?? "" };
		var packBox = new ComboBox { ItemsSource = packs, SelectedIndex = 0, MinWidth = 160 };
		var kinds = new StackPanel { Spacing = 2 };
		foreach(var (kind, key) in Kinds) {
			var rb = new RadioButton { Content = S.Get(key), GroupName = "kind", IsChecked = kind == "img", Tag = kind };
			rb.IsCheckedChanged += (_, _) => { if(rb.IsChecked == true) r.Kind = kind; };
			kinds.Children.Add(rb);
		}
		var name = new TextBox { Width = 220 };
		var bg = new ComboBox { ItemsSource = backgrounds, SelectedIndex = backgrounds.Count > 0 ? 0 : -1, MinWidth = 160 };
		var files = new TextBlock { Text = "", TextWrapping = Avalonia.Media.TextWrapping.Wrap, MaxWidth = 360 };
		var browse = new Button { Content = S.Get("browse") };
		async Task Pick()
		{
			r.Files = r.Kind switch {
				"speak" => await Dialogs.OpenFiles(S.Get("browse"), true, Dialogs.Wav, Dialogs.Lpc),
				"animate" or "act" => await Dialogs.OpenFiles(S.Get("browse"), true, Dialogs.Png),
				"music" => Array.Empty<string>(),
				_ => await Dialogs.OpenFiles(S.Get("browse"), false, Dialogs.Png)
			};
			files.Text = string.Join("\n", r.Files.Select(Path.GetFileName));
			if(r.Files.Length > 0 && string.IsNullOrEmpty(name.Text))
				name.Text = new string(Path.GetFileNameWithoutExtension(r.Files[0]).Where(c => char.IsAsciiLetterOrDigit(c) || c == '_').Take(12).ToArray());
		}
		browse.Click += async (_, _) => await Pick();
		var w = new Window {
			Title = S.Get("add_res"), SizeToContent = SizeToContent.WidthAndHeight, CanResize = false,
			WindowStartupLocation = WindowStartupLocation.CenterOwner, ShowInTaskbar = false
		};
		var ok = new Button { Content = S.Get("ok"), MinWidth = 72 };
		var cancel = new Button { Content = S.Get("cancel"), MinWidth = 72 };
		bool accepted = false;
		ok.Click += async (_, _) => {
			if(r.Kind != "music" && r.Files.Length == 0) { await Pick(); if(r.Files.Length == 0) return; }
			accepted = true;
			w.Close();
		};
		cancel.Click += (_, _) => w.Close();
		w.Content = new StackPanel {
			Margin = new Thickness(14), Spacing = 6,
			Children = {
				new TextBlock { Text = S.Get("pack") }, packBox,
				new TextBlock { Text = S.Get("kind") }, kinds,
				new TextBlock { Text = S.Get("res_name_q") }, name,
				new TextBlock { Text = S.Get("palette_from") }, bg,
				browse, files,
				new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Spacing = 6, Children = { ok, cancel } }
			}
		};
		await w.ShowDialog(Dialogs.Owner);
		if(!accepted) return null;
		r.Pack = packBox.SelectedItem as string ?? packs[0];
		r.Name = name.Text?.Trim() ?? "";
		r.Background = bg.SelectedItem as string ?? "";
		return r;
	}
}

/// <summary>The paths to Mesen, the BIOS and the template disk</summary>
public static class PathsDialog
{
	public static async Task Show(Config cfg)
	{
		var boxes = new Dictionary<string, TextBox>();
		var panel = new StackPanel { Margin = new Thickness(14), Spacing = 4 };
		foreach(var (key, value, types) in new[] {
			("mesen", cfg.Mesen, new[] { ("Mesen", new[] { "*.exe", "*" }) }),
			("bios", cfg.Bios, new[] { ("BIOS", new[] { "*.nes", "*.zip" }) }),
			("template", cfg.Template, new[] { Dialogs.Disk }) }) {
			var box = new TextBox { Text = value, Width = 560 };
			boxes[key] = box;
			var b = new Button { Content = S.Get("browse") };
			b.Click += async (_, _) => { var p = await Dialogs.OpenFile(S.Get(key), types); if(p != null) box.Text = p; };
			panel.Children.Add(new TextBlock { Text = S.Get(key), Margin = new Thickness(0, 6, 0, 0) });
			panel.Children.Add(new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, Children = { box, b } });
		}
		var w = new Window { Title = S.Get("paths"), SizeToContent = SizeToContent.WidthAndHeight, CanResize = false, WindowStartupLocation = WindowStartupLocation.CenterOwner, ShowInTaskbar = false };
		var ok = new Button { Content = S.Get("ok"), MinWidth = 72 };
		var cancel = new Button { Content = S.Get("cancel"), MinWidth = 72 };
		bool accepted = false;
		ok.Click += (_, _) => { accepted = true; w.Close(); };
		cancel.Click += (_, _) => w.Close();
		panel.Children.Add(new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Spacing = 6, Margin = new Thickness(0, 12, 0, 0), Children = { ok, cancel } });
		w.Content = panel;
		await w.ShowDialog(Dialogs.Owner);
		if(!accepted) return;
		cfg.Mesen = boxes["mesen"].Text?.Trim() ?? "";
		cfg.Bios = boxes["bios"].Text?.Trim() ?? "";
		cfg.Template = boxes["template"].Text?.Trim() ?? "";
		cfg.Save();
	}
}
