using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;
using Mesen.Config;
using Mesen.Config.Shortcuts;
using Mesen.Interop;
using Mesen.Utilities;
using Mesen.ViewModels;
using CommunityToolkit.Mvvm.ComponentModel;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;

namespace Mesen.Windows
{
	public partial class SelectDiskWindow : MesenWindow
	{
		private ListBox _listBox;
		private TextBox _searchBox;

		public SelectDiskWindow()
		{
			InitializeComponent();

			_searchBox = this.GetControl<TextBox>("Search");
			_listBox = this.GetControl<ListBox>("ListBox");
		}

		private void InitializeComponent()
		{
			AvaloniaXamlLoader.Load(this);
		}

		protected override void OnOpened(EventArgs e)
		{
			base.OnOpened(e);
			Dispatcher.UIThread.Post(() => {
				Activate();
				_searchBox.Focus();
			});
		}

		protected override void OnKeyDown(KeyEventArgs e)
		{
			if(DataContext is SelectDiskViewModel model) {
				if(e.Key == Key.Down || e.Key == Key.Up) {
					if(_searchBox.IsKeyboardFocusWithin && model.FilteredEntries.Any()) {
						model.SelectedEntry = model.FilteredEntries.First();
						_listBox.ContainerFromIndex(0)?.Focus();
					}
				} else if(e.Key == Key.Enter) {
					InsertSelected();
				} else if(e.Key == Key.Escape) {
					Close();
				}
			}
			base.OnKeyDown(e);
		}

		public static async Task Open(Window parent)
		{
			//Open even when the list is empty: the window's Change Folder button is how the user
			//points at a folder that contains disk images, so it must be reachable from an empty folder.
			SelectDiskViewModel model = new();
			SelectDiskWindow wnd = new SelectDiskWindow() { DataContext = model };
			wnd.WindowStartupLocation = WindowStartupLocation.CenterOwner;
			await wnd.ShowDialog(parent);
		}

		private void InsertSelected()
		{
			if(DataContext is SelectDiskViewModel model && model.SelectedEntry != null) {
				EmuApi.ExecuteShortcut(new ExecuteShortcutParams() { Shortcut = EmulatorShortcut.FdsInsertDiskNumber, Param = (uint)model.SelectedEntry.Index });
				Close();
			}
		}

		private void OnInsertClick(object sender, RoutedEventArgs e)
		{
			InsertSelected();
		}

		private void OnEjectClick(object sender, RoutedEventArgs e)
		{
			EmuApi.ExecuteShortcut(new ExecuteShortcutParams() { Shortcut = EmulatorShortcut.FdsEjectDisk });
			Close();
		}

		private async void OnChangeFolderClick(object sender, RoutedEventArgs e)
		{
			string? folder = await FileDialogHelper.OpenFolder(this);
			if(folder != null && DataContext is SelectDiskViewModel model) {
				//Update the default BBK disk folder and push it to the core, then reload the list.
				//The core scans the folder lazily, so the new images show up on the next query.
				ConfigManager.Config.Nes.BbkDiskFolder = folder;
				ConfigManager.Config.Nes.ApplyConfig();
				ConfigManager.Config.Save();
				model.Load();
			}
		}

		private void OnCancelClick(object sender, RoutedEventArgs e)
		{
			Close();
		}

		bool _isDoubleTap = false;
		private void OnPointerReleased(object? sender, PointerReleasedEventArgs e)
		{
			if(_isDoubleTap) {
				InsertSelected();
				_isDoubleTap = false;
			}
		}

		private void OnDoubleTapped(object sender, TappedEventArgs e)
		{
			_isDoubleTap = true;
		}

		protected override void OnClosed(EventArgs e)
		{
			((SelectDiskViewModel?)DataContext)?.Dispose();
			base.OnClosed(e);
		}
	}

	public class BbkDiskEntry
	{
		public int Index { get; }
		public string Name { get; }
		public bool IsInserted { get; }

		public BbkDiskEntry(int index, string name, bool isInserted)
		{
			Index = index;
			Name = name;
			IsInserted = isInserted;
		}

		public override string ToString()
		{
			return IsInserted ? Name + "  ● (inserted)" : Name;
		}
	}

	public partial class SelectDiskViewModel : DisposableViewModel
	{
		private List<BbkDiskEntry> _entries = new();
		[ObservableProperty] public partial IEnumerable<BbkDiskEntry> FilteredEntries { get; set; } = Array.Empty<BbkDiskEntry>();
		[ObservableProperty] public partial string SearchString { get; set; } = "";
		[ObservableProperty] public partial BbkDiskEntry? SelectedEntry { get; set; }
		[ObservableProperty] public partial bool HasInsertedDisk { get; set; }

		public SelectDiskViewModel()
		{
			Load();
		}

		partial void OnSearchStringChanged(string value)
		{
			ApplyFilter();
		}

		//(Re)read the disk list from the core - used on open and after the disk folder changes
		public void Load()
		{
			(int currentIndex, List<string> disks) = EmuApi.GetNesDiskList();
			_entries = disks.Select((name, i) => new BbkDiskEntry(i, name, i == currentIndex)).ToList();
			HasInsertedDisk = currentIndex >= 0;
			ApplyFilter();
		}

		private void ApplyFilter()
		{
			string search = SearchString;
			FilteredEntries = string.IsNullOrWhiteSpace(search)
				? _entries
				: _entries.Where(e => e.Name.Contains(search, StringComparison.OrdinalIgnoreCase)).ToList();
			SelectedEntry = FilteredEntries.FirstOrDefault(e => e.IsInserted) ?? FilteredEntries.FirstOrDefault();
		}
	}
}
