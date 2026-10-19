using BloodborneLauncher.Core;

static void Expect(bool condition, string scenario)
{
    if (!condition) throw new InvalidOperationException(scenario);
}

static void ExpectTokens(BindingCapture capture, params string[] expected)
{
    Expect(capture.TakeCompleted() is { } actual && actual.SequenceEqual(expected),
        "Capture must complete once, with the accumulated tokens in press order.");
    Expect(capture.TakeCompleted() == null, "Completed input must not be saved twice.");
}

var capture = new BindingCapture();

// No neutral timer tick is needed between opening and the first key press.
capture.Begin(Array.Empty<string>());
capture.Press("E");
Expect(capture.TakeCompleted() == null, "A held key must not commit.");
capture.Release("E");
ExpectTokens(capture, "E");

capture.Begin(Array.Empty<string>());
capture.Press("Left Shift");
capture.Press("Mouse3");
capture.Press("Mouse3");
capture.Release("Left Shift");
Expect(capture.TakeCompleted() == null, "Releasing one chord input must not commit.");
capture.Press("E");
capture.Release("Mouse3");
Expect(capture.TakeCompleted() == null, "Inputs added while another remains held join the chord.");
capture.Release("E");
ExpectTokens(capture, "Left Shift", "Mouse3", "E");

// The click/Enter used to open the dialog never becomes part of the binding.
capture.Begin(new[] { "Mouse1", "Return" });
capture.Press("Mouse1");
capture.Press("T");
capture.Release("T");
Expect(capture.TakeCompleted() == null, "Opening inputs must finish releasing.");
capture.Release("Mouse1");
capture.Release("Return");
ExpectTokens(capture, "T");

capture.Begin(Array.Empty<string>());
capture.Press("Left Ctrl");
capture.Press("WheelDown");
capture.Release("WheelDown");
capture.RefreshHeld(new[] { "Left Ctrl" });
Expect(capture.TakeCompleted() == null, "A wheel pulse must retain held modifiers.");
capture.RefreshHeld(Array.Empty<string>());
ExpectTokens(capture, "Left Ctrl", "WheelDown");

capture.Begin(Array.Empty<string>());
capture.Press("Mouse1");
capture.RefreshHeld(Array.Empty<string>());
ExpectTokens(capture, "Mouse1");

capture.Begin(new[] { "Left Shift" });
capture.RefreshHeld(Array.Empty<string>());
capture.Press("Left Shift");
capture.Release("Left Shift");
ExpectTokens(capture, "Left Shift");

capture.Begin(Array.Empty<string>());
capture.Press("E");
capture.Clear();
Expect(capture.TakeCompleted() == null, "Cancel/focus loss must discard pending input.");
capture.Begin(Array.Empty<string>());
capture.Press("R");
capture.Release("R");
ExpectTokens(capture, "R");

Console.WriteLine("PASS: immediate first key, held/released chords, repeated inputs, opening gesture, wheel, release fallback and cancellation.");
