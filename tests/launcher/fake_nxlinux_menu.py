#!/usr/bin/env python3
# stands in for nxlinux in the launcher's test of its menu over a game: shows its first frame, presses the Home button
# ("open" down NXL_LAUNCHER_FD), then buttons, and prints what NativeNX sends back down NXL_NOTIFY_FD (the menu's
# pictures, a screenshot, hide). The steps: argv[1] ("shot": down, down, A; "home": down x3, A; "close": down x4, A, A)
import os, select, shutil, struct, sys, time
print('[nxl] loaded main at 0x8000000'); print('[nxl] first frame shown in the window (1280x720)'); sys.stdout.flush()
inp, out = int(os.environ['NXL_NOTIFY_FD']), int(os.environ['NXL_LAUNCHER_FD'])
def say(l): os.write(out, (l + '\n').encode())
buf = b''
def lines(wait):
    global buf
    got, end = [], time.time() + wait
    while time.time() < end:
        r, _, _ = select.select([inp], [], [], 0.05)
        if r:
            b = os.read(inp, 4096)
            if not b: break
            buf += b
            while b'\n' in buf:
                l, buf = buf.split(b'\n', 1)
                got.append(l.decode())
    return got
def show(ls):
    for l in ls:
        if l.startswith('menu '):
            p = l[5:]
            with open(p, 'rb') as f: h = f.read(16)
            w, hh = struct.unpack('<II', h[8:16])
            print('menu: %s %dx%d' % (h[:8].decode(), w, hh))
            if len(sys.argv) > 2: shutil.copy(p, sys.argv[2])
        else: print('got: ' + l)
    sys.stdout.flush()
time.sleep(0.3)
say('open 1280 720')
show(lines(1.5))
steps = {'shot': ['down', 'down', 'south'], 'home': ['down', 'down', 'down', 'south'], 'close': ['down', 'down', 'down', 'down', 'south', 'south'], 'resume': ['east']}[sys.argv[1]]
for k in steps:
    say('key ' + k)
    show(lines(0.6))
show(lines(1.0))
time.sleep(3)
print('done'); sys.stdout.flush()
