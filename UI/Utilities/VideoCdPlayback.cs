using Mesen.Config;
using Mesen.Interop;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace Mesen.Utilities
{
	//Playing a video the emulated machine has asked for.
	//
	//The machine's own picture cannot show one - see the note in VideoCdTrack - so the track
	//goes to whatever the system opens MPEG files with, and the machine is held in its wait
	//loop meanwhile. That is the whole of the illusion: the program waits, then carries on,
	//which is what it does on the real machine while its decoder chip has the screen.
	//
	//A real machine comes back by itself when the video ends, so this does too: the play
	//command names where to start and where to stop, so how long it runs for is known before
	//it is handed over, and when that time is up the window is closed and the machine
	//released. Closing the window early still works and simply gets there sooner - whichever
	//comes first wins.
	public static class VideoCdPlayback
	{
		private static readonly object _lock = new();
		private static bool _playing = false;
		//Whether the video ran to its end rather than being closed part way. A transport told
		//to play runs on through the disc, so the machine is told which of the two happened
		//and carries on into the next video only when the first is true. Closing the window is
		//how the user stops it.
		private static bool _completed = false;
		//Cancelled when a play ends some other way, so a window that was closed by hand does
		//not get a second ending when its running time would have been up
		private static CancellationTokenSource? _running;

		//Opening a player takes a moment, and the clock here starts before it appears. Rather
		//than cut the end off every video, give it a little longer than the machine asked for.
		private static readonly TimeSpan StartupGrace = TimeSpan.FromSeconds(2);
		//Extracted tracks are kept for the session: a title plays the same few over and over
		private static readonly Dictionary<string, string> _extracted = new();

		//Called once a frame, on the thread the machine runs on. Everything here has to be
		//cheap: reading a track out of the image takes long enough that doing it here stalls
		//the emulation for a visible moment, and a machine frozen mid-click sees the button
		//still down when it resumes and acts on it twice. So this only notices the request -
		//the work goes to a thread of its own.
		public static void Poll()
		{
			bool busy;
			lock(_lock) {
				busy = _playing;
			}
			if(busy) {
				//A play asked for while one is already on screen. Leaving it in the machine
				//would stall it until the request timed out, so take it and answer it at once
				//rather than opening a second window on top of the first.
				if(EmuApi.GetNesVideoPlayRequest(out _, out _, out _)) {
					//Not a video that ended, so the transport does not carry on from it
					EmuApi.NesVideoPlaybackEnded(false);
				}
				return;
			}

			if(!EmuApi.GetNesVideoPlayRequest(out byte track, out uint startMsf, out uint endMsf)) {
				return;
			}

			if(ConfigManager.Config.Nes.KewangDisableVideoPlayback) {
				//Answered rather than ignored. The machine is stopped in its playback loop until
				//somebody says the video is over, and leaving it there costs it the two seconds
				//the drive waits before giving up on its own. Not a completion, so a transport
				//running through the disc stops here instead of stepping to the next video.
				EmuApi.NesVideoPlaybackEnded(false);
				return;
			}

			lock(_lock) {
				_playing = true;
				_completed = false;
			}

			Task.Run(() => {
				try {
					if(!Start(track, startMsf, endMsf)) {
						Finish(false);
					}
				} catch(Exception ex) {
					EmuApi.WriteLogEntry("[Video CD] " + ex.Message);
					Finish(false);
				}
			});
		}

		//A position in the packet is minutes, seconds and frames, 75 frames to the second,
		//counted from the start of the track it names
		private static uint ToSectors(uint msf)
		{
			return ((msf >> 16) * 60 + ((msf >> 8) & 0xFF)) * 75 + (msf & 0xFF);
		}

		private static bool Start(byte track, uint startMsf, uint endMsf)
		{
			string discPath = EmuApi.GetNesVideoDiscPath();
			if(string.IsNullOrEmpty(discPath)) {
				return false;
			}

			List<VideoCdTrack> tracks = VideoCdTrack.ReadCueSheet(discPath, out string binPath);
			VideoCdTrack? wanted = tracks.Find(t => t.Number == track + 1);
			if(wanted == null) {
				//The scripts number the videos from one and the sheet counts the filesystem
				//as track 1, so a video's own number is one less than its track. A disc that
				//does not line up that way is not one to guess about.
				EmuApi.WriteLogEntry($"[Video CD] no track for video {track} on {Path.GetFileName(discPath)}");
				return false;
			}

			uint from = ToSectors(startMsf);
			uint to = ToSectors(endMsf);
			string outPath = ExtractOnce(wanted, binPath, discPath, from, to);
			Process? player = Process.Start(new ProcessStartInfo() { FileName = outPath, UseShellExecute = true });
			if(player == null) {
				//Nothing is registered for the file, or the shell handed it to something that
				//was already running. Either way there is no window of ours to wait on.
				return false;
			}

			player.EnableRaisingEvents = true;
			player.Exited += (s, e) => Finish(true);
			if(player.HasExited) {
				//Already gone by the time the handler was attached, so no window was ever up
				Finish(false);
				return true;
			}

			//75 sectors to the second, and the length is taken from what came out rather than
			//from what was asked for: the gaps between the parts of a track are dropped on the
			//way, so the file is shorter than the positions that named it.
			long sectors = new FileInfo(outPath).Length / VideoCdTrack.PayloadSize;
			CancellationTokenSource cts = new();
			lock(_lock) {
				_running = cts;
			}
			_ = Task.Delay(TimeSpan.FromSeconds(sectors / 75.0) + StartupGrace, cts.Token)
				.ContinueWith(t => {
					if(t.IsCanceled) {
						return;
					}
					//Set before the window goes, because closing it raises Exited and that
					//gets to Finish first
					lock(_lock) {
						_completed = true;
					}
					ClosePlayer(player);
					Finish(true);
				}, TaskScheduler.Default);
			return true;
		}

		private static string ExtractOnce(VideoCdTrack wanted, string binPath, string discPath, uint from, uint to)
		{
			//A stretch of a track is its own file: the same track appears several times over
			//with different bounds, and they are not interchangeable.
			string key = $"{binPath}#{wanted.Number}#{from}#{to}";
			lock(_lock) {
				if(_extracted.TryGetValue(key, out string? cached) && File.Exists(cached)) {
					return cached;
				}
			}

			string folder = VideoCdTrack.GetScratchFolder();
			string span = to > from ? $" {from}-{to}" : "";
			string outPath = Path.Combine(folder,
				Path.GetFileNameWithoutExtension(discPath) + $" - Video {wanted.VideoNumber}{span}.mpg");
			wanted.ExtractToFile(binPath, outPath, from, to);

			lock(_lock) {
				_extracted[key] = outPath;
			}
			return outPath;
		}

		//Ask the player to go away, the way closing its window would. Only ever the process
		//this started, and only a request first - a media player is the user's own program and
		//may well have other things open in it.
		private static void ClosePlayer(Process player)
		{
			try {
				if(player.HasExited) {
					return;
				}
				if(player.CloseMainWindow()) {
					player.WaitForExit(2000);
				}
				if(!player.HasExited) {
					player.Kill();
				}
			} catch(Exception ex) {
				//It may have gone on its own between the test and the request
				EmuApi.WriteLogEntry("[Video CD] could not close the player: " + ex.Message);
			}
		}

		//Let the machine out of its wait loop. hadWindow says a player really was on screen and
		//has now gone, which is the moment the machine's pointer has been left behind - see
		//MouseManager.WantCaptureSoon.
		private static void Finish(bool hadWindow)
		{
			CancellationTokenSource? cts;
			bool completed;
			lock(_lock) {
				if(!_playing) {
					//The running time was up and the window was closed by hand at the same
					//moment; the machine has already been let go once
					return;
				}
				_playing = false;
				completed = _completed;
				cts = _running;
				_running = null;
			}
			cts?.Cancel();
			cts?.Dispose();
			EmuApi.NesVideoPlaybackEnded(completed);

			if(hadWindow) {
				//Movement was not reaching the machine while the video had the focus, so its
				//pointer is wherever it was before. Taking the capture back means the next thing
				//the user does can be to aim, rather than having to click the old spot first.
				MouseManager.WantCaptureSoon();
			}
		}

		//A machine that stops mid-play would otherwise leave the flag set for the next one.
		//The extracted files go too: they are held for the session because a title plays the
		//same few over and over, but a whole disc of them is several hundred megabytes and
		//nothing else will ever come back for them.
		public static void Reset()
		{
			CancellationTokenSource? cts;
			List<string> files;
			lock(_lock) {
				_playing = false;
				_completed = false;
				cts = _running;
				_running = null;
				files = new List<string>(_extracted.Values);
				_extracted.Clear();
			}
			cts?.Cancel();
			cts?.Dispose();

			foreach(string file in files) {
				try {
					File.Delete(file);
				} catch {
					//A player may still have it open - it will be overwritten next time round
				}
			}
		}
	}
}
