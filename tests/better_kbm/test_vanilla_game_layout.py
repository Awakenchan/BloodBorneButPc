# SPDX-License-Identifier: GPL-3.0-or-later
"""Check original 1.09 menu/camera offsets against a local plaintext SELF/ELF."""
from pathlib import Path
import runpy,struct,sys
m=runpy.run_path(str(Path(__file__).with_name('test_prompt_game_layout.py')))
read=m['read']
for va,expected in [(0x1b20900,'554889e54157415641554154534881eca8020000'),(0x14ef730,'554889e5415741564154534883ec50'),(0x1b211a0,'554889e54157415641554154534881ece8050000'),(0x1b21840,'554889e54157415641554154534881ec28040000')]:assert read(va,len(bytes.fromhex(expected)))==bytes.fromhex(expected),hex(va)
# Original openers point at the two handlers, which the new hook wraps.
for lea,target in [(0x1bdb967,0x1b211a0),(0x1bdb987,0x1b21840)]:
 op=read(lea,7);assert op[:3]==b'\x48\x8d\x0d' and lea+7+struct.unpack_from('<i',op,3)[0]==target
# Vanilla camera row IDs and their actual record fields.
for va,expected in [(0x1b21203,'4d8d6f09'),(0x1b2132f,'4d8d6f0a'),(0x1b2163f,'4d8d6f0c'),(0x1b2145c,'4c89fa'),(0x1b20a5d,'488d057ca18703')]:assert read(va,len(bytes.fromhex(expected)))==bytes.fromhex(expected),hex(va)
# Original child-stack ownership/layout used for nested Defaults dialogs.
for va,expected in [(0x1b24924,'498d5d18'),(0x1b24928,'498b4540'),(0x1b24958,'49894d40')]:assert read(va,len(bytes.fromhex(expected)))==bytes.fromhex(expected),hex(va)
print('PASS: original section hooks, Controls/Environment handlers, shared camera fields, dialog identity and native child-stack layout match supplied game.')

# The same copier handles the live options and stack-local factory defaults.
# Only the former is authoritative for host mouse-camera synchronization.
assert read(0x1bda773,17)==bytes.fromhex('488d05b6099603488b00488b78484c89fe')
for call,target in [(0x1bda784,0x14ef730),(0x1c8b105,0x14eea90),(0x1c8b110,0x14ef730)]:
 op=read(call,5);assert op[0]==0xe8 and call+5+struct.unpack_from('<i',op,1)[0]==target
assert read(0x1b20b5c,3)==bytes.fromhex('4889de') # Handler receives the copied compact record.
print('PASS: live options pointer, shared copy callers and factory-default initialization verified.')

for va,expected in [(0x1b290f0,'554889e54157415641554154534881ec38010000'),(0x1bdb270,'488b47084889f74889d6ffe0')]:assert read(va,len(bytes.fromhex(expected)))==bytes.fromhex(expected),hex(va)
print('PASS: original Defaults action builder and function-pointer invoker verified.')

# Horizontal mouse regression: this writes the shared yaw accumulator and must
# never be disabled by the DLL's auto-rotation policy.
assert read(0x143c6e8,9)==bytes.fromhex('c4c17a118544010000')
print('PASS: shared final yaw store used by manual camera input confirmed.')

# Native Defaults confirmation uses menu-dialog group 204, id 40300; its
# confirmed callback forwards the dialog to the reset loop intercepted for keys.
assert read(0x1b24882,10)==bytes.fromhex('becc000000ba6c9d0000')
assert read(0x1b1a2d0,20)==bytes.fromhex('554889e54157415641554154534881ec98000000')
op=read(0x1b24c1d,5);assert op[0]==0xe8 and 0x1b24c22+struct.unpack_from('<i',op,1)[0]==0x1b1a2d0
assert read(0x1acb760,16)==bytes.fromhex('554889e54156534881ec3001000089f3')
print('PASS: original confirmation text, confirmed reset callback and native visibility prologue verified.')

assert read(0x14df170,16)==bytes.fromhex('8b4f4883f9ff0f84c0000000488d571c')
assert read(0x165bb1c,9)==bytes.fromhex('bfd003000048037808')
for site in (0x165bb25,0x12249e5,0x1bf9f6a):
 op=read(site,5);assert op[0]==0xe8 and site+5+struct.unpack_from('<i',op,1)[0]==0x14df170
assert read(0x14df1dc,4)==bytes.fromhex('8b74f71c')
assert read(0x14df1f0,3)==bytes.fromhex('897748')
print('PASS: six-slot quick-item leaf, selected handle, player-list offset and three native callers verified.')

# Both native Localize.VoiceLang property readers must be verified before overriding them.
for lea,call in [(0x1fb6ec1,0x1fb6ee4),(0x2001b65,0x2001b85)]:
 op=read(lea,7);assert op[:3]==bytes.fromhex('488d35')
 target=lea+7+struct.unpack_from('<i',op,3)[0]
 assert read(target,len("Localize.VoiceLang\0".encode("utf-16-le")))=="Localize.VoiceLang\0".encode('utf-16-le')
 op=read(call,5);assert op[0]==0xe8 and call+5+struct.unpack_from('<i',op,1)[0]==0x20eba00
print('PASS: boot and accessor voice-property calls and their semantic keys match the original game.')

# Native voice initialization stores the regional converter result in the voice global.
op=read(0x1fb6f08,5);assert op[0]==0xe8 and 0x1fb6f0d+struct.unpack_from('<i',op,1)[0]==0x1fb97e0
assert read(0x1fb6f0d,3)==bytes.fromhex('4189c4')
op=read(0x1fb6fae,7);assert op[:3]==bytes.fromhex('448925') and 0x1fb6fb5+struct.unpack_from('<i',op,3)[0]==0x5128954
assert read(0x493a1eb,5)==b'esar\0' and read(0x493a1e6,5)==b'eses\0'
print('PASS: final native voice conversion/store and distinct Latin American/Spain sound-language identifiers verified.')
