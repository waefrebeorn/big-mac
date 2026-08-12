#!/usr/bin/env python3
"""Tiny BMP (32-bit, bottom-up) -> PNG converter. Pure stdlib."""
import struct, zlib, sys

src, out = "/tmp/wbdaw_headless.ppm", "/tmp/wbdaw_text.png"

with open(src, "rb") as f:
    data = f.read()
assert data[0:2] == b'BM', "not a BMP"
offset = struct.unpack_from('<I', data, 10)[0]
w = struct.unpack_from('<i', data, 18)[0]
h = struct.unpack_from('<i', data, 22)[0]
bpp = struct.unpack_from('<H', data, 28)[0]
print(f"BMP {w}x{h} bpp={bpp}")

raw = data[offset:]
stride = ((w * (bpp//8) + 3) // 4) * 4
top = bytearray()
for row in range(h-1, -1, -1):
    base = row * stride
    for px in range(w):
        p = base + px * (bpp//8)
        b,g,r,a = raw[p], raw[p+1], raw[p+2], raw[p+3] if bpp==32 else 255
        top += bytes((r,g,b,a))

def chunk(typ, payload):
    c = struct.pack('>I', len(payload)) + typ + payload
    return c + struct.pack('>I', zlib.crc32(typ+payload) & 0xffffffff)

png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
rs = w * 4
raw_img = b''.join(b'\x00' + top[row*rs:(row+1)*rs] for row in range(h))
png += chunk(b'IDAT', zlib.compress(raw_img)) + chunk(b'IEND', b'')
with open(out, "wb") as f:
    f.write(png)
print(f"saved {out} ({len(png)} bytes)")
