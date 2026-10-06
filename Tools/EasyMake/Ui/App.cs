using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Themes.Fluent;

namespace EasyMake.Ui;

public sealed class App : Application
{
	public static string[] Args = Array.Empty<string>();

	//AvaloniaEdit's styles load by name at run time; the assembly is kept whole when trimming
	//(TrimmerRootAssembly in the project), so this is safe
	[System.Diagnostics.CodeAnalysis.UnconditionalSuppressMessage("Trimming", "IL2026")]
	public override void Initialize()
	{
		Styles.Add(new FluentTheme());
		Styles.Add(new Avalonia.Markup.Xaml.Styling.StyleInclude(new Uri("avares://EasyMake/"))
		{
			Source = new Uri("avares://AvaloniaEdit/Themes/Fluent/AvaloniaEdit.xaml")
		});
		RequestedThemeVariant = Avalonia.Styling.ThemeVariant.Light;
	}

	public override void OnFrameworkInitializationCompleted()
	{
		if(ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop) {
			desktop.MainWindow = new MainWindow(Args);
		}
		base.OnFrameworkInitializationCompleted();
	}

	public static int Start(string[] args)
	{
		Args = args;
		//Pictures the PNG reader cannot take (JPEG, BMP, interlaced or 16-bit PNG) go through Avalonia
		Core.Png.FallbackDecoder = path => {
			using var bmp = new Avalonia.Media.Imaging.Bitmap(path);
			int w = bmp.PixelSize.Width, h = bmp.PixelSize.Height;
			var buf = new byte[w * h * 4];
			var handle = System.Runtime.InteropServices.GCHandle.Alloc(buf, System.Runtime.InteropServices.GCHandleType.Pinned);
			try {
				bmp.CopyPixels(new PixelRect(0, 0, w, h), handle.AddrOfPinnedObject(), buf.Length, w * 4);
			} finally {
				handle.Free();
			}
			//CopyPixels gives the bitmap's own format: Bgra8888, premultiplied
			var p = new Core.Picture(w, h);
			for(int i = 0; i < w * h; i++) {
				byte b = buf[i * 4], g = buf[i * 4 + 1], r = buf[i * 4 + 2], a = buf[i * 4 + 3];
				if(a > 0 && a < 255) { r = (byte)Math.Min(255, r * 255 / a); g = (byte)Math.Min(255, g * 255 / a); b = (byte)Math.Min(255, b * 255 / a); }
				p.Rgba[i * 4] = r; p.Rgba[i * 4 + 1] = g; p.Rgba[i * 4 + 2] = b; p.Rgba[i * 4 + 3] = a;
			}
			return p;
		};
		return AppBuilder.Configure<App>().UsePlatformDetect().LogToTrace().StartWithClassicDesktopLifetime(args);
	}
}
