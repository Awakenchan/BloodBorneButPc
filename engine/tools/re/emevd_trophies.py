#!/usr/bin/env python3
"""Trophies the game's event scripts award, from the player's own dump.

Usage: emevd_trophies.py <game folder>/dvdroot_ps4/event

Every event that runs instruction 2003[28] (award a trophy) is listed with the trophy id and the
event flags it sets with 2003[2]. An event that has run sets the flag of its own id, which the save
keeps: gpu/shim/engine/trophy_sync.cpp reads those flags to report trophies a save already earned.

The files are DCX (zlib, "DFLT", data at 0x4c) holding EMEVD with 64-bit fields: after the 16-byte
header, event count/offset, instruction count/offset, two unused count/offset pairs, parameter
count/offset, linked-file count/offset, argument size/offset. An event is 0x30 bytes (id, count of
instructions, their offset from the instruction table, ...), an instruction 0x20 bytes (bank, id,
argument size, argument offset from the argument block, layer).
"""
import glob
import os
import struct
import sys
import zlib


def unpack(path):
    data = open(path, 'rb').read()
    if data[:4] != b'DCX\0':
        raise ValueError(f'{path}: not DCX')
    return zlib.decompress(data[0x4c:])


def awards(path):
    d = unpack(path)
    if d[:4] != b'EVD\0':
        raise ValueError(f'{path}: not EMEVD')
    events, events_at, _, instructions_at = struct.unpack_from('<4Q', d, 0x10)
    args_at = struct.unpack_from('<Q', d, 0x10 + 13 * 8)[0]
    for e in range(events):
        event, count, first = struct.unpack_from('<3Q', d, events_at + 0x30 * e)
        trophy, flags = None, []
        for k in range(count):
            bank, ident, size, offset = struct.unpack_from('<IIQq', d, instructions_at + first + 0x20 * k)
            arg = d[args_at + offset:args_at + offset + size]
            if (bank, ident) == (2003, 28):
                trophy = struct.unpack_from('<i', arg)[0]
            elif (bank, ident) == (2003, 2) and size >= 8:
                flag, on = struct.unpack_from('<iB', arg)
                if on:
                    flags.append(flag)
        if trophy is not None:
            yield event, trophy, flags


def main():
    folder = sys.argv[1]
    for path in sorted(glob.glob(os.path.join(folder, '*.emevd.dcx'))):
        for event, trophy, flags in awards(path):
            print(f'{os.path.basename(path):24} event {event:9} trophy {trophy:2}  sets {flags}')


if __name__ == '__main__':
    main()
