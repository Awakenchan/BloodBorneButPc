using System.IO;
using System.Runtime.InteropServices;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// The engine's own SDL3 (out\SDL3.dll in the package), used to see controllers exactly as the
    /// game does: the same names, the same button mapping (gamecontrollerdb.txt included) and the
    /// same button names for pad.* lines. Only used while the game is not running, so the two
    /// never hold a pad at the same time.
    /// </summary>
    public static class SdlPad
    {
        public sealed record Device(string Name, bool Supported);

        private const uint InitGamepad = 0x2000;
        private const int ButtonCount = 26, AxisLeftTrigger = 4, AxisRightTrigger = 5;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.U1)]
        private delegate bool SetHintFn([MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPUTF8Str)] string value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.U1)]
        private delegate bool InitFn(uint flags);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void QuitFn(uint flags);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr ListFn(out int count);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void FreeFn(IntPtr p);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr ById(uint id);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.U1)]
        private delegate bool IsById(uint id);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void VoidFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.U1)]
        private delegate bool ButtonFn(IntPtr pad, int button);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate short AxisFn(IntPtr pad, int axis);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr ButtonNameFn(int button);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void CloseFn(IntPtr pad);

        private static bool _loaded, _failed, _initialised;
        private static SetHintFn? _setHint;
        private static InitFn? _init;
        private static QuitFn? _quit;
        private static ListFn? _gamepads, _joysticks;
        private static FreeFn? _free;
        private static ById? _open, _nameForId, _joystickNameForId;
        private static IsById? _isGamepad;
        private static VoidFn? _update;
        private static ButtonFn? _button;
        private static AxisFn? _axis;
        private static ButtonNameFn? _buttonName;
        private static CloseFn? _close;
        private static readonly Dictionary<uint, IntPtr> Open = new();

        /// <summary>False when the package has no SDL3.dll (the launcher falls back to its own detection).</summary>
        public static bool Available => Load();

        private static bool Load()
        {
            if (_loaded || _failed) return _loaded;
            try
            {
                var dll = Path.Combine(Paths.BinariesDir, "SDL3.dll");
                if (!File.Exists(dll)) { _failed = true; return false; }
                var lib = NativeLibrary.Load(dll);
                T Get<T>(string name) where T : Delegate => Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(lib, name));
                _setHint = Get<SetHintFn>("SDL_SetHint");
                _init = Get<InitFn>("SDL_InitSubSystem");
                _quit = Get<QuitFn>("SDL_QuitSubSystem");
                _gamepads = Get<ListFn>("SDL_GetGamepads");
                _joysticks = Get<ListFn>("SDL_GetJoysticks");
                _free = Get<FreeFn>("SDL_free");
                _open = Get<ById>("SDL_OpenGamepad");
                _nameForId = Get<ById>("SDL_GetGamepadNameForID");
                _joystickNameForId = Get<ById>("SDL_GetJoystickNameForID");
                _isGamepad = Get<IsById>("SDL_IsGamepad");
                _update = Get<VoidFn>("SDL_UpdateGamepads");
                _button = Get<ButtonFn>("SDL_GetGamepadButton");
                _axis = Get<AxisFn>("SDL_GetGamepadAxis");
                _buttonName = Get<ButtonNameFn>("SDL_GetGamepadStringForButton");
                _close = Get<CloseFn>("SDL_CloseGamepad");
                _loaded = true;
            }
            catch
            {
                _failed = true;
            }
            return _loaded;
        }

        private static bool Start()
        {
            if (_initialised) return true;
            if (!Load()) return false;
            // The same hints the launcher gives the game (GameEnvironment).
            foreach (var (key, value) in GameEnvironment.SdlHints())
                _setHint!(key, value);
            _setHint!("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
            _initialised = _init!(InitGamepad);
            return _initialised;
        }

        /// <summary>Lets go of every pad (before the game starts, or when the page closes).</summary>
        public static void Stop()
        {
            if (!_initialised) return;
            foreach (var pad in Open.Values) _close!(pad);
            Open.Clear();
            _quit!(InitGamepad);
            _initialised = false;
        }

        private static List<uint> Ids(ListFn list)
        {
            var ids = new List<uint>();
            var array = list(out var count);
            if (array == IntPtr.Zero) return ids;
            for (var i = 0; i < count; i++) ids.Add((uint)Marshal.ReadInt32(array, i * 4));
            _free!(array);
            return ids;
        }

        /// <summary>The controllers connected now; Supported is false for one SDL sees but cannot map.</summary>
        public static List<Device>? Devices()
        {
            if (!Start()) return null;
            _update!();
            var devices = new List<Device>();
            foreach (var id in Ids(_joysticks!))
            {
                var supported = _isGamepad!(id);
                var name = Marshal.PtrToStringUTF8(supported ? _nameForId!(id) : _joystickNameForId!(id));
                devices.Add(new Device(string.IsNullOrWhiteSpace(name) ? L.T("Game controller") : name!, supported));
            }
            return devices;
        }

        /// <summary>The pads' state in coarse form (held buttons, stick positions in eighths). A receiver
        /// stays connected while its pad is switched off, so only a change in this shows the pad is on.</summary>
        public static string State()
        {
            var held = Held();
            var state = new System.Text.StringBuilder(string.Join(",", held.OrderBy(h => h)));
            foreach (var pad in Open.Values)
                for (var axis = 0; axis < 4; axis++)
                    state.Append('|').Append(_axis!(pad, axis) / 8192);
            return state.ToString();
        }

        /// <summary>The buttons held now on any pad, as SDL names (a, leftshoulder, …, lefttrigger, righttrigger).</summary>
        public static HashSet<string> Held()
        {
            var held = new HashSet<string>();
            if (!Start()) return held;
            _update!();
            var ids = Ids(_gamepads!);
            foreach (var gone in Open.Keys.Except(ids).ToList())
            {
                _close!(Open[gone]);
                Open.Remove(gone);
            }
            foreach (var id in ids)
            {
                if (!Open.TryGetValue(id, out var pad))
                {
                    pad = _open!(id);
                    if (pad == IntPtr.Zero) continue;
                    Open[id] = pad;
                }
                for (var b = 0; b < ButtonCount; b++)
                    if (_button!(pad, b) && Marshal.PtrToStringUTF8(_buttonName!(b)) is { Length: > 0 } name)
                        held.Add(name);
                if (_axis!(pad, AxisLeftTrigger) > 16000) held.Add("lefttrigger");
                if (_axis!(pad, AxisRightTrigger) > 16000) held.Add("righttrigger");
            }
            return held;
        }
    }
}
