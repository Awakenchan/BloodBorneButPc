// SPDX-License-Identifier: GPL-3.0-or-later
namespace BloodborneLauncher.Core;

public static class GameLanguages {
    // Keep the previous selection for both channels until the user changes either one.
    // The legacy language key now owns voices; text_language owns all game text.
    public static void Ensure(LauncherSettings settings) {
        if (settings.Str("text_language").Length == 0)
            settings.Set("text_language", ValidText(settings.Str("language")));
    }
    private static string ValidText(string value) => value is "0" or "1" or "2" or "3" or "4" or "5" or "8" or "17" or "20" ? value : "1";
    public static string Text(LauncherSettings settings) => ValidText(
        settings.Str("text_language").Length > 0 ? settings.Str("text_language") : settings.Str("language"));
    public static string Voice(LauncherSettings settings) => settings.Str("language") switch {
        "0" or "1" or "2" or "3" or "4" or "5" or "17" or "20" => settings.Str("language"), _ => "1"
    };
    public static string MenuLocale(string text) => text switch {
        "0" => "ja", "2" => "fr", "3" or "20" => "es", "4" => "de", "5" => "it", "8" => "ru", "17" => "pt_BR", _ => "en"
    };
}
