#!/usr/bin/env python3
# stands in for nxlinux in the launcher's achievements test: shows its first frame, saves (a flags32 save that meets one
# of the test list's achievements), then reads the pop-ups the launcher sends down NXL_NOTIFY_FD and ends
import os, select, struct, sys, time, zlib
def flag(name, v): return struct.pack('<II', zlib.crc32(name.encode()), v & 0xFFFFFFFF)
print('[nxl] loaded main at 0x8000000'); print('[nxl] first frame shown in the window (1280x720)'); sys.stdout.flush()
save = os.environ['DM_SAVE']
os.makedirs(save + '/0', exist_ok=True)
with open(save + '/0/test.sav', 'wb') as f:
    f.write(struct.pack('<III', 1, 0xFFFFFFFF, 1) + flag('Shells', 5) + flag('Kraken_Defeated', 0))
print('saved'); sys.stdout.flush()
fd = int(os.environ.get('NXL_NOTIFY_FD', '-1'))
got, end = b'', time.time() + 5
while fd >= 0 and time.time() < end and got.count(b'\n') < 2:
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        b = os.read(fd, 4096)
        if not b: break
        got += b
for line in got.decode().split('\n'):
    if not line: continue
    with open(line, 'rb') as f: h = f.read(16)
    w, hh = struct.unpack('<II', h[8:16])
    print('pop-up: %s %dx%d %d bytes' % (h[:8].decode(), w, hh, os.path.getsize(line)))
    os.remove(line)
print('NXL_NOTIFY_FD=%s' % os.environ.get('NXL_NOTIFY_FD', 'unset'))
sys.exit(0)
