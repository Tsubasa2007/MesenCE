using Avalonia.Controls;
using Avalonia.Threading;
using Mesen.Config;
using Mesen.Config.Shortcuts;
using Mesen.Interop;
using Mesen.Localization;
using Mesen.ViewModels;
using Mesen.Windows;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace Mesen.Utilities
{
	public static class LoadRomHelper
	{
		public static async void LoadRom(ResourcePath romPath, ResourcePath? patchPath = null)
		{
			if(FolderHelper.IsArchiveFile(romPath)) {
				ResourcePath? selectedRom = await SelectRomWindow.Show(romPath);
				if(selectedRom == null) {
					return;
				}
				romPath = selectedRom.Value;
			}

			if(patchPath == null && ConfigManager.Config.Preferences.AutoLoadPatches) {
				string[] extensions = new string[3] { ".ips", ".ups", ".bps" };
				foreach(string ext in extensions) {
					string file = Path.Combine(romPath.Folder, Path.GetFileNameWithoutExtension(romPath.FileName)) + ext;
					if(File.Exists(file)) {
						patchPath = file;
						break;
					}
				}
			}

			InternalLoadRom(romPath, patchPath);
		}

		private static void InternalLoadRom(ResourcePath romPath, ResourcePath? patchPath)
		{
			//Temporarily hide selection screen to allow displaying error messages
			MainWindowViewModel.Instance.RecentGames.Visible = false;

			Task.Run(() => {
				//Run in another thread to prevent deadlocks etc. when emulator notifications are processed UI-side
				if(EmuApi.LoadRom(romPath, patchPath)) {
					ConfigManager.Config.RecentFiles.AddRecentFile(romPath, patchPath);
					ConfigManager.Config.Save();
				}
				ShowSelectionOnScreenAfterError();
			});
		}

		public static void LoadRecentGame(string filename, bool forceLoadState)
		{
			//Temporarily hide selection screen to allow displaying error messages
			MainWindowViewModel.Instance.RecentGames.Visible = false;

			Task.Run(() => {
				//Run in another thread to prevent deadlocks etc. when emulator notifications are processed UI-side
				if(File.Exists(filename)) {
					EmuApi.LoadRecentGame(filename, !forceLoadState && ConfigManager.Config.Preferences.GameSelectionScreenMode == GameSelectionMode.PowerOn);
				}
				ShowSelectionOnScreenAfterError();
			});
		}

		private static void ShowSelectionOnScreenAfterError()
		{
			if(ConfigManager.Config.Preferences.GameSelectionScreenMode != GameSelectionMode.Disabled) {
				Thread.Sleep(3100);
				if(!EmuApi.IsRunning()) {
					//No game was loaded, show game selection screen again after ~3 seconds
					//This allows error messages to be visible to the user
					Dispatcher.UIThread.Post(() => {
						MainWindowViewModel.Instance.RecentGames.Visible = true;
					});
				}
			}
		}

		public static async void LoadPatchFile(string patchFile)
		{
			string? patchFolder = Path.GetDirectoryName(patchFile);
			if(patchFolder == null) {
				return;
			}

			List<string> romsInFolder = new List<string>();
			foreach(string filepath in Directory.EnumerateFiles(patchFolder)) {
				if(FolderHelper.IsRomFile(filepath)) {
					romsInFolder.Add(filepath);
				}
			}

			if(romsInFolder.Count == 1) {
				//There is a single rom in the same folder as the IPS/BPS patch, use it automatically
				LoadRom(romsInFolder[0], patchFile);
			} else {
				Window? wnd = ApplicationHelper.GetMainWindow();
				if(!EmuApi.IsRunning()) {
					//Prompt the user for a rom to load
					if(await MesenMsgBox.Show(wnd, "SelectRomIps", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) == DialogResult.OK) {
						string? filename = await FileDialogHelper.OpenFile(null, wnd, FileDialogHelper.RomExt);
						if(filename != null) {
							LoadRom(filename, patchFile);
						}
					}
				} else if(await MesenMsgBox.Show(wnd, "PatchAndReset", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) == DialogResult.OK) {
					//Confirm that the user wants to patch the current rom and reset
					LoadRom(EmuApi.GetRomInfo().RomPath, patchFile);
				}
			}
		}

		private static bool IsPatchFile(string filename)
		{
			using(FileStream? stream = FileHelper.OpenRead(filename)) {
				if(stream != null) {
					byte[] header = new byte[5];
					stream.ReadExactly(header, 0, 5);
					if(header[0] == 'P' && header[1] == 'A' && header[2] == 'T' && header[3] == 'C' && header[4] == 'H') {
						return true;
					} else if((header[0] == 'U' || header[0] == 'B') && header[1] == 'P' && header[2] == 'S' && header[3] == '1') {
						return true;
					}
				}
			}
			return false;
		}

		public static void LoadFile(string filename)
		{
			if(File.Exists(filename)) {
				string ext = Path.GetExtension(filename).ToLowerInvariant();
				if(IsPatchFile(filename)) {
					LoadPatchFile(filename);
				} else if(ext == "." + FileDialogHelper.MesenSaveStateExt) {
					EmuApi.LoadStateFile(filename);
				} else if(EmuApi.IsRunning() && (ext == "." + FileDialogHelper.MesenMovieExt || ext == "." + FileDialogHelper.BizHawkMovieExt || ext == "." + FileDialogHelper.GbaHawkMovieExt)) {
					RecordApi.MoviePlay(filename);
				} else if(ext == ".img" || ext == ".ima") {
					OpenLearningMachineDisk(filename);
				} else {
					LoadRom(filename);
				}
			} else {
				DisplayMessageHelper.DisplayMessage("Error", ResourceHelper.GetMessage("FileNotFound", filename));
			}
		}

		//A learning machine's floppy image can't boot on its own, so mount it and start a fresh run
		//of the BIOS (a .nes next to Mesen.exe) belonging to the machine whose disk it is - like
		//loading an FDS disk boots the FDS BIOS.
		private static async void OpenLearningMachineDisk(string imgPath)
		{
			bool sb2k = IsSb2kDisk(imgPath);
			string? bios = FindLearningMachineBios(sb2k);
			if(bios == null) {
				string machine = sb2k ? "Subor SB-2000" : "BBK";
				await MesenMsgBox.Show(null, "BbkBiosMissing", MessageBoxButtons.OK, MessageBoxIcon.Error, machine);
				return;
			}
			EmuApi.SetBbkBootDisk(imgPath);
			LoadRom(bios);
		}

		//Tell the two machines' floppies apart by the boot sector's OEM name: a BBK disk carries the
		//machine's own 6502 DOS and boots from it, while an SB-2000 disk is an ordinary PC-formatted
		//FAT12 floppy whose x86 boot sector the machine never runs.
		private static bool IsSb2kDisk(string imgPath)
		{
			try {
				using FileStream fs = File.OpenRead(imgPath);
				byte[] bootSector = new byte[8];
				if(fs.Read(bootSector, 0, 8) < 8) {
					return false;
				}
				return Encoding.ASCII.GetString(bootSector, 3, 5) != "SMDOS";
			} catch {
				return false;
			}
		}

		//Find a learning machine BIOS ROM in the Mesen executable's folder, identified by its iNES
		//header - the same signature the core matches on (mapper 171, >=128KB PRG, no CHR ROM, with
		//header byte 9 selecting which machine it is). Search the executable's own folder rather
		//than Program.OriginalFolder: that one is the working directory, which is only the same
		//folder when Mesen happens to be launched from it.
		private static string? FindLearningMachineBios(bool sb2k)
		{
			try {
				string? exeFolder = Path.GetDirectoryName(Program.ExePath);
				if(exeFolder == null) {
					return null;
				}
				foreach(string nes in Directory.EnumerateFiles(exeFolder, "*.nes")) {
					if(IsLearningMachineBios(nes, sb2k)) {
						return nes;
					}
				}
			} catch { }
			return null;
		}

		private static bool IsLearningMachineBios(string path, bool sb2k)
		{
			try {
				using FileStream fs = File.OpenRead(path);
				byte[] h = new byte[16];
				if(fs.Read(h, 0, 16) < 16) {
					return false;
				}
				if(h[0] != 'N' || h[1] != 'E' || h[2] != 'S' || h[3] != 0x1A) {
					return false;
				}
				int prg16k = h[4];               //PRG ROM in 16KB units
				int chr8k = h[5];                //CHR ROM in 8KB units
				int mapper = (h[6] >> 4) | (h[7] & 0xF0);
				bool isSb2kBios = (h[9] >> 1) == 1; //machine variant
				return mapper == 171 && prg16k >= 8 && chr8k == 0 && isSb2kBios == sb2k;
			} catch {
				return false;
			}
		}

		private static int _reloadRequestCounter = 0;
		public static void ResetReloadCounter()
		{
			//Reload/etc. operation is done, allow other calls
			Interlocked.Exchange(ref _reloadRequestCounter, 0);
		}

		private static void RunReloadShortcut(EmulatorShortcut shortcut)
		{
			//Block power cycle/power off/reload rom operations until the previous operation is done
			//This helps prevent a lot of edge cases that could happen in the UI when e.g spamming reload rom
			if(Interlocked.Increment(ref _reloadRequestCounter) == 1) {
				Task.Run(() => EmuApi.ExecuteShortcut(new ExecuteShortcutParams() { Shortcut = shortcut }));
			}
		}

		public static void Reset() { Task.Run(() => EmuApi.ExecuteShortcut(new ExecuteShortcutParams() { Shortcut = EmulatorShortcut.ExecReset })); }
		public static void PowerCycle() { RunReloadShortcut(EmulatorShortcut.ExecPowerCycle); }
		public static void PowerOff() { RunReloadShortcut(EmulatorShortcut.ExecPowerOff); }
		public static void ReloadRom() { RunReloadShortcut(EmulatorShortcut.ExecReloadRom); }
	}
}
