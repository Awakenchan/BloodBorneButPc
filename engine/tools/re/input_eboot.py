# SPDX-License-Identifier: GPL-3.0-or-later
import bisect
import struct

import capstone

PT_LOAD, PT_DYNAMIC, PT_GNU_EH_FRAME, PT_SCE_DYNLIBDATA = 1, 2, 0x6474e550, 0x61000000
DT_SCE_RELA, DT_SCE_RELASZ = 0x6100002f, 0x61000031
R_X86_64_RELATIVE = 8

class Eboot:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.file = f.read()
        phoff, = struct.unpack_from('<Q', self.file, 0x20)
        phnum, = struct.unpack_from('<H', self.file, 0x38)

        self.segments = [struct.unpack_from('<IIQQQQQQ', self.file, phoff + 56 * i) for i in range(phnum)]
        self.loaded = [s for s in self.segments if s[0] == PT_LOAD]
        self.code = [s for s in self.loaded if s[1] & 1]
        self._cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self._cs.detail = False
        self._functions = None
        self._relocs = None

    def relocations(self):

        if self._relocs is None:
            self._relocs = {}
            dyn = next((s for s in self.segments if s[0] == PT_DYNAMIC), None)
            lib = next((s for s in self.segments if s[0] == PT_SCE_DYNLIBDATA), None)
            rela = relasz = None
            for i in range(dyn[5] // 16 if dyn else 0):
                tag, value = struct.unpack_from('<qQ', self.file, dyn[2] + 16 * i)
                if tag == 0:
                    break
                if tag == DT_SCE_RELA:
                    rela = value
                elif tag == DT_SCE_RELASZ:
                    relasz = value
            if lib and rela is not None and relasz:
                for off in range(lib[2] + rela, lib[2] + rela + relasz, 24):
                    where, info, addend = struct.unpack_from('<QQq', self.file, off)
                    if info & 0xffffffff == R_X86_64_RELATIVE:
                        self._relocs[where] = addend
        return self._relocs

    def word(self, va):

        return self.relocations().get(va, self.u64(va))

    def _segment(self, va):
        for s in self.loaded:
            if s[3] <= va < s[3] + s[6]:
                return s
        return None

    def mapped(self, va):
        return self._segment(va) is not None

    def executable(self, va):
        s = self._segment(va)
        return bool(s and s[1] & 1)

    def read(self, va, n):
        s = self._segment(va)
        if s is None:
            raise ValueError(f'0x{va:x} is not in a loaded segment')
        out = bytearray(n)
        avail = max(0, min(n, s[3] + s[5] - va))
        out[:avail] = self.file[s[2] + va - s[3]:s[2] + va - s[3] + avail]
        return bytes(out)

    def u32(self, va):
        return struct.unpack('<I', self.read(va, 4))[0]

    def u64(self, va):
        return struct.unpack('<Q', self.read(va, 8))[0]

    def cstring(self, va, limit=256):
        raw = self.read(va, limit)
        return raw.split(b'\0', 1)[0]

    def disasm(self, va, count=None, size=None):
        data = self.read(va, size if size else 16 * (count or 1) + 16)
        out = []
        for i in self._cs.disasm(data, va):
            out.append((i.address, i.size, i.mnemonic, i.op_str, bytes(i.bytes)))
            if count and len(out) == count:
                break
        return out

    def disasm_text(self, va, count=None, size=None):
        return '\n'.join(f'{a:08x}  {b.hex(" "):<44} {m} {o}' for a, _, m, o, b in self.disasm(va, count, size))

    def branches_to(self, target):

        found = []
        for s in self.code:
            code = self.file[s[2]:s[2] + s[5]]
            base = s[3]
            for op in (0xe8, 0xe9):
                i = code.find(bytes([op]))
                while i >= 0:
                    if i + 5 <= len(code):
                        rel, = struct.unpack_from('<i', code, i + 1)
                        if base + i + 5 + rel == target:
                            found.append((base + i, op))
                    i = code.find(bytes([op]), i + 1)
        return sorted(found)

    def pointers_to(self, value):

        needle = struct.pack('<Q', value)
        found = [va for va, addend in self.relocations().items() if addend == value]
        for s in self.loaded:
            data = self.file[s[2]:s[2] + s[5]]
            i = data.find(needle)
            while i >= 0:
                if (s[3] + i) % 8 == 0:
                    found.append(s[3] + i)
                i = data.find(needle, i + 1)
        return sorted(set(found))

    def rip_refs_to(self, target):

        import numpy as np
        cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        found = set()
        for s in self.code:
            data = self.file[s[2]:s[2] + s[5]]
            base = s[3]
            n = len(data) - 4

            disp = np.lib.stride_tricks.as_strided(np.frombuffer(data, dtype=np.uint8), shape=(n, 4),
                                                   strides=(1, 1)).copy().view('<i4').reshape(n).astype(np.int64)
            pos = np.arange(n, dtype=np.int64)
            for k in (0, 1, 2, 4):
                for p in np.nonzero(base + pos + 4 + k + disp == target)[0]:
                    at = base + int(p)
                    ins = self._instruction_over(at)
                    if ins is None:
                        for back in range(11, 0, -1):
                            start = at - back
                            if start < base:
                                continue
                            ins = next(cs.disasm(data[start - base:start - base + 16], start), None)
                            if ins and ins.address + ins.size == at + 4 + k:
                                ins = (ins.address, ins.size, ins.mnemonic, ins.op_str)
                                break
                            ins = None
                    if ins and ins[0] + ins[1] == at + 4 + k and 'rip' in ins[3]:
                        found.add((ins[0], ins[2], ins[3]))
        return sorted(found)

    def _instruction_over(self, va):

        f = self.function_at(va)
        if f is None:
            return None
        if not hasattr(self, '_sweeps'):
            self._sweeps = {}
        if f[0] not in self._sweeps:
            self._sweeps[f[0]] = [(a, n, m, o) for a, n, m, o, _ in self.disasm(f[0], size=f[1])]
        for ins in self._sweeps[f[0]]:
            if ins[0] <= va < ins[0] + ins[1]:
                return ins
        return None

    def _load_functions(self):
        eh = next((s for s in self.segments if s[0] == PT_GNU_EH_FRAME), None)
        funcs = []
        if eh:
            hoff, hva = eh[2], eh[3]
            count, = struct.unpack_from('<i', self.file, hoff + 8)
            for i in range(count):
                loc, fde = struct.unpack_from('<ii', self.file, hoff + 12 + 8 * i)
                fde_va = hva + fde
                size, = struct.unpack('<i', self.read(fde_va + 12, 4))
                funcs.append((hva + loc, size))
        funcs.sort()
        self._functions = funcs
        self._starts = [f[0] for f in funcs]

    def function_at(self, va):
        if self._functions is None:
            self._load_functions()
        k = bisect.bisect_right(self._starts, va) - 1
        if k >= 0 and va < self._functions[k][0] + self._functions[k][1]:
            return self._functions[k]
        return None

    def is_function_start(self, va):
        if self._functions is None:
            self._load_functions()
        k = bisect.bisect_left(self._starts, va)
        return k < len(self._starts) and self._starts[k] == va
