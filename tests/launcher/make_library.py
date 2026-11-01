#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# A library of made-up games for the launcher's tests (tests/launcher): game folders with ExeFS/main, a main.npdm with
# a program ID, and for some a control.nacp (title, publisher, version) and an icon (a picture written by the test
# itself, raw PPM renamed: the launcher reads JPEG and PNG, so the icons are made as PNG with zlib). Nothing from a game.
import os, struct, sys, zlib

def png(path, w, h, fn):
    raw = b''.join(b'\0' + bytes(c for x in range(w) for c in fn(x, y)) for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

def npdm(path, pid):
    meta = bytearray(0x80); meta[:4] = b'META'; struct.pack_into('<II', meta, 0x70, 0x80, 0x40)
    aci = bytearray(0x40); aci[:4] = b'ACI0'; struct.pack_into('<Q', aci, 0x10, pid)
    open(path, 'wb').write(bytes(meta) + bytes(aci))

def nacp(path, title, pub, ver, compressed=False):
    b = bytearray(0x4000)
    t = bytearray(16 * 0x300)
    for lang in (0, 1):
        t[lang * 0x300:lang * 0x300 + len(title.encode())] = title.encode()
        t[lang * 0x300 + 0x200:lang * 0x300 + 0x200 + len(pub.encode())] = pub.encode()
    if compressed:   # system 21.0.0's format: TitlesDataFormat 1 at 0x3215, a 16-bit size, then raw Deflate
        c = zlib.compressobj(9, zlib.DEFLATED, -15); z = c.compress(bytes(t)) + c.flush()
        b[0:2] = struct.pack('<H', len(z)); b[2:2 + len(z)] = z; b[0x3215] = 1
    else:
        b[0:len(t)] = t
    b[0x3060:0x3060 + len(ver)] = ver.encode()
    open(path, 'wb').write(bytes(b))

def art(seed):
    import math
    def f(x, y):
        u, v = x / 255, y / 255
        r = 0.5 + 0.5 * math.sin(6 * u + seed + 3 * math.sin(4 * v + seed))
        g = 0.5 + 0.5 * math.sin(5 * v + seed * 1.7 + 2 * math.cos(3 * u))
        b = 0.5 + 0.5 * math.cos(7 * (u + v) + seed * 0.3)
        return (int(40 + 200 * r), int(40 + 180 * g), int(60 + 190 * b))
    return f

root = sys.argv[1]
games = [  # folder, program ID, NACP title or None, picture: 'nacp' (icon_*.dat), 'cover' (cover.png) or None
    ('Sky-Garden-1.2', 0x0100AAAA00010000, 'Sky Garden Adventures', 'nacp'),
    ('Pixel Racer', 0x0100AAAA00020000, 'Pixel Racer Turbo', 'cover'),
    ('Deep Caves', 0x0100AAAA00030000, None, 'cover'),
    ('Moon Farm', 0x0100AAAA00040000, 'Moon Farm Story', 'nacp'),
    ('Untitled Game', 0x0100AAAA00050000, None, None),
    ('Ocean Song', 0x0100AAAA00060000, 'Ocean Song', 'nacp'),
    ('Packed-2.0', 0x0100AAAA00070000, 'Packed Titles Deluxe', 'nacp'),
    ('Zz Frame Step', 0x01007EF00011E000, None, None),   # a main whose build ID is one nxnative's frame step knows (its header only)
]
for i, (folder, pid, title, pic) in enumerate(games):
    d = os.path.join(root, folder)
    os.makedirs(os.path.join(d, 'ExeFS'), exist_ok=True); os.makedirs(os.path.join(d, 'RomFS'), exist_ok=True)
    hdr = bytearray(b'NSO0' + b'\0' * 0x9c)
    if folder == 'Zz Frame Step': hdr[0x40:0x50] = bytes.fromhex('cd57b23fa4bbad65803d9788c01821ee')   # Breath of the Wild 1.9.0's build ID
    open(os.path.join(d, 'ExeFS', 'main'), 'wb').write(bytes(hdr))
    npdm(os.path.join(d, 'ExeFS', 'main.npdm'), pid)
    if title: nacp(os.path.join(d, 'control.nacp'), title, 'Example Studio', '1.%d.0' % i, compressed=folder.startswith('Packed'))
    if pic == 'nacp': png(os.path.join(d, 'icon_AmericanEnglish.dat'), 256, 256, art(i * 1.3))
    elif pic == 'cover': png(os.path.join(d, 'cover.png'), 256, 256, art(i * 2.1 + 5))
open(os.path.join(root, 'Sky-Garden-1.2', 'nxlinux.env'), 'w').write('# test\nNXL_SEGMENT=0\n')
# its DLC: two, extracted into DLC/<index>/ (and a folder that is not one)
for p in ('DLC/1/dlc', 'DLC/2/dlc', 'DLC/notes'): os.makedirs(os.path.join(root, 'Sky-Garden-1.2', p), exist_ok=True)
# mods: a romfs one, one in Atmosphere's layout, one with code patches only
sg = os.path.join(root, 'Sky-Garden-1.2', 'mods')
for p in ('Wrapped Mod/Wrapped Mod v2/01007EF00011E000/romfs/Actor', 'HD Textures/romfs/Model', 'Atmos Mod/atmosphere/contents/0100AAAA00010000/romfs/Pack', 'Code Only/exefs'):
    os.makedirs(os.path.join(sg, p), exist_ok=True)
open(os.path.join(sg, 'HD Textures/romfs/Model/tree.bin'), 'wb').write(b'hd')
open(os.path.join(sg, 'Code Only/exefs/patch.ips'), 'wb').write(b'PATCHEOF')
os.makedirs(os.path.join(root, 'not a game'), exist_ok=True)
print(root)
