"""Find gzip streams in a firmware image and write each decompressed blob to <outdir>.

usage: find_gzip.py <image.bin> <outdir>
"""
import sys, zlib, os
data = open(sys.argv[1], 'rb').read()
out = sys.argv[2]
os.makedirs(out, exist_ok=True)
i = 0; n = 0
while True:
    i = data.find(b'\x1f\x8b\x08', i)
    if i < 0: break
    try:
        d = zlib.decompressobj(16 + zlib.MAX_WBITS)
        res = d.decompress(data[i:i+400000])
        consumed = len(data[i:i+400000]) - len(d.unused_data)
        if len(res) > 50:
            fn = f"{out}/gz_{i:06x}.bin"
            open(fn, 'wb').write(res)
            print(f"offset 0x{i:x} compressed {consumed} -> {len(res)} bytes: {res[:80]!r}")
            n += 1
    except Exception as e:
        pass
    i += 1
print("found", n)
