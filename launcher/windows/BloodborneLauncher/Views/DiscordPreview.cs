using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>How the Discord status will look on a profile ("Playing" card), drawn from the same
    /// <see cref="DiscordPresence.Look"/> the launcher sends.</summary>
    public sealed class DiscordPreview : Border
    {
        private static readonly Brush Card = Frozen(Color.FromRgb(0x23, 0x24, 0x28));
        private static readonly Brush Muted = Frozen(Color.FromRgb(0xB5, 0xBA, 0xC1));
        private static readonly Brush Bright = Frozen(Color.FromRgb(0xF2, 0xF3, 0xF5));
        private static readonly Brush Green = Frozen(Color.FromRgb(0x3B, 0xA5, 0x5C));
        private static readonly Brush ButtonFill = Frozen(Color.FromRgb(0x4E, 0x50, 0x58));
        private static readonly ImageSource Lantern = LoadLantern();

        private readonly Image _picture = new() { Width = 72, Height = 72, Stretch = Stretch.UniformToFill };
        private readonly Image _badge = new() { Width = 24, Height = 24, Stretch = Stretch.UniformToFill };
        private readonly Border _badgeFrame;
        private readonly TextBlock _details = Line(Bright, 13), _state = Line(Bright, 13), _timer = Line(Green, 12);
        private readonly TextBlock _name = new() { Foreground = Bright, FontWeight = FontWeights.Bold, FontSize = 15 };
        private readonly StackPanel _buttons = new() { Margin = new Thickness(0, 10, 0, 0) };

        public DiscordPreview()
        {
            Background = Card;
            CornerRadius = new CornerRadius(8);
            Padding = new Thickness(14);
            Margin = new Thickness(0, 6, 0, 10);
            MaxWidth = 420;
            HorizontalAlignment = HorizontalAlignment.Left;
            _badgeFrame = new Border
            {
                Width = 28, Height = 28, CornerRadius = new CornerRadius(14), Background = Card,
                HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom,
                Margin = new Thickness(0, 0, -6, -6),
                Child = new Border { Width = 24, Height = 24, CornerRadius = new CornerRadius(12), ClipToBounds = true, Child = _badge },
            };
            var picture = new Grid { Width = 72, Height = 72, VerticalAlignment = VerticalAlignment.Top };
            picture.Children.Add(new Border { CornerRadius = new CornerRadius(8), ClipToBounds = true, Child = _picture });
            picture.Children.Add(_badgeFrame);
            var text = new StackPanel { Margin = new Thickness(14, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
            text.Children.Add(_name);
            text.Children.Add(_details);
            text.Children.Add(_state);
            text.Children.Add(_timer);
            var row = new DockPanel();
            DockPanel.SetDock(picture, Dock.Left);
            row.Children.Add(picture);
            row.Children.Add(text);
            var body = new StackPanel();
            body.Children.Add(new TextBlock { Text = L.T("Playing"), Foreground = Muted, FontSize = 12, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 0, 0, 8) });
            body.Children.Add(row);
            body.Children.Add(_buttons);
            Child = body;
        }

        public void Show(DiscordPresence.Look? look)
        {
            Visibility = look == null ? Visibility.Collapsed : Visibility.Visible;
            if (look == null) return;
            _name.Text = look.Name;
            _picture.Source = look.Image == "logo" ? Lantern : look.Image.Length > 0 ? FromLink(look.Image) : null;
            _picture.ToolTip = look.ImageText;
            _badgeFrame.Visibility = look.Badge.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
            _badge.Source = look.Badge.Length > 0 ? FromLink(look.Badge) : null;
            _badge.ToolTip = look.BadgeText;
            Set(_details, look.Details);
            Set(_state, look.State);
            _timer.Text = "▶ 1:16";
            _timer.Visibility = look.Timer ? Visibility.Visible : Visibility.Collapsed;
            _buttons.Children.Clear();
            _buttons.Visibility = look.Buttons.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
            foreach (var (label, url) in look.Buttons)
                _buttons.Children.Add(new Border
                {
                    Background = ButtonFill, CornerRadius = new CornerRadius(4), Padding = new Thickness(0, 6, 0, 6),
                    Margin = new Thickness(0, 0, 0, 6), ToolTip = url,
                    Child = new TextBlock { Text = label, Foreground = Bright, FontSize = 13, HorizontalAlignment = HorizontalAlignment.Center },
                });
        }

        private static void Set(TextBlock block, string text)
        {
            block.Text = text;
            block.Visibility = text.Length >= 2 ? Visibility.Visible : Visibility.Collapsed;
        }

        private static TextBlock Line(Brush brush, double size) =>
            new() { Foreground = brush, FontSize = size, TextTrimming = TextTrimming.CharacterEllipsis, Margin = new Thickness(0, 2, 0, 0) };

        private static ImageSource? FromLink(string url)
        {
            try
            {
                // Downloaded by WPF in the background; the card fills in when it arrives.
                return new BitmapImage(new Uri(url, UriKind.Absolute));
            }
            catch (Exception)
            {
                return null;
            }
        }

        private static ImageSource LoadLantern()
        {
            var image = new BitmapImage(new Uri("pack://application:,,,/Assets/discord-logo.png", UriKind.Absolute));
            image.Freeze();
            return image;
        }

        private static Brush Frozen(Color color)
        {
            var brush = new SolidColorBrush(color);
            brush.Freeze();
            return brush;
        }
    }
}
