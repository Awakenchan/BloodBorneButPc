using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// Cheats the engine applies live (bbport.ini cheat_*: off, infinite or an amount kept topped up).
    /// Turning one off restores the game values it changed; the items you have stay.
    /// </summary>
    public sealed class CheatsPage : LauncherPage
    {
        private static readonly Choice[] Modes =
        {
            new("Off", "off"),
            new("Infinite", "infinite", "Kept full at the normal carry limit, even if you have none. Items put into storage while it is on come back"),
            new("Custom amount", "custom", "Refilled to the amount below whenever it drops"),
        };

        /// <summary>Engine builds from before the cheats module: they ignore the cheat_* keys.</summary>
        private static readonly string[] EnginesWithoutCheats =
            { "bbhost-0.4.0-fps1", "bbhost-0.4.0-fps2", "bbhost-0.4.0-fps3", "bbhost-0.4.0-fps4" };

        private readonly InfoBar _engine;

        public CheatsPage() : base("Tweaks & Cheats",
            "Set here for the next start. While playing, change them in the in-game menu (Insert or L3 + R3): they apply live. Turning a cheat off puts the game's values back; items you already have stay.")
        {
            Group(PcText.Get("Gameplay Tweaks"));
            ToggleRow(SymbolRegular.Settings24,PcText.Get("In-game tweak menus"),null,()=>Ini.Get("ingame_tweak_menus","1")!="0",v=>Ini.Set("ingame_tweak_menus",v?"1":"0"));
            IniToggle(SymbolRegular.Person24,PcText.Get("Hunter's dream mirror opens the appearance editor"),null,"change_appearance");
            IniToggle(SymbolRegular.ArrowUndo24,PcText.Get("The Altar of Despair resets level and attributes with the Yharnam Stone"),null,"rebirth");
            Group("Cheats");
            _engine = new InfoBar
            {
                IsClosable = false,
                Severity = InfoBarSeverity.Warning,
                Margin = new Thickness(0, 14, 0, 0),
                Title = "Not in this engine yet",
                Message = "Your engine doesn't apply cheats yet: they arrive with the next engine update. What you pick here is saved and starts working then.",
            };
            Body.Children.Add(_engine);

            Group("Items");
            Cheat(SymbolRegular.Drop24, "Blood Vials", "cheat_vials", "cheat_vials_amount");
            Cheat(SymbolRegular.Flash24, "Quicksilver Bullets", "cheat_bullets", "cheat_bullets_amount");
            IniToggle(SymbolRegular.Beaker24, "Infinite consumables",
                "Molotovs, throwing knives, pebbles, antidotes, papers and the like are not used up (not Blood Vials or Bullets, which are above, and not insight items)",
                "cheat_consumables");

            Group("Character");
            IniToggle(SymbolRegular.Run24, "Infinite stamina", "Your stamina stays full", "cheat_stamina");
            IniToggle(SymbolRegular.EyeOff24, "Stealth", "Enemies don't notice you and your steps make no noise", "cheat_stealth");
            IniToggle(SymbolRegular.Shield24, "No weapon wear", "Equipped weapons are kept at full durability, so no repairs", "cheat_no_durability");
            ComboRow(SymbolRegular.Target24, "Firearm range", "How far firearm bullets travel (pistols, rifles, blunderbuss, cannons)",
                new[] { new Choice("Normal", "off"), new Choice("150%", "150"), new Choice("200%", "200"), new Choice("300%", "300") },
                () => Ini.Get("cheat_gun_range", "off"), v => Ini.Set("cheat_gun_range", v));

            Group("Blood Echoes");
            IniToggle(SymbolRegular.Heart24, "Keep echoes on death", "Dying never drops your Blood Echoes, so there is nothing to pick back up", "cheat_keep_echoes");
            ComboRow(SymbolRegular.ArrowTrendingLines24, "Echoes from kills", "Enemies already loaded give the new amount after you rest at a lamp or reload",
                new[] { new Choice("Normal", "1"), new Choice("x2", "2"), new Choice("x5", "5"), new Choice("x10", "10") },
                () => Ini.Get("cheat_echo_multiplier", "1"), v => Ini.Set("cheat_echo_multiplier", v));
            Add(Card(SymbolRegular.Add24, "Add echoes or insight", "In the in-game menu (Insert or L3 + R3): +1,000, +10,000, +100,000 echoes and +1 insight", null, out _));
        }

        /// <summary>A mode row and its amount slider for one cheat key.</summary>
        private void Cheat(SymbolRegular icon, string title, string key, string amountKey)
        {
            Slider amount = null!;
            string Mode() => Ini.Get(key, "off") switch
            {
                "infinite" => "infinite",
                var v when int.TryParse(v, out var n) && n > 0 => "custom",
                _ => "off",
            };
            int Amount() => Math.Clamp(int.TryParse(Ini.Get(key, ""), out var n) && n > 0 ? n : S.Int(amountKey), 1, 99);

            ComboRow(icon, title, null, Modes, Mode, v =>
            {
                Ini.Set(key, v == "custom" ? Amount().ToString(CultureInfo.InvariantCulture) : v);
                ShowAmount(amount, v == "custom");
                Ws.Log($"{title}: {(v == "custom" ? $"kept at {Amount()}" : v)}.", LogLevel.Step);
            });
            amount = SliderRow(SymbolRegular.NumberSymbol24, "Amount", $"How many {title} to keep", 1, 99, 1, () => Amount(), v =>
            {
                S.Set(amountKey, (int)v);
                if (Mode() == "custom") Ini.Set(key, ((int)v).ToString(CultureInfo.InvariantCulture));
            }, "0");
            ShowAmount(amount, Mode() == "custom");
        }

        /// <summary>The amount row only shows for Custom amount.</summary>
        private static void ShowAmount(Slider amount, bool show)
        {
            DependencyObject? at = amount;
            while (at != null && at is not SettingRow) at = LogicalTreeHelper.GetParent(at);
            ((UIElement?)at ?? amount).Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        }

        /// <summary>The package's engine applies cheats (newer than the builds listed above).</summary>
        public static bool Supported => (Updater.LocalEngine() is { } engine && !EnginesWithoutCheats.Contains(engine)) || System.IO.File.Exists(System.IO.Path.Combine(Paths.PortDir,"out","better-kbm-build.json"));

        public override void OnShown() => _engine.IsOpen = !Supported;
    }
}
