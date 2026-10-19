#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run from MSYS2 CLANG64. Reuses your existing Git clone and preserves its build.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
engine_source="${1:-$root/../BetterKBM-engine}"
engine_target="${2:-$root/../BetterKBM-engine-expanded}"
tag="v0.4.0-darkizuku-preview.1"
patch="$root/engine/bbhost-changes.patch"
if [[ ${MSYSTEM:-} != CLANG64 ]]; then
    printf '%s\n' 'Open MSYS2 CLANG64 and run this script there.' >&2; exit 1
fi
for executable in git cmake ninja clang python3 cygpath; do
    command -v "$executable" >/dev/null || { printf 'Missing dependency: %s\n' "$executable" >&2; exit 1; }
done
dotnet_command="$(command -v dotnet.exe || command -v dotnet || true)"
if [[ -z "$dotnet_command" && -x '/c/Program Files/dotnet/dotnet.exe' ]]; then
    dotnet_command='/c/Program Files/dotnet/dotnet.exe'
fi
[[ -n "$dotnet_command" ]] || { printf '%s\n' 'Install a Windows .NET SDK (8 or newer), or add its dotnet.exe directory to PATH.' >&2; exit 1; }
git -C "$engine_source" rev-parse --git-dir >/dev/null || { printf 'Not a Git engine clone: %s\n' "$engine_source" >&2; exit 1; }
# The tag is the engine base recorded by the author in engine/README.md.
git -C "$engine_source" rev-parse --verify "${tag}^{commit}" >/dev/null
if [[ ! -e "$engine_target" ]]; then
    git -C "$engine_source" worktree add --detach "$engine_target" "$tag"
fi
engine_target="$(cd -- "$engine_target" && pwd)"
expected="$(git -C "$engine_source" rev-parse "${tag}^{commit}")"
actual="$(git -C "$engine_target" rev-parse HEAD)"
[[ "$expected" == "$actual" ]] || { printf '%s\n' 'Target has a different base. Choose a new empty target directory as argument 2.' >&2; exit 1; }
git -C "$engine_target" submodule update --init --recursive
if git -C "$engine_target" apply --reverse --check --ignore-space-change "$patch" 2>/dev/null; then
    printf '%s\n' 'This integration is already applied; rebuilding it.'
elif git -C "$engine_target" apply --check --ignore-space-change "$patch" 2>/dev/null; then
    git -C "$engine_target" apply --whitespace=nowarn --ignore-space-change "$patch"
else
    # Try recognized prior releases without resetting settings or local sources.
    upgraded=0
    latest="$root/engine/better-kbm-voice-bank-update.patch"
    voice="$root/engine/better-kbm-voice-track-update.patch"
    latino="$root/engine/better-kbm-latino-update.patch"
    language="$root/engine/better-kbm-language-update.patch"
    wheel="$root/engine/better-kbm-wheel-update.patch"
    device="$root/engine/better-kbm-device-update.patch"
    tuning="$root/engine/better-kbm-tuning-update.patch"
    sprint="$root/engine/better-kbm-sprint-update.patch"
    motion="$root/engine/better-kbm-motion-update.patch"
    popup="$root/engine/better-kbm-popup-input-update.patch"
    previous="$root/engine/better-kbm-image-camera-update.patch"
    for upgrade in "$latest" "$voice" "$latino" "$language" "$wheel" "$device" "$tuning" "$sprint" "$motion" "$popup" "$previous" "$root/engine/better-kbm-panel-update.patch" "$root/engine/better-kbm-menu-update.patch"; do
        [[ -f "$upgrade" ]] || continue
        git -C "$engine_target" apply --check --ignore-space-change "$upgrade" 2>/dev/null || continue
        git -C "$engine_target" apply --whitespace=nowarn --ignore-space-change "$upgrade"
        applied=("$upgrade")
        for followup in "$previous" "$popup" "$motion" "$sprint" "$tuning" "$device" "$wheel" "$language" "$latino" "$voice" "$latest"; do
            if git -C "$engine_target" apply --reverse --check --ignore-space-change "$patch" 2>/dev/null; then break; fi
            if [[ "$followup" != "$upgrade" && -f "$followup" ]] &&
               git -C "$engine_target" apply --check --ignore-space-change "$followup" 2>/dev/null; then
                git -C "$engine_target" apply --whitespace=nowarn --ignore-space-change "$followup"
                applied+=("$followup")
            fi
        done
        if git -C "$engine_target" apply --reverse --check --ignore-space-change "$patch" 2>/dev/null; then
            upgraded=1; break
        fi
        for ((i=${#applied[@]}-1;i>=0;i--)); do
            git -C "$engine_target" apply --reverse --whitespace=nowarn "${applied[i]}"
        done
    done
    if [[ "$upgraded" != 1 ]]; then
        printf '%s\n' 'Target differs from recognized integrations. No upgrade was kept; choose a new empty target directory as argument 2.' >&2; exit 1
    fi
    printf '%s\n' 'Previous BetterKBM integration upgraded; existing settings preserved.'
fi

cp -- "$root/engine/win_compat.h" "$engine_target/gpu/win_compat.h"
# Avoid Ninja regeneration loops if an archive or checkout had future dates.
python3 - "$engine_target" <<'PY'
from pathlib import Path
import os,sys,time,subprocess
root=Path(sys.argv[1]);now=time.time()
names=subprocess.check_output(['git','-C',str(root),'ls-files','-z','--cached','--others','--exclude-standard']).decode().split('\0')
for name in names:
    p=root/name
    if p.is_file() and p.stat().st_mtime>now:os.utime(p,(now,now))
PY
# Native CMake/Clang needs C:/..., not /c/..., inside CXXFLAGS.
compat_windows="$(cygpath -m "$engine_target/gpu/win_compat.h")"
export CFLAGS="${CFLAGS:-} -Wno-error=unused-but-set-global"
export CXXFLAGS="-include \"$compat_windows\" ${BB_EXTRA_CXXFLAGS:-}"
(
    cd -- "$engine_target"
    cmake --fresh -S gpu -B out/gpu -G Ninja
    bash build.sh
)
[[ -f "$engine_target/out/bb-probe.exe" && -f "$engine_target/out/bb-engine-assets.exe" ]] || {
    printf '%s\n' 'Expected native executables were not produced.' >&2; exit 1;
}
# A source build must also deploy the DLLs needed outside the CLANG64 shell.
python3 "$root/tools/deploy-runtime-dlls.py" "$engine_target/out" --dll-dir /clang64/bin
python3 - "$engine_target/out/better-kbm-build.json" <<'PY'
from pathlib import Path
import json,sys
Path(sys.argv[1]).write_text(json.dumps({'integration':'better-kbm-expanded','version':14}),encoding='utf-8')
PY
# ZIP extraction can preserve dates older than existing WPF build outputs.
# Clean first so a successful publish cannot silently reuse the previous UI.
"$dotnet_command" clean "$root/launcher/windows/BloodborneLauncher/BloodborneLauncher.csproj" \
    -c Release -r win-x64
# Keep all publish output outside the tracked source files.
"$dotnet_command" publish "$root/launcher/windows/BloodborneLauncher/BloodborneLauncher.csproj" \
    -c Release -r win-x64 --self-contained true \
    -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -o "$root/LauncherApp"
printf '\nLauncher: %s\nEngine folder: %s\n' \
    "$(cygpath -w "$root/LauncherApp/Bloodborne Launcher.exe")" "$(cygpath -w "$engine_target")"
printf '%s\n' 'In the launcher: More > Advanced > Bloodborne PC > Engine folder. Select the engine folder printed above.'
