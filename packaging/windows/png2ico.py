#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# A Windows icon (.ico) from a square PNG: the PNG itself as the 256 x 256 image, as Windows Vista and newer read it,
# and smaller ones made from it when Pillow is there (without it, the PNG as it is). Usage: png2ico.py <icon.png> <out.ico>
import struct, sys, io

def png_size(b):
    if b[:8] != b'\x89PNG\r\n\x1a\n': raise SystemExit('not a PNG')
    return struct.unpack('>II', b[16:24])

src, out = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
images = []
try:
    from PIL import Image
    im = Image.open(io.BytesIO(data)).convert('RGBA')
    for s in (256, 64, 48, 32, 16):
        b = io.BytesIO()
        im.resize((s, s), Image.LANCZOS).save(b, 'PNG')
        images.append((s, b.getvalue()))
except ImportError:
    w, h = png_size(data)   # (the PNG as it is: Windows scales a larger one down)
    images.append((min(w, 256), data))
head = struct.pack('<HHH', 0, 1, len(images))
off = 6 + 16 * len(images)
dirs, blobs = b'', b''
for s, b in images:
    dirs += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(b), off + len(blobs))
    blobs += b
open(out, 'wb').write(head + dirs + blobs)
