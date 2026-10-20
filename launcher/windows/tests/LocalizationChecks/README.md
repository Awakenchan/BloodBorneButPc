# Localization checks

On Windows with the .NET 8 Desktop SDK, run from the repository root:

```powershell
dotnet run --project launcher/windows/tests/LocalizationChecks -- en
dotnet run --project launcher/windows/tests/LocalizationChecks -- zh-CN
```

Each language runs in a fresh process so the launcher's static choice lists are
initialized in that language. The checks cover English defaults and fallback,
embedded translations, format placeholders, file-dialog filter patterns, menu
codes, keyboard/controller identifiers, and control-profile round trips. They
do not load or save user settings, launch the game, or contact update servers.

For visual review, select **Settings > Appearance > Launcher language > 简体中文**
and restart. Check both layouts, the window buttons, settings search, dialogs,
and **Display > Language > In-game settings menu language**. The game menu uses
`zh_CN`; its font and build steps are documented in `engine/README.md`.
