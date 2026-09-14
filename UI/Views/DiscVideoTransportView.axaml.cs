using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Markup.Xaml;
using Mesen.ViewModels;

namespace Mesen.Views
{
	public class DiscVideoTransportView : UserControl
	{
		public DiscVideoTransportView()
		{
			InitializeComponent();

			Slider seekBar = this.GetControl<Slider>("SeekBar");

			//While the handle is held the bar is the pointer's, so the once-a-frame poll stops
			//writing the position into it. The seek itself waits for the release: seeking on
			//every value change would ask the decoder for a new picture on every pixel of the
			//drag, and each one throws away the sound already decoded.
			seekBar.AddHandler(PointerPressedEvent, (s, e) => {
				if(DataContext is DiscVideoTransportViewModel vm) {
					vm.Scrubbing = true;
				}
			}, Avalonia.Interactivity.RoutingStrategies.Tunnel);

			seekBar.AddHandler(PointerReleasedEvent, (s, e) => {
				if(DataContext is DiscVideoTransportViewModel vm && vm.Scrubbing) {
					vm.Scrubbing = false;
					vm.SeekTo(seekBar.Value);
				}
			}, Avalonia.Interactivity.RoutingStrategies.Tunnel);

			//A click on the track rather than a drag moves the handle without ever pressing it
			seekBar.AddHandler(PointerCaptureLostEvent, (s, e) => {
				if(DataContext is DiscVideoTransportViewModel vm && vm.Scrubbing) {
					vm.Scrubbing = false;
					vm.SeekTo(seekBar.Value);
				}
			});
		}

		private void InitializeComponent()
		{
			AvaloniaXamlLoader.Load(this);
		}

		private void OnPlayPauseClick(object sender, Avalonia.Interactivity.RoutedEventArgs e)
		{
			(DataContext as DiscVideoTransportViewModel)?.TogglePause();
		}

		private void OnSkipClick(object sender, Avalonia.Interactivity.RoutedEventArgs e)
		{
			(DataContext as DiscVideoTransportViewModel)?.Skip();
		}
	}
}
