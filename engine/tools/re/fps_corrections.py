#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lantern Engine: checks, in the 1.09 eboot, every fact the frame-rate corrections, the follow
camera at 30 and the Old Hunters edition switch (scripts/patches.py, start/patches.cpp) rely on.

Usage: fps_corrections.py out/eboot.elf [patches/Bloodborne.xml]

The game's own code and data at every site the corrections key on, then what each frame-rate list
in the XML writes there (60, 90 and uncapped FPS++, whoever made them) and that each shape is one
the corrections handle. Addresses are patch addresses (the image offset plus 0x400000). Prints OK
or FAIL for each check; the exit code is the number of failures.
"""
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

BASE = 0x400000
ROOT = Path(__file__).resolve().parents[2]
FRAME_TIME = 0x575A908
CLOTH_SCALE = 0x575A90C
CLOTH_STIFFNESS = 0x4D3BD00
CLOTH_LEFT_OUT = ((0xB6FBD4, 0xB6FBD8), (0xB73F84, 0xB73F8D), (0xB73FA8, 0xB73FC1), (0x4D3BD10, 0x4D3BD20))
sys.path.insert(0, str(ROOT / 'scripts'))
import patches as P


class Image:
    """The eboot's loaded segments as patch addresses, with its functions from .eh_frame_hdr."""

    def __init__(self, path):
        self.file = Path(path).read_bytes()
        phoff, = struct.unpack_from('<Q', self.file, 0x20)
        phnum, = struct.unpack_from('<H', self.file, 0x38)
        headers = [struct.unpack_from('<IIQQQQQQ', self.file, phoff + 56 * i) for i in range(phnum)]
        self.loads = [(vaddr, offset, filesz, memsz) for kind, _, offset, vaddr, _, filesz, memsz, _ in headers
                      if kind == 1]
        self.code_end = self.loads[0][0] + self.loads[0][2]
        frame = next(h for h in headers if h[0] == 0x6474e550)
        self.functions = self._functions(frame[2], frame[3])
        self.patched = {}
        self.disasm = Cs(CS_ARCH_X86, CS_MODE_64)
        self.disasm.detail = True

    def _functions(self, table, table_va):
        code_va, code_offset = self.loads[0][0], self.loads[0][1]
        count, = struct.unpack_from('<i', self.file, table + 8)
        out = []
        for i in range(count):
            start, fde = struct.unpack_from('<ii', self.file, table + 12 + 8 * i)
            size, = struct.unpack_from('<i', self.file, table_va + fde - code_va + code_offset + 12)
            out.append((table_va + start, size))
        return sorted(out)

    def read(self, address, length, patched=False):
        """Bytes at a patch address: the executable's own (zero past a segment's file data), or with
        the writes in `self.patched` laid over them."""
        image = address - BASE
        for vaddr, offset, filesz, memsz in self.loads:
            if vaddr <= image and image + length <= vaddr + memsz:
                start = image - vaddr
                data = bytearray(self.file[offset + start:offset + min(filesz, start + length)] if start < filesz else b'')
                data += bytes(length - len(data))
                if patched:
                    for i in range(length):
                        if address + i in self.patched:
                            data[i] = self.patched[address + i]
                return bytes(data)
        raise ValueError(f'{address:#x} is outside the eboot')

    def apply(self, writes):
        self.patched = {}
        for address, data in writes:
            for i, byte in enumerate(data):
                self.patched[address + i] = byte

    def function(self, address):
        """The patch address of the function that holds `address`, or None."""
        image = address - BASE
        lo, hi = 0, len(self.functions)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.functions[mid][0] <= image:
                lo = mid + 1
            else:
                hi = mid
        if lo and image < self.functions[lo - 1][0] + self.functions[lo - 1][1]:
            return self.functions[lo - 1][0] + BASE
        return None

    def instruction(self, address, patched=False):
        """The instruction that starts at `address`."""
        return next(self.disasm.disasm(self.read(address, 16, patched), address), None)

    def holding(self, address, patched=False):
        """The instruction whose bytes hold `address`, found by decoding forward from the start of
        its function (a linear sweep: the game's functions here have no data between code)."""
        at = self.function(address)
        if at is None:
            raise ValueError(f'{address:#x}: no function')
        while at <= address:
            insn = self.instruction(at, patched)
            if insn is None:
                raise ValueError(f'{at:#x}: does not decode')
            if address < at + insn.size:
                return insn
            at += insn.size
        raise ValueError(f'{address:#x}: not reached')

    def rip_readers(self, target, patched=False):
        """Every instruction in the code segment with a RIP-relative operand at `target`."""
        vaddr, offset, filesz, _ = self.loads[0]
        code = np.frombuffer(self.file, np.uint8, filesz, offset).copy()
        if patched:
            for address, byte in self.patched.items():
                if 0 <= address - BASE - vaddr < filesz:
                    code[address - BASE - vaddr] = byte
        found = set()
        chunk = 1 << 23
        for start in range(0, filesz - 4, chunk):
            end = min(start + chunk, filesz - 4)
            d = (code[start:end].astype(np.uint32) | code[start + 1:end + 1].astype(np.uint32) << 8 |
                 code[start + 2:end + 2].astype(np.uint32) << 16 | code[start + 3:end + 3].astype(np.uint32) << 24)
            disp = d.view(np.int32).astype(np.int64)
            where = np.arange(start, end, dtype=np.int64) + vaddr + BASE
            for tail in (4, 5, 8):
                for p in np.nonzero(where + tail + disp == target)[0]:
                    found.add(int(where[p]))
        def reads(insn, p):
            return insn is not None and insn.address + insn.disp_offset == p and any(
                op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP and
                insn.address + insn.size + op.mem.disp == target for op in insn.operands)

        readers = []
        for p in sorted(found):
            try:
                insn = self.holding(p, patched)
            except ValueError:
                insn = None
            if not reads(insn, p):
                insn = next((x for back in range(10, 0, -1)
                             for x in [next(self.disasm.disasm(self.read(p - back, 16, patched), p - back), None)]
                             if reads(x, p)), None)
            if insn is not None:
                readers.append(insn)
        return readers


def f32(data):
    return struct.unpack('<f', data)[0]


def text(insn):
    return f'{insn.address:#x}: {insn.mnemonic} {insn.op_str}'


class Report:
    def __init__(self):
        self.failures = 0

    def check(self, ok, what):
        print(('OK    ' if ok else 'FAIL  ') + what)
        self.failures += not ok

    def note(self, what):
        print('      ' + what)


def list_writes(xml, name):
    """[(patch address, bytes)] of one built-in patch, or None when the file has no such patch."""
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') == name and meta.get('AppVer') == '01.09':
            return [(offset + BASE, data) for offset, data in P.compile_patches(xml, [name], '01.09', [(0, 1 << 40)])]
    return None


def imm_store(insn, field=None):
    """(disp, imm) when `insn` is mov dword [reg+disp], imm32."""
    if insn.mnemonic != 'mov' or len(insn.operands) != 2:
        return None
    dst, src = insn.operands
    if dst.type != X86_OP_MEM or dst.size != 4 or src.type != X86_OP_IMM:
        return None
    if field is not None and dst.mem.disp != field:
        return None
    return dst.mem.disp, src.imm & 0xffffffff


def rip_target(insn):
    mem = [op for op in insn.operands if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP]
    return insn.address + insn.size + mem[0].mem.disp if mem else None


def within(at, spans):
    return any(lo <= at < hi for lo, hi in spans)


def game_effect_rate(image, r):
    print('\n== Effects: the data rate (30 a second) ==')
    insn = image.holding(0x26FB39F)
    r.check(imm_store(insn) is not None and insn.address + insn.imm_offset == 0x26FB39F and
            f32(image.read(0x26FB39F, 4)) == 30.0,
            f'0x26FB39F is the 30.0 immediate of {text(insn)} (function {image.function(insn.address):#x})')
    r.check(f32(image.read(0x4CAD7FC, 4)) == 30.0, '0x4CAD7FC holds 30.0')
    for reader in image.rip_readers(0x4CAD7FC):
        r.note(f'read by {text(reader)} (function {image.function(reader.address):#x})')


def game_cloth(image, r):
    print('\n== Cloth ==')
    r.note(f'0xB6E67A: {text(image.instruction(0xB6E67A))} (the substep count)')
    r.check(image.read(CLOTH_SCALE, 4) == bytes(4) and not image.rip_readers(CLOTH_SCALE),
            f'{CLOTH_SCALE:#x}: zero, after the frame time, and no game code reads it')
    r.check(image.read(CLOTH_STIFFNESS, 16) == b'DLLightMutex.cpp' and not image.rip_readers(CLOTH_STIFFNESS),
            f'{CLOTH_STIFFNESS:#x}: 16 bytes of a source file name no game code reads')
    load = image.holding(0xB6FBD4)
    r.check(load.address + load.disp_offset == 0xB6FBD4 and f32(image.read(rip_target(load), 4)) == 1.0,
            f'0xB6FBD4 is the displacement of {text(load)} -> {rip_target(load):#x} = 1.0')
    hook = image.instruction(0xB73F84)
    r.check(text(hook).endswith('vmulss xmm0, xmm0, xmm0'), f'0xB73F84: {text(hook)} (dt*dt)')


def game_camera(image, r):
    print('\n== Follow camera ==')
    owners = set()
    for address, field in P.CAMERA_DEFAULTS.items():
        insn = image.holding(address)
        owners.add(image.function(address))
        r.check(imm_store(insn, field) is not None and insn.address + insn.imm_offset == address,
                f'{address:#x}: {text(insn)} = {f32(image.read(address, 4)):.6g}')
    r.note('in functions ' + ', '.join(f'{x:#x}' for x in sorted(owners)))
    at, game = P.CAMERA_LOCK_BLEND
    insn = image.holding(at)
    r.check(imm_store(insn, 0x330) is not None and insn.address + insn.imm_offset == at and image.read(at, 4) == game,
            f'{at:#x}: {text(insn)} (0.05), function {image.function(insn.address):#x}')
    for lo, hi in P.CAMERA_PARAMETER_BLOCKS:
        at, game = lo, []
        while at < hi:
            insn = image.instruction(at)
            game.append(insn)
            at += insn.size
        r.check(game[0].mnemonic == 'mov' and at >= hi,
                f'{lo:#x}-{hi:#x}: {len(game)} instructions of the update\'s parameter reads: ' +
                '; '.join(text(x) for x in game[:3]) + ' ...')
    jump = image.instruction(0x183DC96)
    r.check(jump.mnemonic == 'jmp' and jump.operands[0].imm == 0x183E2F2, f'0x183DC96: {text(jump)}')
    escape = image.holding(0x183DCFE)
    r.check(escape.address + escape.imm_offset == 0x183DCFE and 'byte ptr [r13 + 0x28c], 1' in escape.op_str,
            f'0x183DCFE is the immediate of {text(escape)}')
    for address, value in P.CAMERA_GAME_EASES.items():
        r.check(image.read(address, 4) == struct.pack('<f', value), f'{address:#x} holds {value:.6g}')
        for reader in image.rip_readers(address):
            r.note(f'read by {text(reader)} (function {image.function(reader.address):#x})')


def game_status_effects(image, r):
    print('\n== Status effects: two steps a frame ==')
    for disp in (P.STATUS_STEP_FIRST, P.STATUS_STEP_SECOND[0]):
        insn = image.holding(disp)
        r.check(insn.mnemonic == 'vmovss' and insn.address + insn.disp_offset == disp and rip_target(insn) == 0x4D27C4C
                and image.read(0x4D27C4C, 4) == struct.pack('<f', 1 / 30),
                f'{text(insn)}: 0x4d27c4c = 1/30, displacement at {disp:#x}')
    r.check(image.read(P.STATUS_STEP_SECOND[0], 4) == P.STATUS_STEP_SECOND[1],
            f'{P.STATUS_STEP_SECOND[0]:#x} holds {P.STATUS_STEP_SECOND[1].hex(" ")}')
    r.check(P.STATUS_STEP_SECOND[0] + 4 + struct.unpack('<i', P.STATUS_STEP_SECOND[2])[0] == FRAME_TIME,
            f'{P.STATUS_STEP_SECOND[2].hex(" ")} there reads {FRAME_TIME:#x}')
    end, calls = 0x1CBC50D + image.instruction(0x1CBC50D).size, []
    while end < 0x1CBC560:
        insn = image.instruction(end)
        if insn.mnemonic == 'call':
            calls.append(insn)
        end += insn.size
    r.check(any('+ 0x150]' in x.op_str for x in calls), 'then a call through vtable +0x150: ' +
            ', '.join(text(x) for x in calls))
    r.check(image.function(0x1CBC4F7) == 0x1CBC470, f'in function {image.function(0x1CBC4F7):#x}')


def check_list(image, name, writes, r):
    """What one frame-rate list writes at the corrected sites, and that each shape is one the
    corrections handle."""
    print(f'\n== {name} ({len(writes)} writes) ==')
    values = dict(writes)
    image.apply(writes)
    try:
        rate = [a for a in P.EFFECT_DATA_RATE if a in values]
        r.check(all(f32(values[a]) == 60.0 for a in rate),
                f'effect data rate: {"60.0 at " + ", ".join(hex(a) for a in rate) + " (left out)" if rate else "not written"}')
        if 0xB6E67A in values:
            r.note(f'cloth substeps: {text(image.instruction(0xB6E67A, patched=True))}')
        if CLOTH_SCALE in values:
            readers = image.rip_readers(CLOTH_SCALE, patched=True)
            loads = [x for x in readers if x.operands[0].type != X86_OP_MEM]
            stores = [x for x in readers if x.operands[0].type == X86_OP_MEM]
            r.check(len(values[CLOTH_SCALE]) == 4 and len(stores) == 1 and
                    (len(loads) == 6 or not loads and values[CLOTH_SCALE] == struct.pack('<f', 1.0)),
                    f'cloth scale {f32(values[CLOTH_SCALE]):.6g} at first, read by {len(loads)} caves, '
                    f'set each frame by {", ".join(text(x) for x in stores)}')
        else:
            r.note('cloth scale: not written')
        r.check(CLOTH_STIFFNESS not in values and not [a for a, _ in writes if within(a, CLOTH_LEFT_OUT)],
                f'nothing written at {CLOTH_STIFFNESS:#x} or in the older lists\' cloth spans')
        defaults = [a for a in P.CAMERA_DEFAULTS if a in values]
        r.check(all(len(values[a]) == 4 for a in defaults),
                f'camera defaults: {len(defaults)} of {len(P.CAMERA_DEFAULTS)} written')
        for lo, hi in P.CAMERA_PARAMETER_BLOCKS:
            block = [(a, d) for a, d in writes if lo <= a < hi]
            stores = [d for _, d in block if len(d) == 11 and d[:3] == P.CAMERA_FIXED_STORE]
            jumps = [(a, d) for a, d in block if d[:1] == b'\xe9' and len(d) == 5]
            r.check(not block or (len(stores) + len(jumps) == len(block) and jumps and jumps[-1][0] + 5 == hi),
                    f'camera block {lo:#x}: ' + (f'{len(stores)} fixed stores and a jump to its end' if block
                                                  else 'not written'))
        for at in (0x183DC96, 0x183DCFE):
            r.note(f'{at:#x}: ' + (values[at].hex(' ') if at in values else 'not written'))
        for at, game in P.CAMERA_GAME_EASES.items():
            r.note(f'{at:#x}: ' + (f'{f32(values[at]):.6g} (the game\'s {game:.6g})' if at in values else 'not written'))
        first = values.get(P.STATUS_STEP_FIRST)
        if first is None:
            r.note('status effects: first step not written')
        else:
            r.check(P.STATUS_STEP_FIRST + 4 + struct.unpack('<i', first)[0] == FRAME_TIME,
                    f'status effects: the first step reads {FRAME_TIME:#x}')
            second = values.get(P.STATUS_STEP_SECOND[0])
            r.check(second in (None, P.STATUS_STEP_SECOND[2]),
                    'the second ' + ('reads it too' if second else 'is added by the corrections'))
            stores = [x for x in image.rip_readers(FRAME_TIME, patched=True) if x.operands[0].type == X86_OP_MEM]
            r.check(bool(stores), f'the list\'s code stores {FRAME_TIME:#x}: ' + ', '.join(text(x) for x in stores))
    finally:
        image.apply([])


def check_old_hunters(image, r):
    print('\n== The Old Hunters edition ==')
    insn = image.instruction(0x23B6D20)
    r.check(image.read(0x23B6D20, 5) == bytes.fromhex('e8bb210000') and insn.mnemonic == 'call' and
            insn.operands[0].imm == 0x23B8EE0, f'{text(insn)} (image offset {0x23B6D20 - BASE:#x})')
    after = image.instruction(0x23B6D25)
    target = after.address + after.size + after.operands[0].mem.disp if after.operands[0].type == X86_OP_MEM else None
    r.check(after.mnemonic == 'mov' and target is not None and after.operands[1].reg is not None and
            'eax' in after.op_str, f'then {text(after)}: the edition stored at {target:#x}' if target else text(after))
    readers = [x for x in image.rip_readers(target) if x.address != after.address] if target else []
    r.note('read by ' + ', '.join(f'{x.address:#x} ({image.function(x.address):#x})' for x in readers[:8]))
    replaced = next(image.disasm.disasm(bytes.fromhex('b801000000'), 0))
    r.check(replaced.mnemonic == 'mov' and replaced.op_str == 'eax, 1', 'b8 01 00 00 00 is mov eax, 1')
    name = image.file.find('Pkgedition'.encode('utf-16-le'))
    r.check(name >= 0, 'the property name "Pkgedition" (UTF-16) is in the file' + (f' at {name:#x}' if name >= 0 else ''))
    print('      the callee:')
    at = 0x23B8EE0
    for _ in range(40):
        insn = image.instruction(at)
        note = ''
        mem = [op for op in insn.operands if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP]
        if mem:
            to = insn.address + insn.size + mem[0].mem.disp
            raw = image.read(to, 64)
            if raw[1:2] == bytes(1) and raw[:1].isalpha():
                note = f'   ; {to:#x} "{raw.decode("utf-16-le", "replace").split(chr(0))[0]}"'
            elif raw[:1].isascii() and raw[:1].isalnum():
                note = f'   ; {to:#x} {raw.split(bytes(1))[0]!r}'
            else:
                note = f'   ; {to:#x}'
        r.note(text(insn) + note)
        at += insn.size
        if insn.mnemonic == 'ret':
            break


def main():
    image = Image(sys.argv[1])
    xml = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / 'patches/Bloodborne.xml'
    r = Report()
    game_effect_rate(image, r)
    game_cloth(image, r)
    game_camera(image, r)
    game_status_effects(image, r)
    check_old_hunters(image, r)
    for name in ('60 FPS++', '90 FPS++', 'Uncap FPS++'):
        writes = list_writes(xml, name)
        if writes is None:
            print(f'\n== {name}: not in {xml.name} ==')
        else:
            check_list(image, name, writes, r)
    print(f'\n{r.failures} failed')
    return r.failures


if __name__ == '__main__':
    sys.exit(main())
