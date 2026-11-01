#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# nxnative's frame step stubs (FRAME_STEP of dm/games/*/patches.py) → a C table (nxl_fstep.c) the loader installs in a main
# module of that build. nxnative's patch_nso.py assembles them into a patch area inside its rewritten image; nxlinux loads
# the console's own modules, so the loader puts the patch area and the state page after the image instead, and relocates
# the stubs' PC-relative words (B / BL and ADRP: the only kinds that leave the stubs; checked here) to where they land.
# Each stub function is run here with a recording context: ctx.here() / ctx.emit(word) assemble at P0, ctx.site(offset,
# original bytes) is checked by the loader, ctx.put(offset, words, reason) is written by it, ctx.state is the state page S0.
# P0 and S0 are placeholders above every address of the image the stubs reach, so the loader can tell them apart.
# Usage: gen_fstep.py <nxnative dm folder> <output .c>
import sys, os, glob, importlib.util, types

STATE_SIZE = 0x4000

def sx(v, bits): return v - (1 << bits) if v & (1 << (bits - 1)) else v
def target(word, pc):   # the address a PC-relative word reaches, its kind; None when it is not one
    if word & 0x7C000000 == 0x14000000: return pc + 4 * sx(word & 0x3FFFFFF, 26), 'b'          # B, BL
    if word & 0x9F000000 == 0x90000000:                                                          # ADRP
        imm = ((word >> 29) & 3) | (((word >> 5) & 0x7FFFF) << 2)
        return (pc & ~0xFFF) + (sx(imm, 21) << 12), 'adrp'
    if word & 0x9F000000 == 0x10000000: return None, 'adr'                                       # ADR
    if word & 0xFF000010 == 0x54000000: return pc + 4 * sx((word >> 5) & 0x7FFFF, 19), 'bcond'   # B.cond
    if word & 0x7E000000 == 0x34000000: return pc + 4 * sx((word >> 5) & 0x7FFFF, 19), 'cbz'     # CBZ / CBNZ
    if word & 0x7E000000 == 0x36000000: return pc + 4 * sx((word >> 5) & 0x3FFF, 14), 'tbz'      # TBZ / TBNZ
    if word & 0x3B000000 == 0x18000000: return None, 'ldr literal'
    return None, None

def record(fn, p0, s0):
    words, sites, puts = [], [], []
    ctx = types.SimpleNamespace(state=s0, render_scale=0)
    ctx.here = lambda: p0 + 4 * len(words)
    ctx.emit = lambda w: words.append(w & 0xFFFFFFFF)
    ctx.site = lambda off, orig: sites.append((off, bytes.fromhex(orig) if isinstance(orig, str) else bytes(orig)))
    ctx.put = lambda off, ws, why: puts.append((off, [w & 0xFFFFFFFF for w in ws], str(why)))
    fn(ctx)
    return words, sites, puts, ctx.render_scale

def check(bid, words, sites, puts, p0, s0):   # every PC-relative word: inside the stubs, or a B / ADRP the loader relocates
    end = p0 + 4 * len(words)
    def one(w, pc, inside):
        t, kind = target(w, pc)
        if kind is None: return 0
        if kind in ('adr', 'ldr literal'): sys.exit(f'gen_fstep: {bid}: a {kind} at {pc:#x} cannot be relocated')
        here = p0 <= t < end or (kind == 'adrp' and (p0 & ~0xFFF) <= t < end)
        if kind not in ('b', 'adrp') and not (here if inside else False): sys.exit(f'gen_fstep: {bid}: a {kind} at {pc:#x} leaves the stubs ({t:#x})')
        return t if not (p0 <= t < p0 + 0x100000 or s0 <= t < s0 + STATE_SIZE) else 0
    hi = 0
    for i, w in enumerate(words): hi = max(hi, one(w, p0 + 4 * i, 1))
    for off, ws, _ in puts:
        for i, w in enumerate(ws): hi = max(hi, one(w, off + 4 * i, 0))
    for off, ob in sites: hi = max(hi, off + len(ob))
    for off, ws, _ in puts: hi = max(hi, off + 4 * len(ws))
    return hi

def load(dm):   # {build ID: (P0, S0, words, sites, puts, render_scale, the highest image offset reached)}
    sys.path.insert(0, os.path.join(dm, 'tools'))
    files = sorted(glob.glob(os.path.join(dm, 'games', '*', 'patches.py')))
    builds = {}
    for n, path in enumerate(files):
        try:
            spec = importlib.util.spec_from_file_location(f'nxl_fstep_{n}', path)
            m = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(m)
        except Exception as e:
            print(f'gen_fstep: skipping {path}: {e!r}', file=sys.stderr)
            continue
        for bid, fn in getattr(m, 'FRAME_STEP', {}).items():
            if bid in builds: sys.exit(f'gen_fstep: build {bid} twice')
            done = None
            for p0 in (0x4000000, 0x6000000, 0x7000000, 0x7800000):   # the first placeholder above everything the stubs reach
                s0 = p0 + 0x100000
                try: rec = record(fn, p0, s0)
                except AssertionError: continue   # a branch out of reach at this placeholder
                hi = check(bid, *rec[:3], p0, s0)
                if hi < p0: done = (p0, s0) + rec + (hi,); break
            if not done: print(f'gen_fstep: skipping build {bid} ({path}): its stubs reach too far for a patch area after the image', file=sys.stderr); continue
            builds[bid] = done
    return builds

def main(dm, out):
    builds = load(dm)
    def cstr(s): return '"' + s.encode('ascii', 'replace').decode().replace('\\', '\\\\').replace('"', '\\"') + '"'
    def u32s(ws): return '(const uint32_t[]){' + ','.join(f'{w:#010x}' for w in ws) + '}'
    def u8s(b): return '(const uint8_t[]){' + ','.join(str(x) for x in b) + '}'
    L = ['// Generated by tools/gen_fstep.py from nxnative\'s games/*/patches.py FRAME_STEP: do not edit', '#include <stdint.h>', '#include <stddef.h>',
         'typedef struct { uint32_t off, len; const uint8_t *orig; } nxl_fstep_site;',
         'typedef struct { uint32_t off, n; const uint32_t *w; const char *why; } nxl_fstep_put;',
         'typedef struct { const char *build; uint32_t p0, s0, nwords; const uint32_t *words; int render_scale;',
         '                 uint32_t nsites; const nxl_fstep_site *sites; uint32_t nputs; const nxl_fstep_put *puts; } nxl_fstep;',
         'const nxl_fstep nxl_fsteps[] = {']
    for bid in sorted(builds):
        p0, s0, words, sites, puts, rs, _ = builds[bid]
        L.append(f'    {{"{bid}", {p0:#x}, {s0:#x}, {len(words)}, {u32s(words)}, {rs},')
        L.append(f'     {len(sites)}, (const nxl_fstep_site[]){{' + ', '.join(f'{{{o:#x}, {len(b)}, {u8s(b)}}}' for o, b in sites) + '},')
        L.append(f'     {len(puts)}, (const nxl_fstep_put[]){{' + ', '.join(f'{{{o:#x}, {len(ws)}, {u32s(ws)}, {cstr(why)}}}' for o, ws, why in puts) + '}},')
    L += ['};', f'const size_t nxl_nfsteps = {len(builds)};',
          'const char *const nxl_fstep_builds[] = {' + ', '.join(f'"{b}"' for b in sorted(builds)) + '};   // the same builds (the launcher)', '']
    tmp = out + '.tmp'
    with open(tmp, 'w') as f: f.write('\n'.join(L))
    os.replace(tmp, out)
    print(f'gen_fstep: {len(builds)} builds with frame step stubs', file=sys.stderr)

if __name__ == '__main__': main(sys.argv[1], sys.argv[2])
