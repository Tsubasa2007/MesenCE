using System.Diagnostics;
using System.Runtime.InteropServices;

namespace EasyMake.Ui;

/// <summary>Plays a WAV file (Windows' PlaySound; elsewhere the file opens in the default player)</summary>
public static class Sound
{
	[DllImport("winmm.dll", CharSet = CharSet.Unicode)]
	static extern bool PlaySound(string file, IntPtr module, uint flags);

	const uint SND_ASYNC = 0x0001, SND_FILENAME = 0x00020000, SND_PURGE = 0x0040;

	/// <summary>Self-tests: record what would play instead of playing it</summary>
	public static List<string> Muted;

	public static void Play(string path)
	{
		if(Muted != null) { Muted.Add(path); return; }
		if(OperatingSystem.IsWindows()) PlaySound(path, IntPtr.Zero, SND_FILENAME | SND_ASYNC);
		else Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
	}

	public static void Stop()
	{
		if(OperatingSystem.IsWindows()) PlaySound(null, IntPtr.Zero, SND_PURGE);
	}

	public static void OpenFolder(string path)
	{
		Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
	}
}
