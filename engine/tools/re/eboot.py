#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import bisect
import struct
import sys

import numpy as np
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

class Eboot:
    def __init__(self, path):
        self.raw = open(path, 'rb').read()
        if self.raw[:4] != b'\x7fELF':
            raise ValueError(f'{path} is not a decrypted ELF')
        phoff, = struct.unpack_from('<Q', self.raw, 0x20)
        phnum, = struct.unpack_from('<H', self.raw, 0x38)
        self.loads = []
        ehh = dynamic = dynlib = None
        for i in range(phnum):
            kind, flags, off, va, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', self.raw, phoff + 56 * i)
            if kind == 1:
                self.loads.append((va, off, filesz, memsz, flags))
            elif kind == 0x6474e550:
                ehh = (off, va)
            elif kind == 2:
                dynamic = off
            elif kind == 0x61000000:
                dynlib = off
        self.relative = self._relocations(dynamic, dynlib) if dynamic and dynlib else {}
        text = next(s for s in self.loads if s[4] & 1)
        self.text_va, self.text = text[0], np.frombuffer(self.raw, np.uint8, text[2], text[1])
        self.cs = Cs(CS_ARCH_X86, CS_MODE_64)
        self.cs.detail = True
        self._targets = {}
        self.funcs = self._functions(ehh) if ehh else []
        self.starts = [s for s, _ in self.funcs]

    def read(self, va, n):
        for sva, off, filesz, memsz, _ in self.loads:
            if sva <= va and va + n <= sva + memsz:
                inside = max(0, min(n, sva + filesz - va))
                return self.raw[off + va - sva:off + va - sva + inside] + bytes(n - inside)
        raise ValueError(f'0x{va:x}+{n} is not in a loaded segment')

    def u8(self, va): return self.read(va, 1)[0]
    def u16(self, va): return struct.unpack('<H', self.read(va, 2))[0]
    def u32(self, va): return struct.unpack('<I', self.read(va, 4))[0]
    def i32(self, va): return struct.unpack('<i', self.read(va, 4))[0]
    def u64(self, va): return struct.unpack('<Q', self.read(va, 8))[0]

    def ptr(self, va):
        return self.relative.get(va, self.u64(va))

    def cstr(self, va, limit=256):
        b = self.read(va, limit)
        return b[:b.index(0)].decode('latin-1') if 0 in b else None

    def wstr(self, va, limit=256):
        b = self.read(va, 2 * limit)
        for i in range(0, len(b), 2):
            if b[i] == 0 and b[i + 1] == 0:
                return b[:i].decode('utf-16-le')
        return None

    def _relocations(self, dynamic, dynlib):
        tags = {}
        for i in range(4096):
            t, v = struct.unpack_from('<qQ', self.raw, dynamic + 16 * i)
            if t == 0:
                break
            tags.setdefault(t, v)
        rela, size = dynlib + tags[0x6100002f], tags[0x61000031]
        table = np.frombuffer(self.raw, '<u8', size // 8, rela).reshape(-1, 3)
        rel = table[(table[:, 1] & 0xffffffff) == 8]
        return dict(zip(rel[:, 0].tolist(), rel[:, 2].tolist()))

    def _functions(self, ehh):
        off, hva = ehh
        count, = struct.unpack_from('<i', self.raw, off + 8)
        out = []
        for i in range(count):
            loc, fde = struct.unpack_from('<ii', self.raw, off + 12 + 8 * i)
            size, = struct.unpack('<i', self.read(hva + fde + 12, 4))
            out.append((hva + loc, size))
        out.sort()
        return out

    def function(self, va):
        k = bisect.bisect_right(self.starts, va) - 1
        if k >= 0 and va < self.funcs[k][0] + self.funcs[k][1]:
            return self.funcs[k]
        return None

    def is_function(self, va):
        f = self.function(va)
        return bool(f) and f[0] == va

    def disasm(self, va, n=None):
        if n is None:
            f = self.function(va)
            n = f[0] + f[1] - va if f else 0x40
        return list(self.cs.disasm(self.read(va, n), va))

    def listing(self, va, n=None):
        return '\n'.join(f'  {i.address:08x}  {i.mnemonic:6} {i.op_str}' for i in self.disasm(va, n))

    def calls_in(self, va):
        return [(i.address, int(i.op_str, 16)) for i in self.disasm(va)
                if i.mnemonic == 'call' and i.op_str.startswith('0x')]

    def _rel32_targets(self, opcode):
        if opcode not in self._targets:
            t = self.text
            pos = np.nonzero(t[:len(t) - 5] == opcode)[0]
            rel = (t[pos + 1].astype(np.int64) | (t[pos + 2].astype(np.int64) << 8) |
                   (t[pos + 3].astype(np.int64) << 16) | (t[pos + 4].astype(np.int64) << 24))
            rel = np.where(rel >= 1 << 31, rel - (1 << 32), rel)
            va = pos.astype(np.int64) + self.text_va
            self._targets[opcode] = (va, va + 5 + rel)
        return self._targets[opcode]

    def _rel32_hits(self, opcode, target):
        va, dest = self._rel32_targets(opcode)
        return [int(x) for x in va[dest == target]]

    def callers(self, target):
        return [s for s in self._rel32_hits(0xe8, target) if self.disasm(s, 5)]

    def jumps_to(self, target):
        return self._rel32_hits(0xe9, target)

    def rip_refs(self, target, lo=None, hi=None):
        t = self.text
        n = len(t) - 4
        disp = (t[:n].astype(np.int64) | (t[1:n + 1].astype(np.int64) << 8) |
                (t[2:n + 2].astype(np.int64) << 16) | (t[3:n + 3].astype(np.int64) << 24))
        disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
        pos = np.arange(n, dtype=np.int64) + self.text_va
        out = []
        for tail in (4, 5, 8):
            for p in pos[pos + tail + disp == target]:
                p = int(p)
                if lo is not None and not (lo <= p < hi):
                    continue
                for back in range(2, 5):
                    for ins in self.cs.disasm(self.read(p - back, back + tail), p - back):
                        if ins.address + ins.size != p + tail:
                            continue
                        for op in ins.operands:
                            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP and \
                                    ins.address + ins.size + op.mem.disp == target:
                                out.append(ins)
                        break
        uniq = {i.address: i for i in out}
        return [uniq[a] for a in sorted(uniq)]

if __name__ == '__main__':
    e = Eboot(sys.argv[1] if len(sys.argv) > 1 else 'out/eboot.elf')
    va = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    print(e.listing(va, int(sys.argv[3], 16) if len(sys.argv) > 3 else None))
