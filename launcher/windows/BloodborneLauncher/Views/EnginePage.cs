using System.Windows;
using System.Windows.Controls;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using TextBlock = System.Windows.Controls.TextBlock;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// The engine's user-facing environment switches as controls, built from Core/EngineSwitches.cs,
    /// so nobody has to type BB_ variables. A switch left at its default is not passed to the game
    /// at all. Every row goes in with Add, so the settings search finds it (by title, description
    /// or variable name).
    /// </summary>
    public sealed class EnginePage : LauncherPage
    {
        private readonly List<Action> _resets = new();
        private readonly TextBlock _changedHint;

        public EnginePage() : base(L.T("Engine"), "")
        {
            var intro = Text(L.T("Each switch stays at the engine's default until you change it. Only changed ones are used."), "Faint");
            intro.ToolTip = L.T("Only the switches you change are passed to the game, when it starts; a start in safe mode (after a crash) ") +
                            L.T("leaves them out. Settings › Diagnostics › Extra variables is still there for anything not listed, and wins over these.");
            Body.Children.Add(intro);
            Add(Card(SymbolRegular.ArrowReset24, L.T("Changed switches"), L.T("Every switch on this page that is not at its default"),
                SmallButton(L.T("Reset all"), SymbolRegular.ArrowUndo24, (_, _) => ResetAll()), out _changedHint));

            foreach (var group in EngineSwitches.Groups.Where(g => EngineSwitches.All.Any(x => x.Group == g)))
            {
                Group(L.T(group));
                foreach (var item in EngineSwitches.All.Where(x => x.Group == group))
                    AddSwitch(item);
            }
            ShowChanged();
        }

        public override void OnShown() => ShowChanged();

        private static SymbolRegular GroupIcon(string group) => group switch
        {
            EngineSwitches.Graphics => SymbolRegular.Sparkle24,
            EngineSwitches.Performance => SymbolRegular.TopSpeed24,
            EngineSwitches.Memory => SymbolRegular.Storage24,
            EngineSwitches.Upscalers => SymbolRegular.ZoomIn24,
            EngineSwitches.Online => SymbolRegular.Globe24,
            EngineSwitches.Input => SymbolRegular.XboxController24,
            EngineSwitches.Sound => SymbolRegular.Speaker224,
            EngineSwitches.Fixes => SymbolRegular.ShieldCheckmark24,
            EngineSwitches.Diagnostics => SymbolRegular.TextBulletListSquare24,
            _ => SymbolRegular.Bug24,
        };

        /// <summary>One switch's row: a toggle, a slider, a choice or a text field, bound to its env: setting.</summary>
        private void AddSwitch(EngineSwitch item)
        {
            var icon = GroupIcon(item.Group);
            var info = EngineSwitches.Describe(item);
            switch (item.Kind)
            {
                case SwitchKind.Toggle:
                {
                    var toggle = ToggleRow(icon, item.Title, info, () => EngineSwitches.Get(S, item) == item.On,
                        v => EngineSwitches.Set(S, item, v ? item.On : item.Off), changed: ShowChanged);
                    _resets.Add(() => toggle.IsChecked = item.Default == item.On);
                    break;
                }
                case SwitchKind.Number:
                {
                    var format = item.Unit.Length > 0 ? $"{item.Format}' {item.Unit}'" : item.Format;
                    var slider = SliderRow(icon, item.Title, info, item.Min, item.Max, item.Step,
                        () => EngineSwitches.NumberOf(S, item),
                        v =>
                        {
                            EngineSwitches.Set(S, item, EngineSwitches.Format(item, v));
                            ShowChanged();
                        },
                        format, valueWidth: item.Unit.Length > 0 ? 86 : 52);
                    _resets.Add(() => slider.Value = EngineSwitches.DefaultNumber(item));
                    break;
                }
                case SwitchKind.Text:
                {
                    var box = TextRow(icon, item.Title, info, item.Placeholder, () => EngineSwitches.Get(S, item),
                        v =>
                        {
                            EngineSwitches.Set(S, item, v);
                            ShowChanged();
                        });
                    _resets.Add(() => box.Text = "");
                    break;
                }
                default:
                {
                    var control = ComboRow(icon, item.Title, info, item.Options, () => EngineSwitches.Get(S, item),
                        v => EngineSwitches.Set(S, item, v), changed: ShowChanged);
                    _resets.Add(() =>
                    {
                        var index = Choices.IndexOf(item.Options, item.Default);
                        if (control is Segmented segmented) segmented.Select(index, notify: true);
                        else if (control is ComboBox box) box.SelectedIndex = index;
                    });
                    break;
                }
            }
        }

        /// <summary>The summary row: what is passed to the game, and what Extra variables override.</summary>
        private void ShowChanged()
        {
            if (_changedHint == null) return; // rows built before the summary row exists
            var changed = EngineSwitches.Changed(S).ToList();
            var extra = GameEnvironment.ExtraVariables(S).Select(e => e.Name).ToHashSet(StringComparer.Ordinal);
            var overridden = changed.Where(c => extra.Contains(c.Name)).Select(c => c.Name).ToList();
            var text = changed.Count == 0
                ? L.T("None: the engine runs with its own defaults")
                : L.F($"{changed.Count} passed to the game: ") + string.Join(", ", changed.Select(c => $"{c.Name}={c.Value}"));
            if (overridden.Count > 0)
                text += L.F($"\nExtra variables also set {string.Join(", ", overridden)}: their value is used");
            SetHint(_changedHint, text);
        }

        private void ResetAll()
        {
            if (!EngineSwitches.Changed(S).Any()) return;
            if (!Prompt.Confirm(L.T("Reset engine switches"), L.T("Every switch on this page goes back to the engine's default."), L.T("Reset all"))) return;
            EngineSwitches.ResetAll(S);
            foreach (var reset in _resets)
                reset();
            Ws.SaveSoon();
            ShowChanged();
        }
    }
}
