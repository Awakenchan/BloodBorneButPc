#!/usr/bin/env python3
"""Lantern Engine: every executable address the frame-rate presets write (for fps_constants.py).

Usage: fps_patched.py out/eboot.elf [uncap|60|90|120|144] > patched.txt
One address per line: each byte a preset's writes (frame-rate patch, corrections, sprint fix) touch.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import patches as P

elf = Path(sys.argv[1])
fps = sys.argv[2] if len(sys.argv) > 2 else 'uncap'
xml = Path(__file__).resolve().parents[2] / 'patches' / 'Bloodborne.xml'
elf_bytes = elf.read_bytes()
segments = P.eboot_segments(elf_bytes)
preset = P.FPS_PRESETS[fps] if fps in P.FPS_PRESETS else [P.SCALED_BASE, 'Sprint Fix (High FPS)']
writes = P.compile_patches(xml, preset, '01.09', segments)
if fps in P.SCALED_FPS:
    writes = P.rate_scaled(writes, P.SCALED_FPS[fps])
writes = P.corrected_frame_rate(writes, {"60": 60, "90": 90, **P.SCALED_FPS}.get(fps), elf_bytes)
covered = set()
for offset, data in writes:
    covered.update(range(offset, offset + len(data)))
for address in sorted(covered):
    print(hex(address))
