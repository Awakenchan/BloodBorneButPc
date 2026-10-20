using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using Wpf.Ui.Controls;
using TextBox = Wpf.Ui.Controls.TextBox;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// Type-to-find for drop-downs. Every list jumps to the entry that starts with what is typed.
    /// Long lists (more than 8 entries) also get a filter box at the top of the open list: typing
    /// (with the list open or closed) hides the entries that don't contain the text, Enter picks the
    /// first one left, Down goes into the list, Esc clears the filter. Only what is shown changes:
    /// an entry is chosen the same way as before, so what is saved does not change.
    /// </summary>
    public static class ComboSearch
    {
        public const int FilterFrom = 9;

        private sealed class State
        {
            public TextBox? Filter;
            public bool Injected;
            public string Pending = "";
        }

        private static readonly ConditionalWeakTable<ComboBox, State> States = new();

        public static void Attach(ComboBox box)
        {
            if (box.IsEditable || States.TryGetValue(box, out _)) return;
            States.Add(box, new State());
            box.IsTextSearchEnabled = true;
            box.GotKeyboardFocus += (_, _) => Label(box);
            box.DropDownOpened += (_, _) => Opened(box);
            box.DropDownClosed += (_, _) => Closed(box);
            box.PreviewTextInput += (_, e) => Typed(box, e);
        }

        private static bool Long(ComboBox box) => box.Items.Count >= FilterFrom;

        /// <summary>The text an entry is found by (its label), for the built-in jump to an entry.</summary>
        private static void Label(ComboBox box)
        {
            foreach (var item in box.Items.OfType<ComboBoxItem>())
                if (string.IsNullOrEmpty(TextSearch.GetText(item)))
                    TextSearch.SetText(item, Text(item));
        }

        private static string Text(ComboBoxItem item) => item.Content as string ?? item.Content?.ToString() ?? "";

        private static void Typed(ComboBox box, TextCompositionEventArgs e)
        {
            if (!Long(box) || string.IsNullOrEmpty(e.Text) || char.IsControl(e.Text[0])) return;
            var state = States.GetValue(box, _ => new State());
            if (state.Filter != null && ReferenceEquals(e.OriginalSource, state.Filter)) return; // typing in the filter itself
            if (state.Filter != null && state.Filter.IsKeyboardFocusWithin) return;
            e.Handled = true;
            if (!box.IsDropDownOpen)
            {
                state.Pending += e.Text;
                box.IsDropDownOpen = true;
                return;
            }
            if (state.Filter == null) return;
            state.Filter.Text += e.Text;
            FocusFilter(state.Filter);
        }

        private static void Opened(ComboBox box)
        {
            Label(box);
            var state = States.GetValue(box, _ => new State());
            if (!Long(box))
            {
                state.Pending = "";
                return;
            }
            if (!state.Injected)
            {
                state.Injected = true;
                state.Filter = Inject(box);
            }
            if (state.Filter == null)
            {
                state.Pending = "";
                return;
            }
            var filter = state.Filter;
            filter.Text = state.Pending;
            state.Pending = "";
            Apply(box, filter.Text);
            // After the list has focused its chosen entry.
            box.Dispatcher.BeginInvoke(DispatcherPriority.ContextIdle, () => { if (box.IsDropDownOpen) FocusFilter(filter); });
        }

        private static void Closed(ComboBox box)
        {
            if (!States.TryGetValue(box, out var state)) return;
            state.Pending = "";
            if (state.Filter != null && state.Filter.Text.Length > 0) state.Filter.Text = "";
            Apply(box, "");
        }

        private static void FocusFilter(TextBox filter)
        {
            filter.Focus();
            Keyboard.Focus(filter);
            filter.CaretIndex = filter.Text.Length;
        }

        /// <summary>Shows the entries containing every word typed, hides the rest.</summary>
        private static void Apply(ComboBox box, string text)
        {
            var words = text.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            foreach (var item in box.Items.OfType<ComboBoxItem>())
            {
                var label = Text(item);
                var show = words.All(w => label.Contains(w, StringComparison.OrdinalIgnoreCase));
                item.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
            }
        }

        private static ComboBoxItem? FirstShown(ComboBox box) =>
            box.Items.OfType<ComboBoxItem>().FirstOrDefault(i => i.Visibility == Visibility.Visible && i.IsEnabled);

        /// <summary>Puts a filter box above the list in the drop-down's popup. Null when the
        /// template has no list to put it over (the drop-down then only jumps to entries).</summary>
        private static TextBox? Inject(ComboBox box)
        {
            box.ApplyTemplate();
            if (box.Template?.FindName("PART_Popup", box) is not Popup { Child: { } root }) return null;
            var list = Find<ScrollViewer>(root) ?? (FrameworkElement?)Find<ItemsPresenter>(root);
            if (list == null) return null;

            var filter = new TextBox
            {
                PlaceholderText = L.T("Type to filter"),
                PlaceholderEnabled = true,
                ClearButtonEnabled = true,
                Icon = new SymbolIcon { Symbol = SymbolRegular.Search24 },
                Margin = new Thickness(6, 6, 6, 4),
                MinHeight = 0,
                Padding = new Thickness(8, 5, 8, 5),
            };
            var host = new DockPanel { LastChildFill = true };
            DockPanel.SetDock(filter, Dock.Top);

            var parent = LogicalTreeHelper.GetParent(list) ?? VisualTreeHelper.GetParent(list);
            switch (parent)
            {
                case Decorator decorator:
                    decorator.Child = null;
                    host.Children.Add(filter);
                    host.Children.Add(list);
                    decorator.Child = host;
                    break;
                case Panel panel:
                {
                    var index = panel.Children.IndexOf(list);
                    if (index < 0) return null;
                    panel.Children.RemoveAt(index);
                    foreach (var dp in new[] { Grid.RowProperty, Grid.ColumnProperty, Grid.RowSpanProperty, Grid.ColumnSpanProperty })
                        host.SetValue(dp, list.GetValue(dp));
                    host.Margin = list.Margin;
                    list.Margin = new Thickness(0);
                    host.Children.Add(filter);
                    host.Children.Add(list);
                    panel.Children.Insert(index, host);
                    break;
                }
                case ContentControl content when ReferenceEquals(content.Content, list):
                    content.Content = null;
                    host.Children.Add(filter);
                    host.Children.Add(list);
                    content.Content = host;
                    break;
                default:
                    return null;
            }

            filter.TextChanged += (_, _) => Apply(box, filter.Text);
            filter.PreviewKeyDown += (_, e) =>
            {
                switch (e.Key)
                {
                    case Key.Enter:
                        // Nothing typed: keep the choice as it is.
                        if (filter.Text.Trim().Length > 0 && FirstShown(box) is { } first) box.SelectedItem = first;
                        box.IsDropDownOpen = false;
                        e.Handled = true;
                        break;
                    case Key.Down:
                        if (FirstShown(box) is { } item)
                        {
                            item.Focus();
                            item.BringIntoView();
                        }
                        e.Handled = true;
                        break;
                    case Key.Escape when filter.Text.Length > 0:
                        filter.Text = "";
                        e.Handled = true;
                        break;
                }
            };
            return filter;
        }

        private static T? Find<T>(DependencyObject root) where T : DependencyObject
        {
            if (root is T hit) return hit;
            foreach (var child in LogicalTreeHelper.GetChildren(root).OfType<DependencyObject>())
                if (Find<T>(child) is { } found) return found;
            if (root is Visual)
                for (var i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
                    if (Find<T>(VisualTreeHelper.GetChild(root, i)) is { } found) return found;
            return null;
        }
    }
}
