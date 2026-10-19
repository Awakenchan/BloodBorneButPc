#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Deploy the transitive PE runtime imports from the compiler's DLL directory.

Run with the same CLANG64 installation used to build the engine. Existing DLLs
in the output directory are preserved and their imports are inspected as well.
Windows system libraries and API sets are supplied by Windows itself.
"""
from pathlib import Path
from collections import deque
import argparse
import os
import shutil
import struct
import sys


SYSTEM_DLLS = set("""
advapi32 avrt bcrypt cfgmgr32 comctl32 comdlg32 crypt32 d3d11 d3d12 dbghelp
dnsapi dsound dwmapi dxcore dxgi gdi32 hid imm32 iphlpapi kernel32 msvcrt
mswsock ncrypt normaliz ntdll ole32 oleaut32 powrprof propsys psapi rpcrt4
secur32 setupapi shell32 shlwapi ucrtbase user32 userenv version winhttp
wininet winmm wintrust ws2_32 wtsapi32
""".split())


def imports(path):
    """Read ordinary PE imports without loading or executing the image."""
    data = path.read_bytes()
    def value(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise ValueError(f"Truncated PE image: {path.name}")
        return struct.unpack_from(fmt, data, offset)[0]
    if data[:2] != b"MZ":
        raise ValueError(f"Not a Windows PE image: {path.name}")
    pe = value("<I", 0x3c)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"Invalid PE signature: {path.name}")
    if value("<H", pe + 4) != 0x8664:
        raise ValueError(f"Expected an x64 runtime library: {path.name}")
    section_count = value("<H", pe + 6)
    optional_size = value("<H", pe + 20)
    optional = pe + 24
    if value("<H", optional) != 0x20b or optional_size < 128:
        raise ValueError(f"Invalid PE32+ optional header: {path.name}")
    if value("<I", optional + 108) < 2:
        return []
    import_rva = value("<I", optional + 120)
    if not import_rva:
        return []
    sections = []
    for i in range(section_count):
        header = optional + optional_size + i * 40
        sections.append((value("<I", header + 12), value("<I", header + 16),
                         value("<I", header + 20)))
    header_size = value("<I", optional + 60)
    def offset(rva):
        if 0 <= rva < min(header_size, len(data)):
            return rva
        for start, size, raw in sections:
            if start <= rva < start + size and raw + rva - start < len(data):
                return raw + rva - start
        raise ValueError(f"Invalid import RVA in {path.name}: {rva:#x}")
    table = offset(import_rva)
    result = []
    for i in range(4096):
        entry = table + i * 20
        if value("<I", entry) == 0 and data[entry:entry + 20] == bytes(20):
            return result
        name_at = offset(value("<I", entry + 12))
        end = data.find(b"\0", name_at, min(len(data), name_at + 4096))
        if end < 0:
            raise ValueError(f"Unterminated import name in {path.name}")
        name = data[name_at:end].decode("ascii")
        if not name or name != Path(name).name or "/" in name or "\\" in name:
            raise ValueError(f"Invalid import name in {path.name}")
        result.append(name)
    raise ValueError(f"Unterminated import table in {path.name}")


def deploy(output, dll_dir):
    output, dll_dir = output.resolve(), dll_dir.resolve()
    if not output.is_dir() or not dll_dir.is_dir():
        raise ValueError("The output and compiler DLL directories must already exist.")
    source = {p.name.casefold(): p for p in dll_dir.iterdir()
              if p.is_file() and p.suffix.casefold() == ".dll"}
    available = {p.name.casefold(): p for p in output.iterdir() if p.is_file()}
    roots = [p for p in available.values() if p.suffix.casefold() in {".exe", ".dll"}]
    if not roots:
        raise ValueError("No Windows executables or libraries were found in the output directory.")
    queue, seen, copied, missing = deque(roots), set(), [], {}
    windows = os.environ.get("SystemRoot") or os.environ.get("SYSTEMROOT") or os.environ.get("WINDIR")
    system = Path(windows) / "System32" if windows else None
    while queue:
        image = queue.popleft()
        key = image.name.casefold()
        if key in seen:
            continue
        seen.add(key)
        for name in imports(image):
            key = name.casefold()
            if key.startswith(("api-ms-win-", "ext-ms-win-")) or Path(key).stem in SYSTEM_DLLS:
                continue
            if key in available:
                queue.append(available[key])
            elif key in source:
                target = output / source[key].name
                temporary = target.with_name(target.name + ".deploy.tmp")
                shutil.copyfile(source[key], temporary)
                os.replace(temporary, target)
                available[key] = target
                copied.append(target.name)
                print(f"Copied runtime: {target.name}")
                queue.append(target)
            elif system and (system / name).is_file():
                continue
            else:
                missing.setdefault(name, set()).add(image.name)
    if missing:
        details = "\n".join(f"  {name} (required by {', '.join(sorted(users))})"
                            for name, users in sorted(missing.items()))
        raise ValueError("Runtime libraries were not found in the output, CLANG64, or Windows system directories:\n" + details)
    print(f"Runtime DLL deployment complete: {len(copied)} copied; {len(seen)} PE images checked.")
    return copied


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="The engine's out directory")
    parser.add_argument("--dll-dir", required=True, type=Path, help="The matching compiler's runtime directory, normally /clang64/bin")
    args = parser.parse_args()
    try:
        deploy(args.output, args.dll_dir)
    except (OSError, ValueError, UnicodeError, struct.error) as error:
        print(f"Runtime deployment failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
