using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Windows.Markup;

namespace BloodborneLauncher.Core;

/// <summary>Presentation-only translations; configuration keys and values remain unchanged.</summary>
public static class Localization
{
    private static readonly Dictionary<string, string> Chinese = Load();
    public static bool IsChinese { get; private set; }

    private static Dictionary<string, string> Load()
    {
        using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("BloodborneLauncher.Localization.zh-CN.json")
            ?? throw new InvalidOperationException("Missing Chinese translation resource.");
        return JsonSerializer.Deserialize<Dictionary<string, string>>(stream)!;
    }

    /// <summary>Called before constructing the UI. English is the default and fallback.</summary>
    public static void Initialize(string language) => IsChinese = language == "zh-CN";

    public static void Initialize()
    {
        Initialize("en");
        try
        {
            using var settings = JsonDocument.Parse(File.ReadAllText(Paths.ConfigFile));
            if (settings.RootElement.ValueKind == JsonValueKind.Object &&
                settings.RootElement.TryGetProperty("ui_language", out var language) &&
                language.ValueKind == JsonValueKind.String)
                Initialize(language.GetString()!);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            // The regular settings loader handles missing or damaged files; do not migrate or save here.
        }
    }

    public static string T(string text) => IsChinese && Chinese.TryGetValue(text, out var translated) ? translated : text;
    public static string F(FormattableString text) => string.Format(CultureInfo.CurrentCulture, T(text.Format), text.GetArguments());
}

[MarkupExtensionReturnType(typeof(string))]
public sealed class TrExtension(string text) : MarkupExtension
{
    public override object ProvideValue(IServiceProvider serviceProvider) => Localization.T(text);
}
