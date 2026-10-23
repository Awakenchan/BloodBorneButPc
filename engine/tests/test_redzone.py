"""Red-zone-safe trampolines for guest stores (link_modules.protect_red_zones).

Where Windows saves extended state in the compact XSAVE layout (Intel 12th gen and later), a
fault's frame holds the AVX registers up to 0x28 bytes below rsp, over the System V red zone, so
a guest leaf function whose store faults on a GPU-tracked page loses its locals (the
attack-trail crash at 0x28ce9b5). The Windows test runs that sequence for real: a vectored
handler unprotects the page as the GPU tracker does.
"""
from paths import ROOT
import ctypes
import hashlib
import json
import os
import struct
import unittest
import xml.etree.ElementTree as ET
from link_modules import LEA_DOWN, LEA_UP, protect_red_zones, rewrite_red_zone_functions

SENTINEL = 0x1122334455667788
STORE = 30


def leaf(page):
    """vcmptrueps ymm0 (AVX state in use); mov rax,SENTINEL; mov [rsp-0x40],rax; mov rcx,page;
    mov dword [rcx],1; mov rax,[rsp-0x40]; vzeroupper; ret"""
    code = (bytes.fromhex('c5fcc2c00f') + b'\x48\xb8' + struct.pack('<Q', SENTINEL) +
            bytes.fromhex('48894424c0') + b'\x48\xb9' + struct.pack('<Q', page) +
            bytes.fromhex('c70101000000') + bytes.fromhex('488b4424c0' 'c5f877' 'c3'))
    return code, [[STORE, 'c70101000000', [-6]]]


class TrampolineBytes(unittest.TestCase):
    def test_site_jumps_to_trampoline_that_lowers_rsp_around_the_store(self):
        image = bytearray(leaf(0)[0])
        code, applied = protect_red_zones(image, leaf(0)[1], 0x100)
        self.assertEqual(applied, 1)
        self.assertEqual(image[STORE:STORE + 5], b'\xe9' + struct.pack('<i', 0x100 - (STORE + 5)))
        self.assertEqual(image[STORE + 5], 0xcc)
        body = LEA_DOWN + bytes.fromhex('c70101000000') + LEA_UP
        self.assertEqual(code[:len(body)], body)
        back = 0x100 + len(body) + 5
        self.assertEqual(code[len(body):len(body) + 5], b'\xe9' + struct.pack('<i', STORE + 6 - back))

    def test_other_instructions_of_a_site_are_copied_unchanged(self):
        image = bytearray(bytes.fromhex('8b442404' '8946a4' 'c3'))
        code, applied = protect_red_zones(image, [[0, '8b4424048946a4', [4, -3]]], 0x40)
        self.assertEqual(applied, 1)
        self.assertTrue(code.startswith(bytes.fromhex('8b442404') + LEA_DOWN + bytes.fromhex('8946a4') + LEA_UP))

    def test_changed_bytes_or_relocations_leave_the_site_alone(self):
        image = bytearray(leaf(0)[0])
        self.assertEqual(protect_red_zones(image, [[STORE, 'c70102000000', [-6]]], 0x100), (b'', 0))
        self.assertEqual(protect_red_zones(image, leaf(0)[1], 0x100, skip=[STORE + 3]), (b'', 0))
        self.assertEqual(image, leaf(0)[0])


class SiteTable(unittest.TestCase):
    sites = json.loads((ROOT / 'patches' / 'redzone.json').read_text())['sites']

    def test_attack_trail_stores_are_covered(self):
        covered = set()
        for offset, original, lengths in self.sites:
            pos = offset
            for length in lengths:
                if length < 0:
                    covered.add(pos)
                pos += abs(length)
        for store in (0x28cedfe, 0x28cee33, 0x28cee68, 0x28cee72):
            self.assertIn(store, covered)

    def test_sites_do_not_overlap_community_patches(self):
        lines = ET.parse(ROOT / 'patches' / 'Bloodborne.xml').iter('Line')
        patched = sorted(int(line.get('Address'), 16) - 0x400000 for line in lines)
        for offset, original, _ in self.sites:
            end = offset + len(original) // 2
            self.assertFalse(any(offset - 8 < p < end for p in patched), hex(offset))


def red_zone_function():
    """sub rsp,0x20; mov [rsp-0x40],rax; mov [rsp+0x70],rax; je +7; lea rax,[rip+0x10];
    add rsp,0x20; ret, and its recipe (tools/redzone_functions.py's format)."""
    code = bytes.fromhex('4883ec20' '48894424c0' '4889442470' '7407' '488d0510000000' '4883c420' 'c3')
    insns = [[4, 4], [5, 1, 2, 4, 1], [5, 1, 2, 4, 1], [2, 3, 4, 23], [7, 2, 3], [4, 5], [1]]
    return code, {'offset': 0, 'size': len(code), 'sha256': hashlib.sha256(code).hexdigest(), 'insns': insns}


class WholeFunctions(unittest.TestCase):
    """rewrite_red_zone_functions (BB_RED_ZONE_FUNCTIONS=1): the copy keeps nothing below rsp."""

    def test_copy_runs_with_rsp_lower_and_operands_moved(self):
        code, recipe = red_zone_function()
        image = bytearray(code)
        copy, applied = rewrite_red_zone_functions(image, [recipe], 0x100)
        self.assertEqual(applied, 1)
        self.assertEqual(image[:5], b'\xe9' + struct.pack('<i', 0x100 - 5))
        expected = (bytes.fromhex('4883ec20') + LEA_DOWN +
                    bytes.fromhex('4889442440') +
                    bytes.fromhex('48898424f0000000'))
        self.assertEqual(copy[:len(expected)], expected)
        at = len(expected)
        self.assertEqual(copy[at:at + 2], b'\x0f\x84')
        lea = at + 6
        epilogue = lea + 7
        self.assertEqual(struct.unpack_from('<i', copy, at + 2)[0], epilogue - (at + 6))
        rip = struct.unpack_from('<i', copy, lea + 3)[0]
        self.assertEqual(0x100 + lea + 7 + rip, 16 + 7 + 0x10)
        self.assertEqual(copy[epilogue:epilogue + len(LEA_UP) + 5], LEA_UP + bytes.fromhex('4883c420c3'))
        self.assertEqual(len(copy) % 16, 0)

    def test_changed_bytes_or_relocations_leave_the_function_alone(self):
        code, recipe = red_zone_function()
        changed = bytearray(code)
        changed[8] = 0xb8
        self.assertEqual(rewrite_red_zone_functions(changed, [recipe], 0x100), (b'', 0))
        image = bytearray(code)
        self.assertEqual(rewrite_red_zone_functions(image, [recipe], 0x100, skip=[16]), (b'', 0))
        self.assertEqual(image, code)

    def test_shipped_recipes_are_complete(self):
        functions = json.loads((ROOT / 'patches' / 'redzone_functions.json').read_text())['functions']
        self.assertEqual(sorted(f['offset'] for f in functions), [0x28ce7b0, 0x28ceec0])
        for f in functions:
            self.assertEqual(sum(i[0] for i in f['insns']), f['size'], hex(f['offset']))


@unittest.skipUnless(os.name == 'nt', 'Windows exception dispatch')
class WindowsFault(unittest.TestCase):
    """A store faults on a read-only page; the handler makes it writable and resumes."""

    def run_leaf(self, protect):
        k32 = ctypes.WinDLL('kernel32', use_last_error=True)
        k32.VirtualAlloc.restype = ctypes.c_void_p
        k32.VirtualAlloc.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32)
        k32.VirtualProtect.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32))
        k32.VirtualFree.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32)
        k32.AddVectoredExceptionHandler.restype = ctypes.c_void_p
        k32.RemoveVectoredExceptionHandler.argtypes = (ctypes.c_void_p,)
        page = k32.VirtualAlloc(None, 4096, 0x3000, 0x02)
        text = k32.VirtualAlloc(None, 4096, 0x3000, 0x40)
        code, sites = leaf(page)
        image = bytearray(code)
        if protect:
            trampolines, applied = protect_red_zones(image, sites, 0x100)
            self.assertEqual(applied, 1)
            image += bytes(0x100 - len(image)) + trampolines
        ctypes.memmove(text, bytes(image), len(image))

        class Record(ctypes.Structure):
            _fields_ = [('code', ctypes.c_uint32), ('flags', ctypes.c_uint32), ('next', ctypes.c_void_p),
                        ('address', ctypes.c_void_p), ('count', ctypes.c_uint32),
                        ('information', ctypes.c_size_t * 15)]

        class Pointers(ctypes.Structure):
            _fields_ = [('record', ctypes.POINTER(Record)), ('context', ctypes.c_void_p)]

        @ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.POINTER(Pointers))
        def handler(info):
            r = info.contents.record.contents
            if r.code != 0xC0000005 or not page <= r.information[1] < page + 4096:
                return 0
            k32.VirtualProtect(page, 4096, 0x04, ctypes.byref(ctypes.c_uint32()))
            return -1

        cookie = k32.AddVectoredExceptionHandler(1, handler)
        try:
            result = ctypes.CFUNCTYPE(ctypes.c_uint64)(text)()
        finally:
            k32.RemoveVectoredExceptionHandler(cookie)
            k32.VirtualFree(text, 0, 0x8000)
            k32.VirtualFree(page, 0, 0x8000)
        return result

    def test_exception_frame_overwrites_the_red_zone(self):
        if self.run_leaf(protect=False) == SENTINEL:
            self.skipTest('this CPU keeps the red zone: its AVX state is saved further down')

    def test_trampoline_keeps_the_red_zone(self):
        self.assertEqual(self.run_leaf(protect=True), SENTINEL)


if __name__ == '__main__':
    unittest.main()
