using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using TextBlock = System.Windows.Controls.TextBlock;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// The trophy notification shown over the game: a small borderless window that never takes
    /// focus and lets every click through, placed in a corner of the monitor the game is on. It
    /// shows over windowed and borderless fullscreen games (SDL's fullscreen is borderless);
    /// unlocks arriving together are shown one after another.
    /// </summary>
    public sealed class TrophyToast : Window
    {
        private static readonly Queue<Trophy> Pending = new();
        private static bool _showing;

        public static Color GradeColor(Grade grade) => grade switch
        {
            Grade.Platinum => Color.FromRgb(0xB9, 0xD3, 0xEE),
            Grade.Gold => Color.FromRgb(0xE3, 0xB3, 0x41),
            Grade.Silver => Color.FromRgb(0xC4, 0xC8, 0xD0),
            _ => Color.FromRgb(0xC8, 0x84, 0x4B),
        };

        /// <summary>Queues a notification (and its sound) for an unlocked trophy.</summary>
        public static void Announce(Trophy trophy)
        {
            Pending.Enqueue(trophy);
            if (!_showing) Next();
        }

        private static void Next()
        {
            if (Pending.Count == 0)
            {
                _showing = false;
                return;
            }
            _showing = true;
            var trophy = Pending.Dequeue();
            var s = Workspace.Current.Settings;
            if (s.Bool("trophy_sound"))
                Chime.Play(s.Int("trophy_volume"), s.Str("trophy_sound_file"));
            new TrophyToast(trophy, s.Str("trophy_position")).Show();
        }

        private readonly string _corner;
        private readonly TranslateTransform _slide = new();

        private TrophyToast(Trophy trophy, string corner)
        {
            _corner = corner;
            // Crisp text, as the main window's (a window does not inherit them).
            System.Windows.Media.TextOptions.SetTextFormattingMode(this, System.Windows.Media.TextFormattingMode.Display);
            System.Windows.Media.TextOptions.SetTextHintingMode(this, System.Windows.Media.TextHintingMode.Fixed);
            UseLayoutRounding = true;
            WindowStyle = WindowStyle.None;
            AllowsTransparency = true;
            Background = Brushes.Transparent;
            Topmost = true;
            ShowActivated = false;
            ShowInTaskbar = false;
            ResizeMode = ResizeMode.NoResize;
            SizeToContent = SizeToContent.WidthAndHeight;
            Focusable = false;
            IsHitTestVisible = false;

            // As the PS4 shows it: a slim dark bar with a soft sheen and a hairline border, a grey
            // tile with a white trophy, the trophy's name after a small cup in its grade's colour,
            // and "You have earned a trophy!" under it.
            var color = GradeColor(trophy.Grade);
            var face = new FontFamily("Segoe UI");

            var tile = new Border
            {
                Width = 46,
                Height = 46,
                CornerRadius = new CornerRadius(2),
                Background = new LinearGradientBrush(Color.FromRgb(0x5A, 0x5D, 0x62), Color.FromRgb(0x33, 0x35, 0x39), 90),
                BorderBrush = new SolidColorBrush(Color.FromArgb(0x50, 0xFF, 0xFF, 0xFF)),
                BorderThickness = new Thickness(1),
                Child = new SymbolIcon { Symbol = SymbolRegular.Trophy24, Filled = true, FontSize = 24, Foreground = new SolidColorBrush(Color.FromRgb(0xF2, 0xF2, 0xF2)) },
                VerticalAlignment = VerticalAlignment.Center,
            };

            var cup = new SymbolIcon
            {
                Symbol = SymbolRegular.Trophy24,
                Filled = true,
                FontSize = 15,
                Foreground = new SolidColorBrush(color),
                Margin = new Thickness(0, 1, 7, 0),
                VerticalAlignment = VerticalAlignment.Center,
            };
            var name = new TextBlock
            {
                Text = trophy.Name,
                FontFamily = face,
                FontSize = 17,
                Foreground = new SolidColorBrush(Color.FromRgb(0xF4, 0xF4, 0xF4)),
                TextTrimming = TextTrimming.CharacterEllipsis,
                MaxWidth = 330,
                VerticalAlignment = VerticalAlignment.Center,
            };
            var title = new StackPanel { Orientation = Orientation.Horizontal };
            title.Children.Add(cup);
            title.Children.Add(name);

            var earned = new TextBlock
            {
                Text = L.T("You have earned a trophy!"),
                FontFamily = face,
                FontSize = 13.5,
                Foreground = new SolidColorBrush(Color.FromRgb(0xC9, 0xCB, 0xCF)),
                Margin = new Thickness(0, 2, 0, 0),
            };
            var text = new StackPanel { Margin = new Thickness(14, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
            text.Children.Add(title);
            text.Children.Add(earned);

            var row = new StackPanel { Orientation = Orientation.Horizontal };
            row.Children.Add(tile);
            row.Children.Add(text);

            // The sheen: lighter at the top, darker at the bottom, a little see-through.
            var background = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(0, 1) };
            background.GradientStops.Add(new GradientStop(Color.FromArgb(0xEE, 0x4A, 0x4D, 0x52), 0));
            background.GradientStops.Add(new GradientStop(Color.FromArgb(0xEE, 0x2C, 0x2E, 0x32), 0.45));
            background.GradientStops.Add(new GradientStop(Color.FromArgb(0xF2, 0x1C, 0x1D, 0x20), 1));

            Content = new Border
            {
                Margin = new Thickness(18),
                Padding = new Thickness(9, 9, 26, 9),
                CornerRadius = new CornerRadius(3),
                Background = background,
                BorderBrush = new SolidColorBrush(Color.FromArgb(0x66, 0xC8, 0xCC, 0xD2)),
                BorderThickness = new Thickness(1),
                Effect = new System.Windows.Media.Effects.DropShadowEffect { BlurRadius = 14, ShadowDepth = 2, Opacity = 0.6 },
                Child = row,
                MinWidth = 380,
                RenderTransform = _slide,
            };
            Opacity = 0;

            SourceInitialized += (_, _) =>
            {
                // Click-through, never activated, no Alt+Tab entry.
                var handle = new WindowInteropHelper(this).Handle;
                SetWindowLong(handle, GWL_EXSTYLE, GetWindowLong(handle, GWL_EXSTYLE) |
                    WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED);
                // SDL makes a focused fullscreen game window topmost too, and raises it again on its
                // own: the toast went under it (its sound played, nothing showed). Back on top while
                // it shows, without taking focus.
                var raise = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromMilliseconds(200) };
                raise.Tick += (_, _) => SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                raise.Start();
                Closed += (_, _) => raise.Stop();
            };
            Loaded += (_, _) =>
            {
                Place();
                Animate();
            };
        }

        /// <summary>The work area of the monitor the game is on (the primary one without a game).</summary>
        private Rect Area()
        {
            var window = IntPtr.Zero;
            foreach (var probe in Process.GetProcessesByName("bb-probe"))
            {
                if (probe.MainWindowHandle != IntPtr.Zero) window = probe.MainWindowHandle;
                probe.Dispose();
            }
            var monitor = window != IntPtr.Zero
                ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)
                : MonitorFromPoint(new Point32(), MONITOR_DEFAULTTOPRIMARY);
            var info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
            if (!GetMonitorInfo(monitor, ref info))
                return SystemParameters.WorkArea;

            // Device pixels → this window's units.
            var source = PresentationSource.FromVisual(this);
            var scaleX = source?.CompositionTarget?.TransformFromDevice.M11 ?? 1;
            var scaleY = source?.CompositionTarget?.TransformFromDevice.M22 ?? 1;
            var r = info.Monitor; // the whole monitor: a fullscreen game covers the taskbar too
            return new Rect(r.Left * scaleX, r.Top * scaleY, (r.Right - r.Left) * scaleX, (r.Bottom - r.Top) * scaleY);
        }

        private void Place()
        {
            var area = Area();
            var right = _corner.EndsWith("right");
            var bottom = _corner.StartsWith("bottom");
            Left = right ? area.Right - ActualWidth - 12 : area.Left + 12;
            Top = bottom ? area.Bottom - ActualHeight - 12 : area.Top + 12;
        }

        private void Animate()
        {
            var from = _corner.EndsWith("right") ? 60 : -60;
            var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
            BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(260)));
            _slide.BeginAnimation(TranslateTransform.XProperty, new DoubleAnimation(from, 0, TimeSpan.FromMilliseconds(320)) { EasingFunction = ease });

            var hold = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromSeconds(5.5) };
            hold.Tick += (_, _) =>
            {
                hold.Stop();
                var fade = new DoubleAnimation(1, 0, TimeSpan.FromMilliseconds(380));
                fade.Completed += (_, _) =>
                {
                    Close();
                    Next();
                };
                BeginAnimation(OpacityProperty, fade);
            };
            hold.Start();
        }

        // ---------------------------------------------------------------- Win32

        private const int GWL_EXSTYLE = -20;
        private const int WS_EX_TRANSPARENT = 0x20, WS_EX_TOOLWINDOW = 0x80, WS_EX_LAYERED = 0x80000, WS_EX_NOACTIVATE = 0x8000000;
        private const uint MONITOR_DEFAULTTOPRIMARY = 1, MONITOR_DEFAULTTONEAREST = 2;

        [StructLayout(LayoutKind.Sequential)]
        private struct Point32 { public int X, Y; }

        [StructLayout(LayoutKind.Sequential)]
        private struct Rect32 { public int Left, Top, Right, Bottom; }

        [StructLayout(LayoutKind.Sequential)]
        private struct MonitorInfo
        {
            public int Size;
            public Rect32 Monitor, Work;
            public uint Flags;
        }

        private static readonly IntPtr HWND_TOPMOST = new(-1);
        private const uint SWP_NOSIZE = 0x0001, SWP_NOMOVE = 0x0002, SWP_NOACTIVATE = 0x0010, SWP_SHOWWINDOW = 0x0040;

        [DllImport("user32.dll")]
        private static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int cx, int cy, uint flags);

        [DllImport("user32.dll")]
        private static extern int GetWindowLong(IntPtr window, int index);

        [DllImport("user32.dll")]
        private static extern int SetWindowLong(IntPtr window, int index, int value);

        [DllImport("user32.dll")]
        private static extern IntPtr MonitorFromWindow(IntPtr window, uint flags);

        [DllImport("user32.dll")]
        private static extern IntPtr MonitorFromPoint(Point32 point, uint flags);

        [DllImport("user32.dll")]
        private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
    }
}
