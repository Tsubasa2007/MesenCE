using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Interop;
using System;

namespace Mesen.ViewModels
{
	//The transport for a video the core is showing on the machine's own screen.
	//
	//A video belongs to the emulated machine: it asked for one and is waiting in its own loop
	//until it is over. Nothing here tells the machine anything, so pausing simply leaves it
	//waiting a little longer, which is what it would be doing anyway. Skipping ends the video
	//the way running out would have, so what the machine does next is what it would have done.
	public partial class DiscVideoTransportViewModel : ViewModelBase
	{
		[ObservableProperty] public partial bool Visible { get; set; }
		[ObservableProperty] public partial bool IsPaused { get; set; }
		[ObservableProperty] public partial double Position { get; set; }
		[ObservableProperty] public partial double Duration { get; set; }
		[ObservableProperty] public partial string PositionText { get; set; } = "";

		//While the handle is held the bar belongs to the pointer, not to the video: letting
		//the poll write the position back would drag it out from under the user.
		public bool Scrubbing { get; set; }

		//Read once a frame, on the UI thread. Cheap: it is three doubles across the interop
		//boundary and nothing is allocated unless the text actually changes.
		public void Poll()
		{
			if(!EmuApi.GetNesDiscVideoStatus(out bool paused, out double position, out double duration)) {
				if(Visible) {
					Visible = false;
					Scrubbing = false;
				}
				return;
			}

			Visible = true;
			IsPaused = paused;

			//Never let a bad length out of here. Math.Clamp throws when the low bound is above
			//the high one, and this runs inside a posted callback where the exception goes
			//nowhere useful - it just stops the bar updating for the rest of the video.
			Duration = duration > 0 && !double.IsNaN(duration) ? duration : 0;
			if(!Scrubbing) {
				Position = Duration > 0 ? Math.Clamp(position, 0, Duration) : 0;
			}

			string text = Format(Position) + " / " + Format(Duration);
			if(text != PositionText) {
				PositionText = text;
			}
		}

		private static string Format(double seconds)
		{
			if(seconds < 0 || double.IsNaN(seconds)) {
				seconds = 0;
			}
			TimeSpan t = TimeSpan.FromSeconds(seconds);
			return t.Hours > 0
				? $"{(int)t.TotalHours}:{t.Minutes:00}:{t.Seconds:00}"
				: $"{t.Minutes}:{t.Seconds:00}";
		}

		public void TogglePause()
		{
			IsPaused = !IsPaused;
			EmuApi.SetNesDiscVideoPaused(IsPaused);
		}

		//Where the bar was let go. The core drops what it had already decoded, so the sound
		//does not carry a quarter second of the old position over the new picture.
		public void SeekTo(double seconds)
		{
			EmuApi.SeekNesDiscVideo(Math.Clamp(seconds, 0, Duration));
		}

		public void Skip()
		{
			EmuApi.SkipNesDiscVideo();
			Visible = false;
		}
	}
}
