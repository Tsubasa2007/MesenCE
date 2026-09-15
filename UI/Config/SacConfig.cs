using Mesen.Interop;
using CommunityToolkit.Mvvm.ComponentModel;
using System;
using System.Runtime.InteropServices;

namespace Mesen.Config;

public partial class SacConfig : BaseConfig<SacConfig>
{
	[ObservableProperty] public partial ControllerConfig Port1 { get; set; } = new();
	[ObservableProperty] public partial ControllerConfig Port2 { get; set; } = new();

	public void ApplyConfig()
	{
		//Both ports always hold the machine's own pad
		Port1.Type = ControllerType.SacController;
		Port2.Type = ControllerType.SacController;

		ConfigApi.SetSacConfig(new InteropSacConfig() {
			Port1 = Port1.ToInterop(),
			Port2 = Port2.ToInterop(),
		});
	}

	internal void InitializeDefaults(DefaultKeyMappingType defaultMappings)
	{
		Port1.InitDefaults(defaultMappings, ControllerType.SacController);
	}
}

[StructLayout(LayoutKind.Sequential)]
public struct InteropSacConfig
{
	public InteropControllerConfig Port1;
	public InteropControllerConfig Port2;
}
