using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// The title bar's settings search. Nothing is listed by hand: it reads the rows every page put
    /// in with LauncherPage.Add (title, description, status line and a choice's options), so a new
    /// setting is found as soon as it is on a page. Each page keeps the list of its rows as it is
    /// built (LauncherPage.SearchRows), so a search only matches text: no tree walk, no page made.
    /// The window builds the pages in the background once the search box is focused and searches
    /// 150 ms after the last key (MainWindow.ShowHits).
    /// </summary>
    public static class SettingSearch
    {
        /// <summary>A page to look through: its name in the results and how to bring it up.</summary>
        public sealed record Place(string Name, LauncherPage Page, Action Show);

        /// <summary>A setting found. Snippet: the text that matched when it is not the title.</summary>
        public sealed record Hit(SettingRow Row, Place Place, string? Snippet, int Score);

        public const int MaxHits = 8;

        public static List<Hit> Find(IEnumerable<Place> places, string query)
        {
            query = query.Trim();
            var words = query.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            var hits = new List<Hit>();
            if (words.Length == 0) return hits;
            foreach (var place in places)
                foreach (var row in place.Page.SearchRows())
                    if (Match(row, place, query, words) is { } hit)
                        hits.Add(hit);
            // OrderByDescending keeps the page order among equal scores.
            return hits.OrderByDescending(h => h.Score).Take(MaxHits).ToList();
        }

        /// <summary>
        /// The title starting with the query ranks first, then the query at the start of a word in the
        /// title, then anywhere in it, then every word in the title. Words found only in the
        /// description, status line, options, group or page rank last, with that text shown.
        /// </summary>
        private static Hit? Match(SettingRow row, Place place, string query, string[] words)
        {
            var title = row.Title;
            var at = title.IndexOf(query, StringComparison.OrdinalIgnoreCase);
            if (at == 0) return new Hit(row, place, null, 100);
            if (at > 0) return new Hit(row, place, null, char.IsLetterOrDigit(title[at - 1]) ? 60 : 80);

            var info = row.Info ?? "";
            var hint = row.Hint.Visibility == Visibility.Visible ? row.Hint.Text : "";
            // The fixed text first (no string built per row and query); the live hint only when needed.
            if (!words.All(w => Has(title, w) || Has(info, w) || Has(hint, w) || Has(row.Choices, w) ||
                                Has(row.Section ?? "", w) || Has(place.Name, w)))
                return null;
            var inTitle = words.Count(w => Has(title, w));
            if (inTitle == words.Length) return new Hit(row, place, null, 50);
            bool Shows(string text) => words.Any(w => !Has(title, w) && Has(text, w));
            var snippet = Shows(info) ? info : Shows(hint) ? hint : Shows(row.Choices) ? L.T("Options: ") + row.Choices : null;
            return new Hit(row, place, snippet, 10 + 10 * inTitle);
        }

        private static bool Has(string text, string word) => text.Contains(word, StringComparison.OrdinalIgnoreCase);

        /// <summary>The logical tree under root, parents before children, in page order. Rows hold no
        /// rows, and a Frame's page is looked through as a place of its own, so neither is entered.</summary>
        public static IEnumerable<DependencyObject> Descendants(DependencyObject root)
        {
            foreach (var child in LogicalTreeHelper.GetChildren(root).OfType<DependencyObject>())
            {
                yield return child;
                if (child is SettingRow or Frame) continue;
                foreach (var below in Descendants(child))
                    yield return below;
            }
        }

        /// <summary>One line of the results: the title with its page and group, and the matched text under it.</summary>
        public static Border View(Hit hit)
        {
            var title = new TextBlock { Text = hit.Row.Title, FontSize = 12.5, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
            title.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");
            var where = new TextBlock
            {
                // "Graphics › Experimental"; a group named like its page ("More › Mods") is not said twice.
                Text = hit.Row.Section is { Length: > 0 } section && !hit.Place.Name.EndsWith(section, StringComparison.Ordinal)
                    ? $"{hit.Place.Name} › {section}"
                    : hit.Place.Name,
                FontSize = 11,
                Margin = new Thickness(12, 0, 0, 0),
                VerticalAlignment = VerticalAlignment.Center,
            };
            where.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorTertiaryBrush");

            var top = new Grid();
            top.ColumnDefinitions.Add(new ColumnDefinition());
            top.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            top.Children.Add(title);
            Grid.SetColumn(where, 1);
            top.Children.Add(where);

            var stack = new StackPanel();
            stack.Children.Add(top);
            if (hit.Snippet is { Length: > 0 } snippet)
            {
                var line = new TextBlock { Text = snippet.ReplaceLineEndings(" "), FontSize = 11, TextTrimming = TextTrimming.CharacterEllipsis, Margin = new Thickness(0, 2, 0, 0) };
                line.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorTertiaryBrush");
                stack.Children.Add(line);
            }
            return new Border
            {
                Child = stack,
                Tag = hit,
                Padding = new Thickness(10, 6, 10, 6),
                CornerRadius = new CornerRadius(6),
                Background = Brushes.Transparent,
                Cursor = Cursors.Hand,
            };
        }

        /// <summary>The line shown when nothing matches.</summary>
        public static TextBlock Nothing(string query)
        {
            var text = LauncherPage.Text(L.F($"No setting matches \"{query.Trim()}\"."), "Faint");
            text.Margin = new Thickness(10, 7, 10, 7);
            return text;
        }
    }
}
