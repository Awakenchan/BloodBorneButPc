# SPDX-License-Identifier: GPL-3.0-or-later
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from input_eboot import Eboot

failures = 0

def check(what, ok, detail=''):
    global failures
    print(f'{"ok  " if ok else "FAIL"} {what}{"  (" + detail + ")" if detail else ""}')
    if not ok:
        failures += 1

def bytes_at(e, va, hexbytes):
    want = bytes.fromhex(hexbytes)
    return e.read(va, len(want)) == want

def reads(e, fn, pattern):

    start, size = e.function_at(fn)
    rx = re.compile(pattern)
    return any(rx.search(f'{m} {o}') for _, _, m, o, _ in e.disasm(start, size=size))

def calls(e, fn, target):
    start, size = e.function_at(fn)
    return any(m == 'call' and o == hex(target) for _, _, m, o, _ in e.disasm(start, size=size))

def global_at(e, ins_va):

    a, n, m, o, _ = e.disasm(ins_va, 1)[0]
    disp = int(re.search(r'rip ([+-]) (0x[0-9a-f]+)', o).group(2), 16)
    sign = -1 if re.search(r'rip -', o) else 1
    return a + n + sign * disp

def menu_list(e):
    print('-- the menus\' list component')
    update = 0x1acff30
    check('list update 1acff30 opens push rbp .. sub rsp, 0x98 (20 bytes, nothing rip-relative)',
          bytes_at(e, update, '554889e541574156415541545348 81ec98000000'.replace(' ', '')))
    check('list update: rsi is the focus byte, rdi the list', reads(e, update, r'mov r15, rsi') and
          reads(e, update, r'mov r13, rdi'))
    branches = e.branches_to(update)
    check('list update is reached by 4 calls and 1 tail jump',
          branches == [(0x1b108c7, 0xe9), (0x1b25226, 0xe8), (0x1b26066, 0xe8), (0x1b26f28, 0xe8),
                       (0x1b72c80, 0xe8)], ', '.join(f'{a:x}/{o:x}' for a, o in branches))
    check('... and from one vtable slot, 5399fb0 slot 3', e.pointers_to(update) == [0x5399fc8])
    check('list: cursor +0x74, its override +0x78', reads(e, update, r'r13 \+ 0x74\]') and
          reads(e, update, r'r13 \+ 0x78\]'))
    check('list: listener +0x30, slot 0x10 (&index)', reads(e, update, r'mov rdi, qword ptr \[r13 \+ 0x30\]'))
    check('list: root clip wrapper +0x88', reads(e, update, r'lea rsi, \[r13 \+ 0x88\]'))
    set_cursor = 0x1acf6b0
    check('SetCursor 1acf6b0: count +0x70, cells +0x7c, lines +0x80',
          all(reads(e, set_cursor, rf'r12 \+ {o}\]') for o in ('0x70', '0x7c', '0x80')))
    check('SetCursor: scroll along +0x84, in lines +0x24c, wraps +0x2b0',
          all(reads(e, set_cursor, rf'r12 \+ {o}\]') for o in ('0x84', '0x24c', '0x2b0')))
    check('SetCursor: the predicate +0x60 slot 0x10', reads(e, set_cursor, r'r12 \+ 0x60\]') and
          reads(e, set_cursor, r'call qword ptr \[rax \+ 0x10\]'))
    check('SetCursor: provider +0x258, slot 0x18 (out, provider, &{cell, line})',
          reads(e, set_cursor, r'r12 \+ 0x258\]') and reads(e, set_cursor, r'call qword ptr \[rax \+ 0x18\]'))
    check('SetCursor calls ScrollBy 1ad0dd0 and HideCursors 1acf0e0', calls(e, set_cursor, 0x1ad0dd0) and
          calls(e, set_cursor, 0x1acf0e0))
    check('SetCursor finds the row\'s "Cursor" (4934044) with 1adb220 and shows it (wrapper slot 1)',
          calls(e, set_cursor, 0x1adb220) and global_at(e, 0x1acfa49) == 0x4934044 and
          e.cstring(0x4934044) == b'Cursor' and reads(e, set_cursor, r'call qword ptr \[rax \+ 8\]'))
    check('ScrollBy 1ad0dd0 (list, along the line, lines) adds to +0x84 and +0x24c',
          reads(e, 0x1ad0dd0, r'r14 \+ 0x84\]') and reads(e, 0x1ad0dd0, r'r14 \+ 0x24c\]'))
    check('item wrapper vtable 539a070: slot 0 copies interface +0x40, type +0x48, data +0x50; slot 1 SetVisible',
          e.word(0x539a070) == 0x1adb830 and e.word(0x539a078) == 0x1acb760 and
          reads(e, 0x1adb830, r'rsi \+ 0x40\]') and reads(e, 0x1adb830, r'rsi \+ 0x48\]') and
          reads(e, 0x1adb830, r'rsi \+ 0x58\]'))
    check('root clip wrapper vtable 5399f90: type +0x40, data +0x48 (list +0xc8, +0xd0)',
          e.word(0x5399f90) == 0x1acd450 and reads(e, 0x1acd450, r'eax, dword ptr \[rsi \+ 0x40\]') and
          reads(e, 0x1acd450, r'qword ptr \[rsi \+ 0x48\]'))
    check('a stack wrapper is released through interface slot 0x18 when type & 0x40',
          reads(e, set_cursor, r'test byte ptr \[rbp - 0x58\], 0x40'))
    check('a display value (type & 0x8f == 10) holds its object at data + 0x88',
          reads(e, update, r'and ecx, 0x8f') and reads(e, update, r'mov rdi, qword ptr \[rcx \+ 0x88\]'))
    check('display object: parent +0x38, own matrix at vtable +0x18 (world matrix 2040)',
          reads(e, 0x2040, r'r14 \+ 0x38\]') and reads(e, 0x2040, r'call qword ptr \[rax \+ 0x18\]'))
    check('display object: visible = word +0x6a bit 0x4000 (SetVisible at 170)',
          reads(e, 0x170, r'or eax, 0x4000') and reads(e, 0x170, r'and eax, 0xbfff') and
          reads(e, 0xf0, r'mov word ptr \[rdi \+ 0x6a\], 0x4000'))
    check('display object: GetBounds at vtable +0x1f0 (out, object, matrix)',
          reads(e, 0x60a0, r'call qword ptr \[rax \+ 0x1f0\]'))
    check('a list listener call (vtable 5336060 slot 0x10, 1a842f0) does not open with push rbp',
          e.word(0x5336070) == 0x1a842f0 and e.read(0x1a842f0, 4) != bytes.fromhex('554889e5'))
    for name in (b'Widgets/Slider', b'Widgets/ComboBox', b'LeftArrow', b'RightArrow', b'Slider'):
        check(f'the menus name a clip {name.decode()}', e.file.find(name + b'\0') >= 0)

def menu_flow(e):
    print('-- the menu flow, input blockers, sound, region')
    flow, blocker = 0x5580e38, 0x5562878
    check('front-end test 1bb2c60: flow 5580e38 exists and +0xb11 is 0',
          global_at(e, 0x1bb2c60) == flow and reads(e, 0x1bb2c60, r'0xb11\], 0'))
    check('the game proper starting (1bb2cc0) sets +0xb11', reads(e, 0x1bb2cc0, r'byte ptr \[rdi \+ 0xb11\], 1'))
    check('flow update 1baf8c0: an open menu at +0xa88 runs through +0xab0',
          reads(e, 0x1baf8c0, r'cmp qword ptr \[r13 \+ 0xa88\], 0') and reads(e, 0x1baf8c0, r'r13 \+ 0xab0\]'))
    check('input gate 1adfec0: blocked by flow +0xa78 or 5562878 -> +0x2e4',
          global_at(e, 0x1adfef3) == flow and reads(e, 0x1adfec0, r'0xa78\], 0') and
          global_at(e, 0x1adff09) == blocker and reads(e, 0x1adfec0, r'0x2e4\], 0'))
    check('the list update posts sound 0 at 557fc60 {id, priority} with label 557fc68 = 49344e4',
          global_at(e, 0x1ad0480) == 0x557fc64 and global_at(e, 0x1ad048a) == 0x557fc60 and
          global_at(e, 0x1ad0495) == 0x49344e4 and global_at(e, 0x1ad049c) == 0x557fc68)
    check('the menu sound (1ae9bc0) plays the id and sets it back to -1',
          global_at(e, 0x1ae9bc4) == 0x557fc60 and global_at(e, 0x1ae9bda) == 0x557fc60)
    check('region 5128948 = table 4736690 {5,0,1,2,3,4}[app parameter % 10], set at boot',
          [e.u32(0x4736690 + 4 * i) for i in range(6)] == [5, 0, 1, 2, 3, 4] and
          global_at(e, 0x1fb6d1a) == 0x5128948 and reads(e, 0x1fb9190, r'imul ecx, ecx, 0xa'))

def follow_camera(e):
    print('-- the follow camera')
    update = 0x143ac60
    check('camera update 143ac60 opens push rbp .. sub rsp, 0x3d8 (20 bytes)',
          bytes_at(e, update, '554889e541574156415541545348' + '81ecd8030000'))
    check('free camera store at 143ce54: pitch +0x140 from xmm0, yaw +0x144 from xmm1[1] (19 bytes)',
          bytes_at(e, 0x143ce54, 'c4c17a118540010000' + 'c4c379178d4401000001'))
    check('... clamped between +0x1f0 (lowest) and +0x1ec (highest) just before',
          bytes_at(e, 0x143ce38, 'c4c17a1085f0010000') and bytes_at(e, 0x143ce47, 'c4c17a1085ec010000'))
    check('... the stick vector in xmm10 (pitch lane 0, yaw lane 1)', bytes_at(e, 0x143cde0, 'c5a859c9'))
    check('after it, 143ce67: the frame\'s input byte [rbp-0x3c4] stores xmm0 as the centre pitch (+0x150)',
          bytes_at(e, 0x143ce67, '8b853cfcffff84c0') and bytes_at(e, 0x143ce71, 'c4c17a118550010000'))
    for site, rel in ((0x143f9bc, '9dd4ffff'), (0x143fabf, '9ad3ffff')):
        check(f'fast turn exit {site:x}: pitch store, jmp 143ce67 (14 bytes)',
              bytes_at(e, site, 'c4c17a118540010000e9' + rel) and
              site + 9 + 5 + int.from_bytes(bytes.fromhex(rel), 'little', signed=True) == 0x143ce67)
    start, size = e.function_at(update)
    targets = {int(o, 16) for _, _, m, o, _ in e.disasm(start, size=size) if m.startswith('j') and o.startswith('0x')}
    check('no branch lands inside the replaced bytes',
          not any(lo < t < hi for t in targets for lo, hi in
                  ((0x143ce54, 0x143ce67), (0x143f9bc, 0x143f9ca), (0x143fabf, 0x143facd))))
    holds = {0x143c6e8: 'c4c17a118544010000', 0x143c870: 'c4c17a11ad44010000',
             0x143c984: 'c4c17a118d30010000', 0x143dde6: 'c4c17a118594020000'}
    for site, code in holds.items():
        check(f'auto-rotation store {site:x} ({code})', bytes_at(e, site, code) and
              not any(site < t < site + 9 for t in targets))
    check('the auto turn (+0x262) is asked only by 14368b0, from the manager\'s +0x1141 set at 1890d59',
          reads(e, 0x14368b0, r'byte ptr \[rax \+ 0x262\], 1') and
          bytes_at(e, 0x1890d59, 'c6804111000001'))

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'out/eboot.elf'
    e = Eboot(path)
    menu_list(e)
    menu_flow(e)
    follow_camera(e)
    print(f'{failures} failed' if failures else 'all facts hold')
    return 1 if failures else 0

if __name__ == '__main__':
    sys.exit(main())
