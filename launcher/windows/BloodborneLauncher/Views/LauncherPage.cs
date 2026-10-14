using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;
using TextBlock = System.Windows.Controls.TextBlock;
using TextBox = Wpf.Ui.Controls.TextBox;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// One setting: its title on the left (descriptions in an ⓘ tooltip), its control on the right,
    /// or below it for wide controls such as segmented buttons. Hint is a line under the title for
    /// live status (a path, "not available on this GPU"); it stays hidden until something is set.
    /// </summary>
    public sealed class SettingRow : Border
    {
        public SymbolIcon Icon { get; }
        public TextBlock Hint { get; }

        public bool ShowIcon
        {
            get => Icon.Visibility == Visibility.Visible;
            set => Icon.Visibility = value ? Visibility.Visible : Visibility.Collapsed;
        }

        /// <summary>Classic layout: one card per setting with its icon and description, as before 0.9.4.</summary>
        public static bool Classic { get; set; }

        public SettingRow(SymbolRegular icon, string title, string? info, object? control, bool wide)
        {
            Padding = Classic ? new Thickness(16, 12, 16, 12) : new Thickness(14, 9, 14, 9);
            MinHeight = Classic ? 64 : 42;
            BorderThickness = Classic ? new Thickness(1) : new Thickness(0, 1, 0, 0);
            SetResourceReference(BorderBrushProperty, Classic ? "CardStrokeColorDefaultBrush" : "GlassDividerBrush");
            if (Classic)
            {
                CornerRadius = new CornerRadius(6);
                Margin = new Thickness(0, 0, 0, 4);
                SetResourceReference(BackgroundProperty, "CardBackgroundFillColorDefaultBrush");
            }

            Icon = new SymbolIcon { Symbol = icon, FontSize = Classic ? 20 : 18, Margin = new Thickness(0, 0, Classic ? 16 : 12, 0), VerticalAlignment = VerticalAlignment.Center,
                Visibility = Classic ? Visibility.Visible : Visibility.Collapsed };

            var name = new TextBlock { Text = title, FontSize = 13.5, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
            var titleLine = new StackPanel { Orientation = Orientation.Horizontal };
            titleLine.Children.Add(name);
            if (!string.IsNullOrWhiteSpace(info) && !Classic)
            {
                var mark = new SymbolIcon { Symbol = SymbolRegular.Info16, FontSize = 12, Margin = new Thickness(6, 1, 0, 0), VerticalAlignment = VerticalAlignment.Center, ToolTip = info, Cursor = Cursors.Help };
                mark.SetResourceReference(System.Windows.Documents.TextElement.ForegroundProperty, "TextFillColorTertiaryBrush");
                ToolTipService.SetInitialShowDelay(mark, 150);
                titleLine.Children.Add(mark);
            }

            Hint = new TextBlock { FontSize = 11.5, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 3, 0, 0), Visibility = Visibility.Collapsed };
            Hint.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorTertiaryBrush");

            var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            text.Children.Add(titleLine);
            if (Classic && !string.IsNullOrWhiteSpace(info))
            {
                // Classic shows the description under the title instead of in a tooltip.
                var description = new TextBlock { Text = info, FontSize = 12, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 2, 0, 0) };
                description.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
                text.Children.Add(description);
            }

            var grid = new Grid();
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            grid.Children.Add(Icon);
            Grid.SetColumn(text, 1);
            grid.Children.Add(text);

            if (control is UIElement element)
            {
                if (wide)
                {
                    // Title on its own line, the control across the row, the hint under it.
                    grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                    grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                    if (element is FrameworkElement fe) fe.Margin = new Thickness(0, 8, 0, 0);
                    Grid.SetRow(element, 1);
                    Grid.SetColumn(element, 1);
                    Grid.SetColumnSpan(element, 2);
                    grid.Children.Add(element);
                    grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                    Grid.SetRow(Hint, 2);
                    Grid.SetColumn(Hint, 1);
                    Grid.SetColumnSpan(Hint, 2);
                    grid.Children.Add(Hint);
                }
                else
                {
                    text.Children.Add(Hint);
                    if (element is FrameworkElement fe) fe.Margin = new Thickness(12, 0, 0, 0);
                    Grid.SetColumn(element, 2);
                    grid.Children.Add(element);
                }
            }
            else
                text.Children.Add(Hint);

            Child = grid;
        }
    }

    /// <summary>A row of options where one is chosen, like the PS4/macOS segmented control.</summary>
    public sealed class Segmented : Border
    {
        private readonly List<Border> _items = new();
        private readonly IReadOnlyList<Choice> _choices;
        public int SelectedIndex { get; private set; } = -1;
        public event Action<int>? Changed;

        public Segmented(IReadOnlyList<Choice> choices, Func<Choice, bool>? enabled = null)
        {
            _choices = choices;
            CornerRadius = new CornerRadius(14);
            Padding = new Thickness(3);
            SetResourceReference(BackgroundProperty, "PillTrackBrush");

            var grid = new Grid();
            for (var i = 0; i < choices.Count; i++)
            {
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                var index = i;
                var choice = choices[i];
                var label = new TextBlock
                {
                    Text = Short(choice.Label),
                    FontSize = 12.5,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    VerticalAlignment = VerticalAlignment.Center,
                    TextTrimming = TextTrimming.CharacterEllipsis,
                };
                var item = new Border
                {
                    CornerRadius = new CornerRadius(11),
                    Padding = new Thickness(8, 5, 8, 5),
                    Child = label,
                    Cursor = Cursors.Hand,
                    Background = Brushes.Transparent,
                    ToolTip = choice.Label != Short(choice.Label) || choice.Note != null
                        ? string.Join("\n", new[] { choice.Label, choice.Note }.Where(t => t != null))
                        : null,
                };
                var on = enabled?.Invoke(choice) ?? true;
                item.IsEnabled = on;
                if (!on) label.Opacity = 0.35;
                item.MouseLeftButtonUp += (_, _) => Select(index, notify: true);
                item.MouseEnter += (_, _) => { if (index != SelectedIndex) item.SetResourceReference(BackgroundProperty, "SidebarItemHoverBrush"); };
                item.MouseLeave += (_, _) => { if (index != SelectedIndex) item.Background = Brushes.Transparent; };
                Grid.SetColumn(item, i);
                grid.Children.Add(item);
                _items.Add(item);
            }
            Child = grid;
        }

        /// <summary>"TAA (native anti-aliasing)" → "TAA": the full label is in the tooltip.</summary>
        public static string Short(string label) => label.Split(" (")[0];

        public void Select(int index, bool notify)
        {
            if (index < 0 || index >= _items.Count || !_items[index].IsEnabled) return;
            SelectedIndex = index;
            for (var i = 0; i < _items.Count; i++)
            {
                var label = (TextBlock)_items[i].Child;
                if (i == index)
                {
                    _items[i].SetResourceReference(BackgroundProperty, "SegmentSelectedBrush");
                    label.Foreground = Brushes.White;
                    label.FontWeight = FontWeights.SemiBold;
                }
                else
                {
                    _items[i].Background = Brushes.Transparent;
                    label.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");
                    label.FontWeight = FontWeights.Normal;
                }
            }
            if (notify) Changed?.Invoke(index);
        }
    }

    /// <summary>
    /// A settings page: groups of compact rows, each group one rounded box (Sectionize puts the
    /// rows of a group into it once the page is built). The helpers bind a control straight to a
    /// settings.json or bbport.ini key and save on change.
    /// </summary>
    public abstract class LauncherPage : Page
    {
        protected static Workspace Ws => Workspace.Current;
        protected static LauncherSettings S => Ws.Settings;
        protected static IniFile Ini => Ws.Ini;

        protected StackPanel Body { get; }
        protected ScrollViewer Scroller { get; }
        private bool _sectioned;

        protected LauncherPage(string title, string subtitle)
        {
            Background = Brushes.Transparent;
            SetResourceReference(ForegroundProperty, "TextFillColorPrimaryBrush");
            SetResourceReference(System.Windows.Documents.TextElement.ForegroundProperty, "TextFillColorPrimaryBrush");

            // The tab says which page this is; no page title.
            Body = new StackPanel { Margin = new Thickness(20, 6, 20, 24), MaxWidth = 880 };
            Scroller = new ScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Content = Body };
            Content = Scroller;
            PageScroll.Attach(Scroller);
            if (SettingRow.Classic) Body.MaxWidth = double.PositiveInfinity;
            Loaded += (_, _) =>
            {
                if (_sectioned || SettingRow.Classic) return;
                _sectioned = true;
                Sectionize();
            };
        }

        /// <summary>Called each time the page is shown; refresh anything read from disk.</summary>
        public virtual void OnShown() { }

        /// <summary>
        /// Puts the rows that follow each group header into one rounded box. Rows, panels of rows
        /// and short captions go in; anything else (an info bar, a table, a summary card) ends the box.
        /// </summary>
        private void Sectionize()
        {
            var children = Body.Children.Cast<UIElement>().ToList();
            Body.Children.Clear();
            StackPanel? box = null;
            foreach (var child in children)
            {
                if (child is TextBlock { Tag: "group" })
                {
                    Body.Children.Add(child);
                    box = new StackPanel { Margin = new Thickness(0, -1, 0, 0) };
                    var frame = new Border
                    {
                        CornerRadius = new CornerRadius(16),
                        BorderThickness = new Thickness(1),
                        Child = box,
                    };
                    // Clip to the rounded shape (ClipToBounds is a plain rectangle, which let the
                    // first row's divider run across the rounded corners).
                    frame.SizeChanged += (_, e) =>
                        frame.Clip = new RectangleGeometry(new Rect(0, 0, e.NewSize.Width, e.NewSize.Height), 16, 16);
                    frame.SetResourceReference(Border.BackgroundProperty, "GlassPaneBrush");
                    frame.SetResourceReference(Border.BorderBrushProperty, "GlassPaneStrokeBrush");
                    Body.Children.Add(frame);
                    continue;
                }
                var fits = child is SettingRow || child is StackPanel || child is WrapPanel ||
                           (child is TextBlock && box != null);
                if (box != null && fits)
                {
                    if (child is TextBlock caption) caption.Margin = new Thickness(14, 10, 14, 6);
                    if (child is WrapPanel wrap) wrap.Margin = new Thickness(10, 4, 10, 10);
                    box.Children.Add(child);
                }
                else
                {
                    box = null;
                    Body.Children.Add(child);
                }
            }
        }

        // ---------------------------------------------------------------- building blocks

        public static TextBlock Text(string text, string style)
        {
            var block = new TextBlock { Text = text };
            block.SetResourceReference(StyleProperty, style);
            return block;
        }

        protected TextBlock Group(string text, Panel? into = null)
        {
            var header = Text(text, "GroupHeader");
            header.Tag = "group";
            (into ?? Body).Children.Add(header);
            return header;
        }

        public static SymbolIcon Icon(SymbolRegular symbol) => new() { Symbol = symbol };

        /// <summary>A setting row. info goes in the ⓘ tooltip; hintBlock is a visible status line, empty until set.</summary>
        public static SettingRow Card(SymbolRegular icon, string title, string? info, object? control, out TextBlock hintBlock, bool wide = false)
        {
            var row = new SettingRow(icon, title, info, control, wide);
            hintBlock = row.Hint;
            return row;
        }

        public static void SetHint(TextBlock hint, string? text)
        {
            hint.Text = text ?? "";
            hint.Visibility = string.IsNullOrEmpty(text) ? Visibility.Collapsed : Visibility.Visible;
        }

        protected SettingRow Add(SettingRow row, Panel? into = null)
        {
            (into ?? Body).Children.Add(row);
            return row;
        }

        public static Button SmallButton(string text, SymbolRegular icon, RoutedEventHandler click, ControlAppearance look = ControlAppearance.Secondary)
        {
            var button = new Button
            {
                Content = text,
                Icon = Icon(icon),
                Appearance = look,
                Margin = new Thickness(6, 0, 0, 0),
                Padding = new Thickness(10, 5, 10, 5),
                VerticalAlignment = VerticalAlignment.Center,
            };
            button.Click += click;
            return button;
        }

        public static Button IconButton(SymbolRegular icon, string tooltip, RoutedEventHandler click)
        {
            var button = new Button
            {
                Icon = Icon(icon),
                ToolTip = tooltip,
                Appearance = ControlAppearance.Transparent,
                Padding = new Thickness(6),
                Margin = new Thickness(2, 0, 0, 0),
                VerticalAlignment = VerticalAlignment.Center,
            };
            button.Click += click;
            return button;
        }

        public static StackPanel Row(params UIElement[] children)
        {
            var panel = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
            foreach (var child in children)
                panel.Children.Add(child);
            return panel;
        }

        // ---------------------------------------------------------------- bound rows

        /// <summary>A choice: segmented buttons for a few short options, a drop-down otherwise.
        /// The chosen option's note shows under the row.</summary>
        protected FrameworkElement ComboRow(SymbolRegular icon, string title, string? hint, IReadOnlyList<Choice> choices,
            Func<string> get, Action<string> set, Panel? into = null, double width = 240, Action? changed = null,
            Func<Choice, bool>? enabled = null)
        {
            TextBlock note = null!;
            void ShowNote(int index) => SetHint(note, index >= 0 ? choices[index].Note : null);
            void Picked(int index)
            {
                set(choices[index].Value);
                ShowNote(index);
                Ws.SaveSoon();
                changed?.Invoke();
            }

            var shortLabels = choices.Sum(c => Segmented.Short(c.Label).Length);
            if (!SettingRow.Classic && choices.Count is >= 2 and <= 6 && shortLabels <= 72)
            {
                var segmented = new Segmented(choices, enabled);
                var row = Add(Card(icon, title, hint, segmented, out note, wide: true), into);
                segmented.Select(Choices.IndexOf(choices, get()), notify: false);
                ShowNote(segmented.SelectedIndex);
                segmented.Changed += Picked;
                row.Tag = note;
                return segmented;
            }

            var box = new ComboBox { Width = width, VerticalAlignment = VerticalAlignment.Center };
            foreach (var choice in choices)
                box.Items.Add(new ComboBoxItem { Content = choice.Label, Tag = choice.Value });
            var card = Add(Card(icon, title, hint, box, out note), into);
            box.SelectedIndex = Choices.IndexOf(choices, get());
            ShowNote(box.SelectedIndex);
            box.SelectionChanged += (_, _) => { if (box.SelectedIndex >= 0) Picked(box.SelectedIndex); };
            card.Tag = note;
            return box;
        }

        protected FrameworkElement SettingCombo(SymbolRegular icon, string title, string? hint, IReadOnlyList<Choice> choices,
            string key, Panel? into = null, Action? changed = null) =>
            ComboRow(icon, title, hint, choices, () => S.Str(key), v => S.Set(key, v), into, changed: changed);

        protected FrameworkElement IniCombo(SymbolRegular icon, string title, string? hint, IReadOnlyList<Choice> choices,
            string key, Panel? into = null, Action? changed = null, Func<Choice, bool>? enabled = null) =>
            ComboRow(icon, title, hint, choices, () => Ini.Get(key), v => Ini.Set(key, v), into, changed: changed, enabled: enabled);

        protected ToggleSwitch ToggleRow(SymbolRegular icon, string title, string? hint, Func<bool> get, Action<bool> set,
            Panel? into = null, Action? changed = null)
        {
            var toggle = new ToggleSwitch { IsChecked = get(), VerticalAlignment = VerticalAlignment.Center };
            toggle.Click += (_, _) =>
            {
                set(toggle.IsChecked == true);
                Ws.SaveSoon();
                changed?.Invoke();
            };
            Add(Card(icon, title, hint, toggle, out _), into);
            return toggle;
        }

        protected ToggleSwitch SettingToggle(SymbolRegular icon, string title, string? hint, string key, Panel? into = null, Action? changed = null) =>
            ToggleRow(icon, title, hint, () => S.Bool(key), v => S.Set(key, v), into, changed);

        protected ToggleSwitch IniToggle(SymbolRegular icon, string title, string? hint, string key, Panel? into = null, Action? changed = null) =>
            ToggleRow(icon, title, hint, () => Ini.Flag(key), v => Ini.Set(key, v ? "1" : "0"), into, changed);

        /// <summary>A slider with its value printed beside it.</summary>
        protected Slider SliderRow(SymbolRegular icon, string title, string? hint, double min, double max, double step,
            Func<double> get, Action<double> set, string format = "0.00", Panel? into = null, double valueWidth = 44)
        {
            var slider = new Slider
            {
                Minimum = min,
                Maximum = max,
                SmallChange = step,
                LargeChange = step * 4,
                TickFrequency = step,
                IsSnapToTickEnabled = true,
                Width = 260,
                VerticalAlignment = VerticalAlignment.Center,
                Value = Math.Clamp(get(), min, max),
            };
            var value = new TextBlock
            {
                Width = valueWidth,
                TextAlignment = TextAlignment.Right,
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(0, 0, 10, 0),
            };
            value.SetResourceReference(TextBlock.FontFamilyProperty, "MonoFont");
            value.Text = slider.Value.ToString(format, System.Globalization.CultureInfo.InvariantCulture);
            slider.ValueChanged += (_, _) =>
            {
                value.Text = slider.Value.ToString(format, System.Globalization.CultureInfo.InvariantCulture);
                set(slider.Value);
                Ws.SaveSoon();
            };
            Add(Card(icon, title, hint, Row(value, slider), out _), into);
            return slider;
        }

        /// <summary>A text field row; saves as you type.</summary>
        protected TextBox TextRow(SymbolRegular icon, string title, string? hint, string placeholder,
            Func<string> get, Action<string> set, Panel? into = null, double width = 300)
        {
            var box = new TextBox
            {
                Text = get(),
                PlaceholderText = placeholder,
                PlaceholderEnabled = true,
                Width = width,
                VerticalAlignment = VerticalAlignment.Center,
            };
            box.TextChanged += (_, _) =>
            {
                set(box.Text);
                Ws.SaveSoon();
            };
            Add(Card(icon, title, hint, box, out _), into);
            return box;
        }

        protected static string Fmt(double value) => value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture);

        protected static double ParseDouble(string text, double fallback) =>
            double.TryParse(text, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var v)
                ? v
                : fallback;

        protected static string? PickFolder(string title, string current)
        {
            var dialog = new Microsoft.Win32.OpenFolderDialog { Title = title };
            if (System.IO.Directory.Exists(current))
                dialog.InitialDirectory = current;
            return dialog.ShowDialog() == true ? dialog.FolderName : null;
        }
    }
}
