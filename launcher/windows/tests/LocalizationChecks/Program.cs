using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Windows.Input;
using BloodborneLauncher.Core;

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

// No App/Workspace instance, settings.Load/Save, game process or network access.
Require(!Localization.IsChinese && Localization.T("Settings") == "Settings", "English must be the default");
Localization.Initialize("unknown");
Require(Localization.T("Settings") == "Settings", "Unknown languages must fall back to English");
var language = args.FirstOrDefault() ?? "en";
Localization.Initialize(language);
var chinese = language == "zh-CN";
Require(Localization.T("Settings") == (chinese ? "设置" : "Settings"), "Language selection failed");
Require(Localization.T("untranslated fallback") == "untranslated fallback", "Missing-key fallback failed");

var assembly = typeof(Localization).Assembly;
using var resource = assembly.GetManifestResourceStream("BloodborneLauncher.Localization.zh-CN.json")!;
var catalog = JsonSerializer.Deserialize<Dictionary<string, string>>(resource)!;
static string[] Slots(string text) => Regex.Matches(text, @"(?<!\{)\{\d+(?:,[^}:]+)?(?::[^}]+)?\}(?!\})")
    .Select(m => m.Value).Order(StringComparer.Ordinal).ToArray();
foreach (var (english, translation) in catalog)
{
    Require(Slots(english).SequenceEqual(Slots(translation)), "Changed format placeholders: " + english);
    Require(!string.IsNullOrWhiteSpace(translation), "Empty translation: " + english);
    if (english.Contains('|'))
    {
        // File-dialog filters must retain their patterns, regardless of their display names.
        var original = english.Split('|');
        var localized = translation.Split('|');
        Require(original.Length == localized.Length, "Changed filter structure: " + english);
        for (var i = 1; i < original.Length; i += 2)
            Require(original[i] == localized[i], "Changed filter pattern: " + english);
    }
}
CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("de-DE");
var value = 12.5;
Require(Localization.F($"Unknown format {value:0.0}") == "Unknown format 12,5", "Formatting must retain the current culture");
Require(Localization.F($"There is already a profile called {"测试玩家"}.").Contains("测试玩家"), "Dynamic Unicode text was lost");

Require(Choices.MenuLanguages.Count(c => c.Value == "zh_CN") == 1, "Chinese menu code missing or duplicated");
Require(Choices.GameLanguages.Select(c => c.Value).SequenceEqual(new[] { "1", "8", "0", "2", "3", "4", "5" }), "Game language values changed");
Require(InputMap.SdlKeyName(Key.Left) == "Left" && InputMap.SdlKeyName(Key.Return) == "Return", "Keyboard protocol names were translated");
Require(InputMap.Controls.Single(c => c.Name == "switch_right").Default == "Right", "Default keyboard binding was translated");
Require(PadMap.InputFor(InputMap.Controls.Single(c => c.Name == "switch_item")) == "down", "Controller lookup labels changed");
foreach (var profile in ControlProfiles.BuiltIns)
{
    var roundtrip = ControlProfiles.Parse(ControlProfiles.ToJson(profile)).Profile;
    Require(roundtrip.Name == profile.Name, "Profile name changed on round-trip");
    Require(roundtrip.Bindings.OrderBy(x => x.Key).SequenceEqual(profile.Bindings.OrderBy(x => x.Key)), "Bindings changed on round-trip");
}
Require(PlayerProfiles.Main == "Main", "Saved main-profile identifier changed");
Console.WriteLine($"PASS {language}: {catalog.Count} translations, placeholders, filters, language codes and input/profile round-trips; no game started.");
