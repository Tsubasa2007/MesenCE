using Mesen.Interop;
using Mesen.ViewModels;
using CommunityToolkit.Mvvm.ComponentModel;

namespace Mesen.Config;

public partial class SacEventViewerConfig : ViewModelBase
{
	[ObservableProperty] public partial EventViewerCategoryCfg VideoScrollWrite { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[0]);
	[ObservableProperty] public partial EventViewerCategoryCfg VideoWindowWrite { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[1]);
	[ObservableProperty] public partial EventViewerCategoryCfg VideoSpriteWrite { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[2]);
	[ObservableProperty] public partial EventViewerCategoryCfg VideoDmaWrite { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[3]);
	[ObservableProperty] public partial EventViewerCategoryCfg VideoOtherWrite { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[4]);
	[ObservableProperty] public partial EventViewerCategoryCfg VideoStatusRead { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[5]);
	[ObservableProperty] public partial EventViewerCategoryCfg VideoOtherRead { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[6]);

	[ObservableProperty] public partial EventViewerCategoryCfg PaletteWrite { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[7]);
	[ObservableProperty] public partial EventViewerCategoryCfg PaletteRead { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[8]);
	[ObservableProperty] public partial EventViewerCategoryCfg VramWrite { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[9]);
	[ObservableProperty] public partial EventViewerCategoryCfg VramRead { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[10]);

	[ObservableProperty] public partial EventViewerCategoryCfg SoundWrite { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[11]);
	[ObservableProperty] public partial EventViewerCategoryCfg SoundRead { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[12]);
	[ObservableProperty] public partial EventViewerCategoryCfg InputRead { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[13]);

	[ObservableProperty] public partial EventViewerCategoryCfg DmaWrite { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[14]);
	[ObservableProperty] public partial EventViewerCategoryCfg DmaRead { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[15]);
	[ObservableProperty] public partial EventViewerCategoryCfg IrqWrite { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[16]);
	[ObservableProperty] public partial EventViewerCategoryCfg IrqRead { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[17]);
	[ObservableProperty] public partial EventViewerCategoryCfg TimerWrite { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[18]);
	[ObservableProperty] public partial EventViewerCategoryCfg TimerRead { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[19]);
	[ObservableProperty] public partial EventViewerCategoryCfg OtherWrite { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[20]);
	[ObservableProperty] public partial EventViewerCategoryCfg OtherRead { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[21]);

	[ObservableProperty] public partial EventViewerCategoryCfg Irq { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[22]);
	[ObservableProperty] public partial EventViewerCategoryCfg MarkedBreakpoints { get; set; } = new EventViewerCategoryCfg(EventViewerColors.Colors[23]);

	[ObservableProperty] public partial bool ShowPreviousFrameEvents { get; set; } = true;

	public InteropSacEventViewerConfig ToInterop()
	{
		return new InteropSacEventViewerConfig() {
			VideoScrollWrite = this.VideoScrollWrite,
			VideoWindowWrite = this.VideoWindowWrite,
			VideoSpriteWrite = this.VideoSpriteWrite,
			VideoDmaWrite = this.VideoDmaWrite,
			VideoOtherWrite = this.VideoOtherWrite,
			VideoStatusRead = this.VideoStatusRead,
			VideoOtherRead = this.VideoOtherRead,
			PaletteWrite = this.PaletteWrite,
			PaletteRead = this.PaletteRead,
			VramWrite = this.VramWrite,
			VramRead = this.VramRead,
			SoundWrite = this.SoundWrite,
			SoundRead = this.SoundRead,
			InputRead = this.InputRead,
			DmaWrite = this.DmaWrite,
			DmaRead = this.DmaRead,
			IrqWrite = this.IrqWrite,
			IrqRead = this.IrqRead,
			TimerWrite = this.TimerWrite,
			TimerRead = this.TimerRead,
			OtherWrite = this.OtherWrite,
			OtherRead = this.OtherRead,
			Irq = this.Irq,
			MarkedBreakpoints = this.MarkedBreakpoints,

			ShowPreviousFrameEvents = this.ShowPreviousFrameEvents
		};
	}
}
