#!/usr/bin/env python3
"""Lantern Engine: where the game's code still assumes 30 frames a second.

Usage: fps_constants.py out/eboot.elf [patched-addresses.txt]

Finds the float constants of a 30 fps frame in the data (1/30, 30.0, 1/60 and 60.0 as written by
the 60 fps patches' style), then every instruction that reads one RIP-relative (movss/vmovss,
mulss/divss/addss/subss and their VEX forms, cvt, comiss) and every `mov dword [..], imm32` that
stores one. Each site is printed with its function (from .eh_frame_hdr) and whether a frame-rate
patch already writes it (a list of addresses, one per line, from patches.py).
"""
import bisect
import struct
import sys

CONSTANTS = {
    struct.pack('<f', 1 / 30): '1/30',
    struct.pack('<f', 30.0): '30.0',
    struct.pack('<d', 1 / 30): '1/30 (double)',
    struct.pack('<d', 30.0): '30.0 (double)',
}


def load(path):
    f = open(path, 'rb').read()
    phoff, = struct.unpack_from('<Q', f, 0x20)
    phnum, = struct.unpack_from('<H', f, 0x38)
    segs = [struct.unpack_from('<IIQQQQQQ', f, phoff + 56 * i) for i in range(phnum)]
    loads = [s for s in segs if s[0] == 1]
    ehh = next(s for s in segs if s[0] == 0x6474e550)
    funcs = []
    text = next(s for s in loads if s[1] & 1)
    toff, tva = text[2], text[3]
    h, hva = ehh[2], ehh[3]
    count, = struct.unpack_from('<i', f, h + 8)
    for i in range(count):
        loc, fde = struct.unpack_from('<ii', f, h + 12 + 8 * i)
        fde_off = hva + fde - tva + toff
        size, = struct.unpack_from('<i', f, fde_off + 12)
        funcs.append((hva + loc, size))
    funcs.sort()
    return f, loads, funcs


def main():
    f, loads, funcs = load(sys.argv[1])
    patched = set()
    if len(sys.argv) > 2:
        patched = {int(line.split()[0], 0) for line in open(sys.argv[2]) if line.strip()}
    starts = [s for s, _ in funcs]

    def owner(va):
        k = bisect.bisect_right(starts, va) - 1
        return starts[k] if k >= 0 and va < funcs[k][0] + funcs[k][1] else None

    where = {}
    for seg in loads:
        off, va, size = seg[2], seg[3], seg[5]
        data = f[off:off + size]
        for raw, name in CONSTANTS.items():
            at = data.find(raw)
            while at != -1:
                if at % 4 == 0:
                    where[va + at] = name
                at = data.find(raw, at + 1)

    text = next(s for s in loads if s[1] & 1)
    toff, tva, tsz = text[2], text[3], text[5]
    code = f[toff:toff + tsz]
    sites = []
    for i in range(len(code) - 8):
        modrm = code[i]
        if modrm & 0xC7 != 0x05:
            continue
        disp, = struct.unpack_from('<i', code, i + 1)
        end = tva + i + 5
        target = end + disp
        name = where.get(target)
        if not name:
            continue
        op = code[i - 1]
        if op not in (0x10, 0x11, 0x2E, 0x2F, 0x51, 0x58, 0x59, 0x5A, 0x5C, 0x5D, 0x5E, 0x5F):
            continue
        sites.append((tva + i - 1, target, name))
    for raw, name in CONSTANTS.items():
        if len(raw) != 4:
            continue
        at = code.find(raw)
        while at != -1:
            if at >= 2 and code[at - 2] == 0xC7 or (at >= 3 and code[at - 3] == 0xC7):
                sites.append((tva + at, None, name + ' (immediate)'))
            at = code.find(raw, at + 1)

    sites.sort()
    by_func = {}
    for va, target, name in sites:
        by_func.setdefault(owner(va), []).append((va, target, name))
    print(f'{len(where)} constant slots, {len(sites)} code sites in {len(by_func)} functions')
    for func, items in sorted(by_func.items(), key=lambda x: (x[0] or 0)):
        hit = lambda va, target: any(a in patched for a in range(va, va + 9)) or target in patched
        fixed = sum(1 for va, target, _ in items if hit(va, target))
        label = f'{func:#x}' if func is not None else '?'
        print(f'{label}: {len(items)} sites, {fixed} patched: ' +
              ', '.join(f'{va:#x} {name}' + ('*' if hit(va, target) else '')
                        for va, target, name in items[:6]))


if __name__ == '__main__':
    main()
