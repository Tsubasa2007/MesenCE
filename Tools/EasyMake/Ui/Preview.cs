using System.Globalization;
using System.Text.RegularExpressions;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using EasyMake.Core;

namespace EasyMake.Ui;

/// <summary>What a preview shows: a background picture (opaque), XDOS text on top of it, sprites
/// above that (transparent where empty), and outlined menu areas - drawn the way XDOS lays a
/// page out on its 256x240 screen.</summary>
public sealed class Scene
{
	/// <summary>Everything XDOS places shows this far right of its coordinates on the machine's
	/// screen: text and glyph pictures (column c at pixel (c + 1) x 8), sprites, and the mouse
	/// pointer, so menu areas too. Only the .GRA background is not moved.</summary>
	public const int Shift = 8;

	public Picture Back = new(256, 240);
	public Picture Sprites = new(256, 240);
	public readonly List<(int col, int row, string text)> Text = new();
	public readonly List<(int l, int t, int r, int b)> Menus = new();
	public readonly List<(int x0, int y0, int x1, int y1)> Lines = new();

	public static Scene Checker()
	{
		var s = new Scene();
		for(int y = 0; y < 240; y++)
			for(int x = 0; x < 256; x++) {
				byte v = ((x / 16 + y / 16) % 2) != 0 ? (byte)80 : (byte)60;
				s.Back.Set(x, y, (v, v, v));
			}
		return s;
	}

	/// <summary>An image object's 8x16 cells (types 1 and 3): glyphs onto the background at text
	/// cells, sprites onto the sprite layer, both Shift pixels right, as the machine shows them</summary>
	public void Cells(ImageObj img, IList<byte> pal, int left, int top)
	{
		if(img.Type == 0 || img.Type == 4) { TextImage(img, left, top); return; }
		if(img.Type != 1 && img.Type != 3) return;
		foreach(var (hdr, cell) in img.Cells()) {
			var rows = Gfx.DecodeCell(cell, 0);
			int X, Y;
			if(img.Type == 1) { X = (left + (hdr[0] >> 3)) * 8 + Shift; Y = (top + (hdr[1] >> 4)) * 16; }
			else { X = left + hdr[0] + Shift; Y = top + hdr[1]; }
			int sp = hdr[2] & 3;
			var layer = img.Type == 1 ? Back : Sprites;
			for(int r = 0; r < 16; r++)
				for(int c = 0; c < 8; c++) {
					int v = rows[r, c];
					if((v != 0 || img.Type == 1) && X + c >= 0 && X + c < 256 && Y + r >= 0 && Y + r < 240)
						layer.Set(X + c, Y + r, Gfx.NesRgb(v == 0 ? pal[0] : pal[sp * 4 + v]));
				}
		}
	}

	/// <summary>Image types 0 (a string) and 4 (coloured characters), at text cells</summary>
	void TextImage(ImageObj img, int col, int row)
	{
		if(img.Type == 0) Text.Add((col, row, Gbk.Decode(img.Body)));
		else
			for(int i = 0; i < img.Count; i++) {
				var b = img.Body[(i * 7)..(i * 7 + 7)];
				string ch = b[5] == 0 ? ((char)b[6]).ToString() : Gbk.Decode(new[] { b[5], b[6] });
				Text.Add((col + b[0], row + b[1], ch));
			}
	}
}

public sealed class PreviewControl : Control
{
	Scene scene = new();
	WriteableBitmap back, sprites;
	public int Scale = 2;
	public bool Interactive;
	public event Action<int, int> Hover;
	public event Action<int, int, int, int> Dragged;
	Point? dragStart;
	Rect? dragRect;

	static readonly Typeface Font = new(new FontFamily("SimSun, NSimSun, Microsoft YaHei, Noto Sans CJK SC"));

	public PreviewControl()
	{
		RenderOptions.SetBitmapInterpolationMode(this, BitmapInterpolationMode.None);
		ClipToBounds = true;
	}

	protected override Size MeasureOverride(Size available) => new(256 * Scale, 240 * Scale);

	public void Show(Scene s)
	{
		scene = s;
		back = ToBitmap(s.Back, back);
		sprites = ToBitmap(s.Sprites, sprites);
		InvalidateVisual();
	}

	static WriteableBitmap ToBitmap(Picture p, WriteableBitmap reuse)
	{
		var bmp = reuse ?? new WriteableBitmap(new PixelSize(p.W, p.H), new Vector(96, 96), PixelFormat.Rgba8888, AlphaFormat.Unpremul);
		using var fb = bmp.Lock();
		for(int y = 0; y < p.H; y++)
			System.Runtime.InteropServices.Marshal.Copy(p.Rgba, y * p.W * 4, fb.Address + y * fb.RowBytes, p.W * 4);
		return bmp;
	}

	public override void Render(DrawingContext ctx)
	{
		var full = new Rect(0, 0, 256 * Scale, 240 * Scale);
		if(back != null) ctx.DrawImage(back, new Rect(0, 0, 256, 240), full);
		foreach(var (col, row, text) in scene.Text) {
			double x = (col * 8 + Scene.Shift) * Scale;
			foreach(var ch in text) {
				double w = (ch < 128 ? 8 : 16) * Scale;
				ctx.FillRectangle(Brushes.Black, new Rect(x, row * 16 * Scale, w, 16 * Scale));
				var ft = new FormattedText(ch.ToString(), CultureInfo.InvariantCulture, FlowDirection.LeftToRight, Font, 16 * Scale, Brushes.White);
				ctx.DrawText(ft, new Point(x, row * 16 * Scale));
				x += w;
			}
		}
		if(sprites != null) ctx.DrawImage(sprites, new Rect(0, 0, 256, 240), full);
		var dash = new Pen(new SolidColorBrush(Color.FromRgb(255, 64, 255)), 1, new DashStyle(new double[] { 4, 2 }, 0));
		foreach(var (l, t, r, b) in scene.Menus) ctx.DrawRectangle(null, dash, new Rect((l + Scene.Shift) * Scale, t * Scale, (r - l) * Scale, (b - t) * Scale));
		var grey = new Pen(new SolidColorBrush(Color.FromRgb(150, 150, 150)), Scale);
		foreach(var (x0, y0, x1, y1) in scene.Lines) ctx.DrawLine(grey, new Point(x0 * Scale, y0 * Scale), new Point(x1 * Scale, y1 * Scale));
		if(dragRect != null) ctx.DrawRectangle(null, new Pen(Brushes.Yellow, 1), dragRect.Value);
	}

	/// <summary>A point on the control -> XDOS coordinates (the screen pixel less Scene.Shift)</summary>
	(int x, int y) ToScreen(Point p) => (Math.Clamp((int)(p.X / Scale) - Scene.Shift, 0, 255), Math.Clamp((int)(p.Y / Scale), 0, 239));

	protected override void OnPointerMoved(PointerEventArgs e)
	{
		base.OnPointerMoved(e);
		if(!Interactive) return;
		var p = e.GetPosition(this);
		var (x, y) = ToScreen(p);
		Hover?.Invoke(x, y);
		if(dragStart != null) {
			dragRect = new Rect(dragStart.Value, p).Normalize();
			InvalidateVisual();
		}
	}

	protected override void OnPointerPressed(PointerPressedEventArgs e)
	{
		base.OnPointerPressed(e);
		if(Interactive) dragStart = e.GetPosition(this);
	}

	protected override void OnPointerReleased(PointerReleasedEventArgs e)
	{
		base.OnPointerReleased(e);
		if(!Interactive || dragStart == null) return;
		var (x0, y0) = ToScreen(dragStart.Value);
		var (x1, y1) = ToScreen(e.GetPosition(this));
		dragStart = null;
		dragRect = null;
		InvalidateVisual();
		if(Math.Abs(x1 - x0) < 3 || Math.Abs(y1 - y0) < 3) return;
		Dragged?.Invoke(Math.Min(x0, x1), Math.Min(y0, y1), Math.Max(x0, x1), Math.Max(y0, y1));
	}
}

/// <summary>Laying out a script page the way XDOS draws it</summary>
public static class PageLayout
{
	static readonly Regex TagRe = new(@"^\s*<(/?\w+)([^>]*)>");

	public sealed record Tag(int Line, string Name, Dictionary<string, string> Kv);

	public static (List<Tag> tags, Dictionary<int, string> plain) Parse(string text)
	{
		var tags = new List<Tag>();
		var plain = new Dictionary<int, string>();
		var lines = text.Replace("\r\n", "\n").Split('\n');
		for(int i = 0; i < lines.Length; i++) {
			var m = TagRe.Match(lines[i]);
			if(m.Success) {
				var kv = Project.Attrs(m.Groups[2].Value).ToDictionary(x => x.Key, x => x.Value.Trim('"'));
				tags.Add(new Tag(i, m.Groups[1].Value.ToLowerInvariant(), kv));
			} else plain[i] = lines[i];
		}
		return (tags, plain);
	}

	static int Num(Dictionary<string, string> kv, string k) => kv.TryGetValue(k, out var v) && int.TryParse(v, out int n) ? n : 0;

	/// <summary>The page holding line 'cur' (0-based). background: name -> picture or null;
	/// objects: pack name -> {entry name: object} or null</summary>
	public static Scene Page(string text, int cur, Func<string, Picture> background, Func<string, Dictionary<string, Obj>> objects)
	{
		var (tags, plain) = Parse(text);
		int start = 0;
		for(int i = 0; i < tags.Count; i++) if(tags[i].Name == "desktop" && tags[i].Line <= cur) start = i;
		string bg = null, res = "SDOS.RES";
		for(int i = 0; i <= start && i < tags.Count; i++) {
			if(tags[i].Name != "desktop") continue;
			if(tags[i].Kv.TryGetValue("background", out var b)) bg = b;
			if(tags[i].Kv.TryGetValue("resource", out var r) && r.Length > 0) res = r.ToUpperInvariant();
		}
		var s = new Scene();
		var bgPic = bg != null ? background(bg) : null;
		if(bgPic != null) Array.Copy(bgPic.Rgba, s.Back.Rgba, s.Back.Rgba.Length);
		else for(int i = 3; i < s.Back.Rgba.Length; i += 4) s.Back.Rgba[i] = 255;
		var objs = objects(res) ?? new Dictionary<string, Obj>();
		int end = tags.Count;
		for(int i = start + 1; i < tags.Count; i++) if(tags[i].Name == "desktop") { end = i; break; }
		var sprites = new List<(ImageObj img, byte[] pal, int x, int y)>();
		for(int i = start; i < end; i++) {
			var t = tags[i];
			switch(t.Name) {
				case "img" when t.Kv.TryGetValue("src", out var src) && objs.TryGetValue(src, out var o) && o is ImgRes ir:
					if(ir.Image.Type == 1) s.Cells(ir.Image, ir.Palette, Num(t.Kv, "left"), Num(t.Kv, "top"));
					else sprites.Add((ir.Image, ir.Palette, Num(t.Kv, "left"), Num(t.Kv, "top")));
					break;
				case "animate" when t.Kv.TryGetValue("src", out var src) && objs.TryGetValue(src, out var o) && o is AnimRes ar && ar.Anim.Frames.Count > 0: {
					var f = ar.Anim.Frames[^1];
					if(f.Image.Type == 0 || f.Image.Type == 4) s.Cells(f.Image, ar.Palette, f.X, f.Y);
					else sprites.Add((f.Image, ar.Palette, f.X - f.Xo, f.Y - f.Yo));
					break;
				}
				case "static":
					s.Text.Add((Num(t.Kv, "left"), Num(t.Kv, "top"), t.Kv.GetValueOrDefault("caption", "")));
					break;
				case "textbox": {
					int l = Num(t.Kv, "left"), tp = Num(t.Kv, "top"), r = Num(t.Kv, "right"), b = Num(t.Kv, "bottom");
					for(int y = tp * 16; y < b * 16 && y < 240; y++)
						for(int x = l * 8 + Scene.Shift; x < r * 8 + Scene.Shift && x < 256; x++) s.Back.Set(x, y, (0, 0, 0));
					int row = tp;
					int next = i + 1 < tags.Count ? tags[i + 1].Line : int.MaxValue;
					for(int k = t.Line + 1; k < next && row < b; k++)
						if(plain.TryGetValue(k, out var line)) { s.Text.Add((l, row, line.TrimEnd())); row++; }
					break;
				}
				case "menuitem":
					s.Menus.Add((Num(t.Kv, "left"), Num(t.Kv, "top"), Num(t.Kv, "right"), Num(t.Kv, "bottom")));
					break;
			}
		}
		foreach(var (img, pal, x, y) in sprites) s.Cells(img, pal, x, y);
		return s;
	}

	/// <summary>A resource on its own: a picture, or frame 'frame' of an animation or hover effect
	/// (a hover effect is shown at a pointer drawn at 120,112)</summary>
	public static Scene Resource(Obj o, int frame = 0)
	{
		var s = Scene.Checker();
		switch(o) {
			case ImgRes ir:
				s.Cells(ir.Image, ir.Palette, 0, 0);
				break;
			case AnimRes or Act or Anim: {
				var anim = o is AnimRes ar ? ar.Anim : o is Act act ? act.Anim : (Anim)o;
				var pal = o is AnimRes ar2 ? ar2.Palette : o is Act act2 ? act2.Raw[9..25] : new byte[16];
				if(anim == null || anim.Frames.Count == 0) break;
				var f = anim.Frames[((frame % anim.Frames.Count) + anim.Frames.Count) % anim.Frames.Count];
				int bx = o is Act ? 120 : 0, by = o is Act ? 112 : 0;
				if(o is Act) {
					s.Lines.Add((bx + 8, by - 6, bx + 8, by + 6));
					s.Lines.Add((bx + 2, by, bx + 14, by));
				}
				if(f.Image.Type == 0 || f.Image.Type == 4) s.Cells(f.Image, pal, f.X, f.Y);
				else s.Cells(f.Image, pal, bx + f.X - f.Xo, by + f.Y - f.Yo);
				break;
			}
		}
		return s;
	}
}
