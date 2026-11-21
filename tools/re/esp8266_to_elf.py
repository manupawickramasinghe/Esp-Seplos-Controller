"""Wrap an ESP8266 (Arduino, eboot+app) image's app segments into a minimal ELF32 Xtensa file."""
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
img = open(src, 'rb').read()
assert img[0] == 0xE9
nseg = img[1]; entry = struct.unpack_from('<I', img, 4)[0]
off = 8; segs = []
for _ in range(nseg):
    addr, ln = struct.unpack_from('<II', img, off); off += 8
    segs.append((addr, img[off:off+ln])); off += ln
ehsize, phsize, shsize = 52, 32, 40
names = b'\0' + b''.join(f'.seg{i}\0'.encode() for i in range(len(segs))) + b'.shstrtab\0'
data_off = ehsize + phsize*len(segs)
blobs = []; cur = data_off
for addr, d in segs:
    blobs.append((addr, cur, d)); cur += len(d)
shstr_off = cur; cur += len(names)
cur = (cur + 3) & ~3
shoff = cur
out = bytearray()
out += b'\x7fELF' + bytes([1,1,1,0]) + b'\0'*8
out += struct.pack('<HHIIIIIHHHHHH', 2, 94, 1, entry, ehsize, shoff, 0x300, ehsize, phsize, len(segs), shsize, len(segs)+2, len(segs)+1)
for addr, fo, d in blobs:
    exec_ = addr >= 0x40000000
    out += struct.pack('<IIIIIIII', 1, fo, addr, addr, len(d), len(d), 5 if exec_ else 6, 4)
for _, _, d in blobs: out += d
out += names
out += b'\0' * (shoff - len(out))
out += b'\0'*shsize
noff = 1
for i,(addr, fo, d) in enumerate(blobs):
    exec_ = addr >= 0x40000000
    out += struct.pack('<IIIIIIIIII', noff, 1, 2|(4 if exec_ else 1), addr, fo, len(d), 0, 0, 4, 0)
    noff += len(f'.seg{i}\0')
out += struct.pack('<IIIIIIIIII', noff, 3, 0, 0, shstr_off, len(names), 0, 0, 1, 0)
open(dst, 'wb').write(out)
for a, fo, d in blobs: print(hex(a), hex(len(d)))
