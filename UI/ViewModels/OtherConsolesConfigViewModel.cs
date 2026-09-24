using Avalonia;
using Avalonia.Controls;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Mesen.Config;
using Mesen.Controls;
using Mesen.Utilities;
using Mesen.Windows;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace Mesen.ViewModels
{
	public partial class OtherConsolesConfigViewModel : DisposableViewModel
	{
		[ObservableProperty] public partial CvConfig CvConfig { get; set; }
		[ObservableProperty] public partial CvConfig CvOriginalConfig { get; set; }
		[ObservableProperty] public partial SacConfig SacConfig { get; set; }
		[ObservableProperty] public partial SacConfig SacOriginalConfig { get; set; }
		[ObservableProperty] public partial OtherConsolesConfigTab SelectedTab { get; set; } = 0;

		public CvInputConfigViewModel CvInput { get; private set; }

		public IRelayCommand SetupSacPort1 { get; }
		public IRelayCommand SetupSacPort2 { get; }

		public Enum[] AvailableRegionsCv => new Enum[] {
			ConsoleRegion.Auto,
			ConsoleRegion.Ntsc,
			ConsoleRegion.Pal
		};

		public OtherConsolesConfigViewModel()
		{
			CvConfig = ConfigManager.Config.Cv;
			CvOriginalConfig = CvConfig.Clone();
			CvInput = new CvInputConfigViewModel(CvConfig);

			SacConfig = ConfigManager.Config.Sac;
			SacOriginalConfig = SacConfig.Clone();
			SetupSacPort1 = new RelayCommand<Button>(btn => this.OpenSacSetup(btn!, 0));
			SetupSacPort2 = new RelayCommand<Button>(btn => this.OpenSacSetup(btn!, 1));

			if(Design.IsDesignMode) {
				return;
			}

			AddDisposable(CvInput);
			AddDisposable(ReactiveHelper.RegisterRecursiveObserver(CvConfig, (s, e) => {
				CvConfig.ApplyConfig();
				ConfigManager.Config.Video.ApplyConfig();
			}));
			AddDisposable(ReactiveHelper.RegisterRecursiveObserver(SacConfig, (s, e) => {
				SacConfig.ApplyConfig();
				ConfigManager.Config.Video.ApplyConfig();
			}));
		}

		private async void OpenSacSetup(Button btn, int port)
		{
			PixelPoint startPosition = btn.PointToScreen(new Point(-7, btn.Bounds.Height));
			ControllerConfigWindow wnd = new ControllerConfigWindow();
			ControllerConfig orgCfg = port == 0 ? SacConfig.Port1 : SacConfig.Port2;
			ControllerConfig cfg = orgCfg.Clone();

			wnd.DataContext = new ControllerConfigViewModel(ControllerType.SacController, cfg, orgCfg, port);

			if(await wnd.ShowDialogAtPosition<bool>(btn.GetWindow(), startPosition)) {
				if(port == 0) {
					SacConfig.Port1 = cfg;
				} else {
					SacConfig.Port2 = cfg;
				}
			}
		}
	}

	public enum OtherConsolesConfigTab
	{
		ColecoVision,
		SuperAcan
	}
}
