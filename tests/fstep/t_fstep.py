#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks the loader's moving of nxnative's frame step stubs (src/fstep.c) against the stubs assembled where they land: for each
# build, a patch area right after an image just large enough for what the stubs reach (and one 16 MiB further), the words
# t_fstep prints must equal those nxnative's own FRAME_STEP function emits with here() and state at those offsets.
# Usage: t_fstep.py <t_fstep binary> <nxnative dm folder> <nxlinux tools folder>
import sys, subprocess
exe, dm, tools = sys.argv[1:4]
sys.path.insert(0, tools)
import gen_fstep
builds = gen_fstep.load(dm)
if len(builds) < 5: sys.exit(f'only {len(builds)} builds with frame step stubs')
args, want = [], {}
for bid, (p0, s0, words, sites, puts, rs, hi) in builds.items():
    for extra in (0, 0x1000000):
        p = ((hi + 0xFFF) & ~0xFFF) + extra
        st = p + ((4 * len(words) + 0xFFF) & ~0xFFF)
        want.setdefault(bid, []).append((p, st))
        args += [bid, hex(p), hex(st)]
fns = {}
import glob, os, importlib.util
for n, path in enumerate(sorted(glob.glob(os.path.join(dm, 'games', '*', 'patches.py')))):
    spec = importlib.util.spec_from_file_location(f't_fstep_{n}', path); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    fns.update(getattr(m, 'FRAME_STEP', {}))
bad = 0
for bid, places in want.items():
    for p, st in places:
        words, sites, puts, rs = gen_fstep.record(fns[bid], p, st)
        out = subprocess.run([exe, bid, hex(p), hex(st)], capture_output=True, text=True, check=True).stdout.split('\n')
        got_s, got_p = out[0].split()[2:], out[1].split()[2:]
        exp_s = [f'{w:08x}' for w in words]
        exp_p = [f'{w:08x}' for _, ws, _ in puts for w in ws]
        if got_s != exp_s or got_p != exp_p:
            bad += 1
            print(f'{bid} at {p:#x}/{st:#x}: moved words differ')
            for i, (a, b) in enumerate(zip(got_s + got_p, exp_s + exp_p)):
                if a != b: print(f'  word {i}: {a} != {b}')
        else: print(f'{bid} at {p:#x}: {len(words)} stub words and {len(exp_p)} site words as assembled there')
sys.exit(1 if bad else 0)
