using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Threading;
using Mesen.Config;
using Mesen.Interop;
using Mesen.Localization;
using Mesen.ViewModels;
using Mesen.Views;
using Mesen.Windows;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace Mesen.Utilities
{
	public class MouseManager : IDisposable
	{
		public const int LeftMouseButtonKeyCode = 0x200;
		public const int RightMouseButtonKeyCode = 0x201;
		public const int MiddleMouseButtonKeyCode = 0x202;
		public const int MouseButton4KeyCode = 0x203;
		public const int MouseButton5KeyCode = 0x204;

		private DispatcherTimer _timer = new DispatcherTimer(DispatcherPriority.Normal);

		private bool _usesSoftwareRenderer;
		private MainMenuView _mainMenu;
		private MainWindow _wnd;

		private int _prevPositionX;
		private int _prevPositionY;
		private bool _mouseCaptured = false;

		//Normally the capture is taken when the picture is clicked. That does not suit a
		//machine whose mouse reports movement and never position: until the capture is on,
		//movement is not passed on, so the pointer that machine draws stays where it was last
		//left and there is no way to aim - the first click has to land on the old spot before
		//anything can be moved. That only bites after something else has had the focus, so
		//rather than change when the capture is taken, the one case that causes it asks for it.
		//See VideoCdPlayback, which asks when a video window it opened has closed.
		private static DateTime _captureWantedUntil = DateTime.MinValue;

		//Good for a few seconds: long enough to cover the pointer being moved back over the
		//picture, short enough not to surprise anyone later on.
		public static void WantCaptureSoon()
		{
			_captureWantedUntil = DateTime.Now.AddSeconds(5);
		}

		private static bool CaptureWanted => DateTime.Now < _captureWantedUntil;

		//A captured mouse is normally pinned - a clipping rectangle holds the pointer inside
		//the window, it is re-centered on every poll, and the drift from center is the
		//movement. Neither half survives Remote Desktop, where the pointer belongs to the
		//client and is moved there in absolute coordinates: the re-center is applied after
		//the position has been read back, so the difference reads as movement never made,
		//and the clip is enforced on this machine only, so the pointer stops dead at the
		//window edge while the client's carries on. Both are therefore skipped remotely.
		//
		//Movement is scaled down by how far the window is stretched over the emulated screen,
		//so that the pointer covers the same ground on screen as it does on the desk. Over
		//Remote Desktop the pointer is moved by the client, which has already applied its own
		//acceleration and works in a session whose scaling need not match the desk at all, and
		//the result is an emulated pointer that crawls while the real one crosses the whole
		//window. Give a remote session the movement it is short of, rather than making the
		//user raise a sensitivity setting that would then be wrong everywhere else. How much
		//is Input.RemoteSessionMouseScale, which lives in settings.json only.
		[DllImport("user32.dll")] private static extern int GetSystemMetrics(int index);
		private const int SM_REMOTESESSION = 0x1000;
		private static readonly bool _isRemoteSession = OperatingSystem.IsWindows() && GetSystemMetrics(SM_REMOTESESSION) != 0;
		private bool _closeMenuPending = false;
		private DateTime _lastMouseMove = DateTime.Now;

		public MouseManager(MainWindow wnd, MainMenuView mainMenu, bool usesSoftwareRenderer)
		{
			_wnd = wnd;
			_mainMenu = mainMenu;
			_usesSoftwareRenderer = usesSoftwareRenderer;

			_timer.Interval = TimeSpan.FromMilliseconds(15);
			_timer.Tick += TmrProcessMouse;
			_timer.Start();
		}

		private bool IsPointerInMenu()
		{
			return _mainMenu.IsPointerOver || MenuHelper.IsPointerInMenu(_mainMenu.MainMenu);
		}

		private void TmrProcessMouse(object? sender, EventArgs e)
		{
			UpdateMainMenuVisibility();

			if(MainWindowViewModel.Instance.RecentGames.Visible) {
				return;
			}

			SystemMouseState mouseState = InputApi.GetSystemMouseState(GetRendererHandle());

			if((mouseState.LeftButton || _closeMenuPending) && _wnd.IsActive && !IsPointerInMenu() && (EmuApi.IsRunning() || !MainWindowViewModel.Instance.RecentGames.Visible)) {
				if(_closeMenuPending && !mouseState.LeftButton) {
					//Close menu when renderer is clicked, after the mouse button is released (while not in the menu)
					_mainMenu.MainMenu.Close();
					if(MainWindowViewModel.Instance.AudioPlayer == null) {
						//Only give renderer focus when the audio player isn't active
						//Otherwise clicking on the audio player's buttons does nothing
						_wnd.Renderer.Focus();
					}
					_closeMenuPending = false;
				} else {
					_closeMenuPending = true;
				}
			} else {
				_closeMenuPending = false;
			}

			PixelPoint rendererTopLeft = _wnd.Renderer.PointToScreen(new Point());
			PixelRect rendererScreenRect = new PixelRect(rendererTopLeft, PixelSize.FromSize(_wnd.Renderer.Bounds.Size, LayoutHelper.GetLayoutScale(_wnd) / InputApi.GetPixelScale()));

			if(_prevPositionX != mouseState.XPosition || _prevPositionY != mouseState.YPosition) {
				//Send mouse movement x/y values to core
				if(_mouseCaptured) {
					int deltaX = mouseState.XPosition - _prevPositionX;
					int deltaY = mouseState.YPosition - _prevPositionY;
					if(_isRemoteSession) {
						int scale = (int)ConfigManager.Config.Input.RemoteSessionMouseScale;
						deltaX *= scale;
						deltaY *= scale;
					}
					InputApi.SetMouseMovement((Int16)deltaX, (Int16)deltaY);
				}
				_prevPositionX = mouseState.XPosition;
				_prevPositionY = mouseState.YPosition;
				_lastMouseMove = DateTime.Now;
			}
			PixelPoint mousePos = new PixelPoint(mouseState.XPosition, mouseState.YPosition);

			if(_wnd.IsActive && (_mainMenu.IsPointerOver || _mainMenu.IsKeyboardFocusWithin || _mainMenu.MainMenu.IsOpen)) {
				//When mouse or keyboard focus is in menu, release mouse and keep arrow cursor
				SetMouseOffScreen();
				ReleaseMouse();
				if(rendererScreenRect.Contains(mousePos)) {
					SetMouseCursor(CursorImage.Arrow);
				}
				return;
			}

			if(rendererScreenRect.Contains(mousePos)) {
				//Send mouse state to emulation core
				Point rendererPos = _wnd.Renderer.PointToClient(mousePos) * LayoutHelper.GetLayoutScale(_wnd) / InputApi.GetPixelScale();
				InputApi.SetMousePosition(rendererPos.X / rendererScreenRect.Width, rendererPos.Y / rendererScreenRect.Height);

				bool buttonPressed = (mouseState.LeftButton || mouseState.RightButton || mouseState.MiddleButton || mouseState.Button4 || mouseState.Button5);

				InputApi.SetKeyState(LeftMouseButtonKeyCode, mouseState.LeftButton);
				InputApi.SetKeyState(RightMouseButtonKeyCode, mouseState.RightButton);
				InputApi.SetKeyState(MiddleMouseButtonKeyCode, mouseState.MiddleButton);
				InputApi.SetKeyState(MouseButton4KeyCode, mouseState.Button4);
				InputApi.SetKeyState(MouseButton5KeyCode, mouseState.Button5);

				if(!_mouseCaptured && AllowMouseCapture && (buttonPressed || CaptureWanted)) {
					//If the mouse button is clicked and mouse isn't captured but can be, turn on mouse capture
					CaptureMouse(buttonPressed);
					if(_mouseCaptured) {
						_captureWantedUntil = DateTime.MinValue;
					}
				}

				if(_mouseCaptured) {
					if(AllowMouseCapture) {
						SetMouseCursor(CursorImage.Hidden);
						if(!_isRemoteSession) {
							InputApi.SetSystemMousePosition(rendererTopLeft.X + rendererScreenRect.Width / 2, rendererTopLeft.Y + rendererScreenRect.Height / 2);
							SystemMouseState newState = InputApi.GetSystemMouseState(GetRendererHandle());
							_prevPositionX = newState.XPosition;
							_prevPositionY = newState.YPosition;
						}
					} else {
						ReleaseMouse();
					}
				}

				if(!_mouseCaptured) {
					SetMouseCursor(MouseIcon);
				}
			} else {
				if(_mouseCaptured && _isRemoteSession) {
					//Nothing pins the pointer inside the window here, so show it again once it
					//leaves - it still drives the emulated one, and hiding it would leave no
					//way to tell where it went.
					SetMouseCursor(CursorImage.Arrow);
				}
				SetMouseOffScreen();
			}
		}

		private void SetMouseCursor(CursorImage icon)
		{
			InputApi.SetCursorImage(icon);
			if(_usesSoftwareRenderer && !OperatingSystem.IsMacOS()) {
				//On MacOS, also setting the cursor on the renderer causes the cursor visibility to act oddly
				_wnd.Renderer.Cursor = new Cursor(icon.ToStandardCursorType());
			}
		}

		private void UpdateMainMenuVisibility()
		{
			//Get global mouse position without restrictions - need to know if mouse is over menu or not
			SystemMouseState mouseState = InputApi.GetSystemMouseState(IntPtr.Zero);
			PixelPoint mousePos = new PixelPoint(mouseState.XPosition, mouseState.YPosition);

			bool inExclusiveFullscreen = _wnd.WindowState == WindowState.FullScreen && ConfigManager.Config.Video.UseExclusiveFullscreen;
			bool autoHideMenu = _wnd.WindowState == WindowState.FullScreen || ConfigManager.Config.Preferences.AutoHideMenu;
			if(inExclusiveFullscreen) {
				MainWindowViewModel.Instance.IsMenuVisible = false;
			} else if(autoHideMenu) {
				if(_mainMenu.MainMenu.IsOpen) {
					MainWindowViewModel.Instance.IsMenuVisible = true;
				} else {
					PixelPoint wndTopLeft = _wnd.PointToScreen(new Point(0, 0));
					double scale = LayoutHelper.GetLayoutScale(_wnd);
					bool showMenu = (
						mousePos.Y >= wndTopLeft.Y - 15 && mousePos.Y <= wndTopLeft.Y + Math.Max(_mainMenu.Bounds.Height * scale + 10, 35 * scale) &&
						mousePos.X >= wndTopLeft.X && mousePos.X <= wndTopLeft.X + _wnd.Bounds.Width * scale
					);
					MainWindowViewModel.Instance.IsMenuVisible = showMenu;
				}
			} else {
				MainWindowViewModel.Instance.IsMenuVisible = true;
			}
		}

		private static void SetMouseOffScreen()
		{
			//Send mouse state to emulation core (mouse is set as off the screen)
			InputApi.SetMousePosition(-1, -1);
			InputApi.SetKeyState(LeftMouseButtonKeyCode, false);
			InputApi.SetKeyState(RightMouseButtonKeyCode, false);
			InputApi.SetKeyState(MiddleMouseButtonKeyCode, false);
			InputApi.SetKeyState(MouseButton4KeyCode, false);
			InputApi.SetKeyState(MouseButton5KeyCode, false);
		}

		private bool AllowMouseCapture
		{
			get
			{
				if(!EmuApi.IsRunning() || EmuApi.IsPaused() || !_wnd.IsActive) {
					return false;
				}

				bool hasMouseDevice = (
					InputApi.HasControlDevice(ControllerType.SnesMouse) ||
					InputApi.HasControlDevice(ControllerType.SuborMouse) ||
					InputApi.HasControlDevice(ControllerType.SuborMouse24) ||
					InputApi.HasControlDevice(ControllerType.BbkMouse) ||
					InputApi.HasControlDevice(ControllerType.Sb2kMouse) ||
					InputApi.HasControlDevice(ControllerType.YuxingMouse) ||
					InputApi.HasControlDevice(ControllerType.YuxingSerialMouse) ||
					InputApi.HasControlDevice(ControllerType.FamicomArkanoidController) ||
					InputApi.HasControlDevice(ControllerType.NesArkanoidController) ||
					InputApi.HasControlDevice(ControllerType.HoriTrack)
				);

				if(hasMouseDevice) {
					return true;
				}

				return false;
			}
		}

		private CursorImage MouseIcon
		{
			get
			{
				if(!EmuApi.IsRunning() || EmuApi.IsPaused()) {
					return CursorImage.Arrow;
				}

				bool hasLightGun = (
					InputApi.HasControlDevice(ControllerType.FamicomZapper) ||
					InputApi.HasControlDevice(ControllerType.NesZapper) ||
					InputApi.HasControlDevice(ControllerType.SmsLightPhaser) ||
					InputApi.HasControlDevice(ControllerType.SuperScope) ||
					InputApi.HasControlDevice(ControllerType.BandaiHyperShot)
				);

				if(hasLightGun) {
					if(ConfigManager.Config.Input.HidePointerForLightGuns) {
						return CursorImage.Hidden;
					} else {
						return CursorImage.Cross;
					}
				} else if(InputApi.HasControlDevice(ControllerType.OekaKidsTablet)) {
					return CursorImage.Cross;
				}

				if((DateTime.Now - _lastMouseMove).TotalSeconds > 1) {
					return CursorImage.Hidden;
				} else {
					return CursorImage.Arrow;
				}
			}
		}

		//announce is off for a capture nobody asked for by clicking - saying so would be noise
		private void CaptureMouse(bool announce = true)
		{
			if(!_mouseCaptured && AllowMouseCapture) {
				PixelPoint topLeft = _wnd.Renderer.PointToScreen(new Point());
				PixelRect rendererScreenRect = new PixelRect(topLeft, PixelSize.FromSize(_wnd.Renderer.Bounds.Size, LayoutHelper.GetLayoutScale(_wnd)));

				if(InputApi.CaptureMouse(topLeft.X, topLeft.Y, rendererScreenRect.Width, rendererScreenRect.Height, GetRendererHandle())) {
					if(_isRemoteSession) {
						//Keep the capture, drop the clipping rectangle - see the note above
						InputApi.ReleaseMouse();
					}
					if(announce) {
						DisplayMessageHelper.DisplayMessage("Input", ResourceHelper.GetMessage("MouseModeEnabled"));
					}
					_mouseCaptured = true;
				}
			}
		}

		private void ReleaseMouse()
		{
			if(_mouseCaptured) {
				_mouseCaptured = false;
				InputApi.ReleaseMouse();
			}
		}

		private IntPtr GetRendererHandle()
		{
			return _usesSoftwareRenderer ? IntPtr.Zero : (_wnd.Renderer as NativeRenderer)!.Handle;
		}

		public void Dispose()
		{
			if(_timer is DispatcherTimer timer) {
				timer.Tick -= TmrProcessMouse;
				timer.Stop();
			}
		}
	}
}
