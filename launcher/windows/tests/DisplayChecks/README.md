Run on Windows with the .NET 8 SDK:

```powershell
dotnet run --project launcher/windows/tests/DisplayChecks -c Release
```

Checks saved device selection after rearranging displays, negative coordinates, missing displays,
primary-display fallback, default selection, and automatic output dimensions. No settings are saved.

For manual validation, select a secondary HDR display in Display > Screen > Monitor and restart the
game with Windows HDR and HDR output enabled. Check windowed and fullscreen modes, then disconnect
the selected display and verify primary-display fallback. Move the HDR sliders to both ends and
verify that `hdr_paper_white` (10–500) and `hdr_peak` (10–4000) persist after restarting the launcher.
