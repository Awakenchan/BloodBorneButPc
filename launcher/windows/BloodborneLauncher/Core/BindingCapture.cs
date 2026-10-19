namespace BloodborneLauncher.Core;

/// <summary>Accumulate a binding until every participating input is released.</summary>
public sealed class BindingCapture
{
    private readonly HashSet<string> _initial = new(StringComparer.OrdinalIgnoreCase);
    private readonly HashSet<string> _held = new(StringComparer.OrdinalIgnoreCase);
    private readonly List<string> _tokens = new();

    public IReadOnlyList<string> Tokens => _tokens;

    public void Begin(IEnumerable<string> initiallyHeld)
    {
        Clear();
        _initial.UnionWith(initiallyHeld);
    }

    public void Clear()
    {
        _initial.Clear();
        _held.Clear();
        _tokens.Clear();
    }

    public void Press(string token)
    {
        // Ignore only the opening gesture, not every new input until a timer ticks.
        if (_initial.Contains(token)) return;
        _held.Add(token);
        if (!_tokens.Contains(token, StringComparer.OrdinalIgnoreCase)) _tokens.Add(token);
    }

    public void Release(string token)
    {
        _initial.Remove(token);
        _held.Remove(token);
    }

    public void RefreshHeld(IEnumerable<string> physicallyHeld)
    {
        var current = new HashSet<string>(physicallyHeld, StringComparer.OrdinalIgnoreCase);
        _initial.IntersectWith(current);
        _held.IntersectWith(current);
    }

    public string[]? TakeCompleted()
    {
        if (_tokens.Count == 0 || _held.Count != 0 || _initial.Count != 0) return null;
        var result = _tokens.ToArray();
        Clear();
        return result;
    }
}
