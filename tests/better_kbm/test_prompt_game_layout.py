# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate prompt hook offsets against the player's plaintext SELF or ELF.
No decryption. Does not modify the input and does not ship any game bytes.
"""
from pathlib import Path
import struct,sys,hashlib
b=Path(sys.argv[1]).read_bytes();self_file=b[:4]==b'O\x15=\x1d'
base=32+struct.unpack_from('<H',b,24)[0]*32 if self_file else 0
assert b[base:base+4]==b'\x7fELF'
h=struct.unpack_from('<16sHHIQQQIHHHHHH',b,base)
ph=[struct.unpack_from('<IIQQQQQQ',b,base+h[5]+i*56) for i in range(h[10])]
segments=[]
if self_file:
 for i in range(struct.unpack_from('<H',b,24)[0]):
  flags,off,size,mem=struct.unpack_from('<QQQQ',b,32+i*32)
  if not flags&0x800:continue
  assert not flags&10,'Only an already plaintext SELF is supported'
  p=ph[(flags>>20)&4095];segments.append((p[3],b[off:off+size]))
else:segments=[(p[3],b[p[2]:p[2]+p[5]]) for p in ph if p[0]==1]
def read(va,size):
 for start,data in segments:
  if start<=va and va+size<=start+len(data):return data[va-start:va-start+size]
 raise AssertionError('No segment for '+hex(va))
for va,expected in [(0x1ae7270,bytes.fromhex('554889e5415741564154534881ec20010000')),
                    (0x1321f10,bytes.fromhex('554889e54157415641554154534883ec18')),
                    (0x1b57460,bytes.fromhex('554889e54157415641554154534881ec68030000')),
                    (0x1acbd90,bytes.fromhex('554889e54157415641554154534881ecc8030000'))]:assert read(va,len(expected))==expected,hex(va)
fmt="<img src='%s' vspace='-8'>".encode('utf-16-le')+b'\0\0';assert read(0x49985be,len(fmt))==fmt
# Proven game action-caption caller. This site chooses the region's decide icon.
op=read(0x1b57943,5);assert op[0]==0xe8 and 0x1b57948+struct.unpack_from('<i',op,1)[0]==0x1acbd90
for call in (0x1ae7c95,0x1ae6337,0x1ae6557,0x1c27e9c):
 op=read(call,5);assert op[0]==0xe8 and call+5+struct.unpack_from('<i',op,1)[0]==0x1ae7270,hex(call)
print('PASS: four complete hook prologues, UTF-16 image formatter and four semantic callers match supplied game. Input SHA256:',hashlib.sha256(b).hexdigest())
