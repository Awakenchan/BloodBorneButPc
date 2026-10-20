using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Effects;
using System.Windows.Media.Imaging;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;
using TextBlock = System.Windows.Controls.TextBlock;
using TextBox = Wpf.Ui.Controls.TextBox;

namespace BloodborneLauncher.Views
{
    /// <summary>A small modal dialog in the launcher's style: a title, text, optional fields, and two buttons.</summary>
    public sealed class Prompt : Window
    {
        public sealed record Field(string Label, string Value = "", string Placeholder = "", bool Multiline = false);

        private readonly List<TextBox> _boxes = new();

        /// <summary>Choose: the index of the button pressed; -1 when closed with Escape.</summary>
        private int _chosen = -1;

        private Prompt(string title, string? message, IReadOnlyList<Field> fields, string ok, bool showCancel, string[]? choices = null)
        {
            var owner = Application.Current.MainWindow is { IsLoaded: true } main && main.WindowState != WindowState.Minimized ? main : null;
            Owner = owner;
            // Crisp text, as the main window's (a window does not inherit them).
            System.Windows.Media.TextOptions.SetTextFormattingMode(this, System.Windows.Media.TextFormattingMode.Display);
            System.Windows.Media.TextOptions.SetTextHintingMode(this, System.Windows.Media.TextHintingMode.Fixed);
            UseLayoutRounding = true;
            WindowStyle = WindowStyle.None;
            ResizeMode = ResizeMode.NoResize;
            AllowsTransparency = true;
            Background = Brushes.Transparent;
            ShowInTaskbar = false;

            var stack = new StackPanel();
            var heading = LauncherPage.Text(title, "PageTitle");
            stack.Children.Add(heading);
            if (!string.IsNullOrEmpty(message))
            {
                var text = LauncherPage.Text(message, "Muted");
                text.FontSize = 13;
                text.Margin = new Thickness(0, 8, 0, 4);
                text.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
                // A long message (an import's list of changes) scrolls instead of pushing the buttons
                // out of a small window.
                stack.Children.Add(new ScrollViewer
                {
                    Content = text,
                    MaxHeight = 300,
                    VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
                    HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
                });
            }
            foreach (var field in fields)
            {
                var label = LauncherPage.Text(field.Label, "StatLabel");
                label.Margin = new Thickness(0, 12, 0, 4);
                stack.Children.Add(label);
                var box = new TextBox
                {
                    Text = field.Value,
                    PlaceholderText = field.Placeholder,
                    PlaceholderEnabled = true,
                    AcceptsReturn = field.Multiline,
                    TextWrapping = field.Multiline ? TextWrapping.Wrap : TextWrapping.NoWrap,
                    MinHeight = field.Multiline ? 70 : 0,
                    // A pasted code is one long line: it wraps and scrolls inside the box, which keeps
                    // the dialog's buttons on screen in a small window.
                    MaxHeight = field.Multiline ? 160 : double.PositiveInfinity,
                    VerticalScrollBarVisibility = field.Multiline ? ScrollBarVisibility.Auto : ScrollBarVisibility.Hidden,
                };
                _boxes.Add(box);
                stack.Children.Add(box);
            }

            // Two equal-width buttons along the bottom, the action on the right.
            var buttons = new UniformGrid { Rows = 1, Margin = new Thickness(0, 22, 0, 0), HorizontalAlignment = HorizontalAlignment.Right };
            if (choices != null)
            {
                // Several actions: equal widths, the last one the main action.
                buttons.HorizontalAlignment = HorizontalAlignment.Stretch;
                for (var i = 0; i < choices.Length; i++)
                {
                    var index = i;
                    var last = i == choices.Length - 1;
                    var button = new Button
                    {
                        Content = choices[i],
                        Margin = new Thickness(0, 0, last ? 0 : 8, 0),
                        HorizontalAlignment = HorizontalAlignment.Stretch,
                        Appearance = last ? ControlAppearance.Primary : ControlAppearance.Secondary,
                        IsDefault = last,
                    };
                    button.Click += (_, _) => { _chosen = index; DialogResult = true; };
                    buttons.Children.Add(button);
                }
            }
            else if (showCancel)
            {
                var cancel = new Button { Content = L.T("Cancel"), Margin = new Thickness(0, 0, 8, 0), IsCancel = true, MinWidth = 110, HorizontalAlignment = HorizontalAlignment.Stretch };
                cancel.Click += (_, _) => DialogResult = false;
                buttons.Children.Add(cancel);
            }
            if (choices == null)
            {
                var accept = new Button { Content = ok, Appearance = ControlAppearance.Primary, IsDefault = true, MinWidth = 110, HorizontalAlignment = HorizontalAlignment.Stretch };
                accept.Click += (_, _) => DialogResult = true;
                buttons.Children.Add(accept);
            }
            stack.Children.Add(buttons);

            var card = new Border
            {
                Width = 440,
                Padding = new Thickness(24, 20, 24, 20),
                CornerRadius = new CornerRadius(16),
                BorderThickness = new Thickness(1),
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
                Child = stack,
                Effect = new DropShadowEffect { BlurRadius = 40, ShadowDepth = 8, Direction = 270, Opacity = 0.55, Color = Colors.Black },
                RenderTransformOrigin = new Point(0.5, 0.5),
                RenderTransform = new ScaleTransform(0.96, 0.96),
            };
            card.SetResourceReference(Border.BackgroundProperty, "DialogBackgroundBrush");
            card.SetResourceReference(Border.BorderBrushProperty, "DialogStrokeBrush");
            SetResourceReference(ForegroundProperty, "TextFillColorPrimaryBrush");

            if (owner != null)
            {
                // Cover the launcher with a dimmed layer and float the card in the middle of it.
                WindowStartupLocation = WindowStartupLocation.Manual;
                var scrim = new Border { CornerRadius = new CornerRadius(8), Child = card };
                scrim.SetResourceReference(Border.BackgroundProperty, "DialogScrimBrush");
                Content = scrim;
                CoverOwner(owner);
            }
            else
            {
                WindowStartupLocation = WindowStartupLocation.CenterScreen;
                SizeToContent = SizeToContent.WidthAndHeight;
                card.Margin = new Thickness(24);
                card.MouseLeftButtonDown += (_, e) => { if (e.OriginalSource is Border or StackPanel) DragMove(); };
                Content = card;
            }

            Opacity = 0;
            if (SnapshotPath != null) ShowActivated = false;
            Loaded += (_, _) =>
            {
                if (SnapshotPath != null && owner != null) Snapshot(owner, card);
                var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
                var time = TimeSpan.FromMilliseconds(140);
                BeginAnimation(OpacityProperty, new DoubleAnimation(1, time) { EasingFunction = ease });
                var scale = (ScaleTransform)card.RenderTransform;
                scale.BeginAnimation(ScaleTransform.ScaleXProperty, new DoubleAnimation(1, time) { EasingFunction = ease });
                scale.BeginAnimation(ScaleTransform.ScaleYProperty, new DoubleAnimation(1, time) { EasingFunction = ease });

                if (_boxes.Count > 0)
                {
                    _boxes[0].Focus();
                    _boxes[0].SelectAll();
                }
            };
            PreviewKeyDown += (_, e) => { if (e.Key == Key.Escape) DialogResult = false; };
        }

        /// <summary>--test-prompt &lt;png&gt;: where to save a picture of the first dialog over the launcher, then close.</summary>
        internal static string? SnapshotPath;

        private void Snapshot(Window owner, Border card)
        {
            var timer = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
            timer.Tick += (_, _) =>
            {
                timer.Stop();
                var width = (int)owner.ActualWidth;
                var height = (int)owner.ActualHeight;
                var shot = new RenderTargetBitmap(width, height, 96, 96, PixelFormats.Pbgra32);
                shot.Render((Visual)owner.Content);
                var layer = new DrawingVisual();
                using (var dc = layer.RenderOpen())
                    dc.DrawRectangle(new VisualBrush((Visual)Content), null, new Rect(0, 0, width, height));
                shot.Render(layer);
                var png = new PngBitmapEncoder();
                png.Frames.Add(BitmapFrame.Create(shot));
                using (var file = System.IO.File.Create(SnapshotPath!))
                    png.Save(file);
                DialogResult = false;
                Application.Current.Shutdown();
            };
            timer.Start();
        }

        /// <summary>Places this window exactly over the owner's client area (in device-independent units).</summary>
        private void CoverOwner(Window owner)
        {
            var source = PresentationSource.FromVisual(owner);
            var toDip = source?.CompositionTarget?.TransformFromDevice ?? Matrix.Identity;
            var topLeft = toDip.Transform(owner.PointToScreen(new Point(0, 0)));
            Left = topLeft.X;
            Top = topLeft.Y;
            Width = owner.ActualWidth;
            Height = owner.ActualHeight;
        }

        /// <summary>The fields' values, or null if cancelled.</summary>
        public static string[]? Ask(string title, string? message, params Field[] fields)
        {
            var dialog = new Prompt(title, message, fields, L.T("OK"), showCancel: true);
            return dialog.ShowDialog() == true ? dialog._boxes.Select(b => b.Text).ToArray() : null;
        }

        public static bool Confirm(string title, string message, string ok) =>
            new Prompt(title, message, Array.Empty<Field>(), ok, showCancel: true).ShowDialog() == true;

        /// <summary>One of several actions (the last is the main one): its index, or -1 if dismissed.</summary>
        public static int Choose(string title, string message, params string[] choices)
        {
            var dialog = new Prompt(title, message, Array.Empty<Field>(), "", showCancel: false, choices);
            dialog.ShowDialog();
            return dialog._chosen;
        }

        public static void Tell(string title, string message) =>
            new Prompt(title, message, Array.Empty<Field>(), L.T("OK"), showCancel: false).ShowDialog();
    }
}
