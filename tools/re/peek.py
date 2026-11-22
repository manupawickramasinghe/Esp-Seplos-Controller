"""Print 32-bit words (as int, float and string pointer) at ESP8266 addresses of an app image.

usage: peek.py <app.bin> <hex address>...   (app.bin = firmware image without the 4 KiB eboot)
"""
import struct, sys
img = open(sys.argv[1],'rb').read()
segs=[]; off=8
for _ in range(img[1]):
    a,l=struct.unpack_from('<II',img,off); off+=8; segs.append((a,off,l)); off+=l
def rd(addr,n):
    for a,o,l in segs:
        if a<=addr<a+l: return img[o+addr-a:o+addr-a+n]
for x in sys.argv[2:]:
    addr=int(x,16); w=struct.unpack('<I',rd(addr,4))[0]; f=struct.unpack('<f',rd(addr,4))[0]
    s=rd(w,40) if rd(w,1) else b''
    print(f"{x}: 0x{w:08x} int={struct.unpack('<i',rd(addr,4))[0]} float={f:g} str={s.split(b'\0')[0][:40]!r}")
