// SPDX-License-Identifier: GPL-3.0-or-later
// What the generated thunks need, and nothing of nxlinux's own names: it is included by src/hle.h (nxlinux's host
// sources) and by compat/nxnative/hle.h (nxnative's), whose dm.h defines module_t, LOG and die differently.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <scarletnx/snx_recompiler.h>

typedef void (*hle_thunk)(CPU *c);   // reads the arguments from the guest registers, calls the host function, writes the result
typedef struct { const char *name; hle_thunk thunk; const char *why; uintptr_t force; } hle_ent;

// calls from host code into guest code
uint64_t guest_callv(uint64_t fn, int n, const uint64_t *args);
uint64_t guest_call_full(uint64_t fn, const uint64_t *ints, V128 *fps, uint64_t *x1);
void *host_callable(uint64_t guest_fn);
uint64_t guest_addr_of(const void *fn);
#define GUEST_CALL(fn, ...) guest_callv((uint64_t)(fn), sizeof((uint64_t[]){__VA_ARGS__}) / 8, (uint64_t[]){__VA_ARGS__})
#define GUEST_CALL0(fn) guest_callv((uint64_t)(fn), 0, NULL)
typedef uint64_t guest_fn;   // a guest function pointer handed to host code

// ---- argument and result marshalling ----
// AAPCS64: integer, pointer and small composite arguments in x0-x7 (a composite of 9-16 bytes takes two, a larger one is
// passed by reference), float and double in v0-v7, the rest on the stack in 8-byte slots. Switch callers do not extend
// arguments narrower than 32 bits, and reading the low bytes into the parameter's own type truncates them as needed.
// Not handled yet: homogeneous floating-point aggregates (a struct of 2-4 floats passed in v registers).
typedef struct { CPU *c; int nx, nv; uint64_t nsaa; } nxl_args;
#define NXL_IS_FP(T) _Generic((T){0}, float: 1, double: 1, default: 0)
static inline void *nxl_arg(nxl_args *a, size_t size, int fp) {
    CPU *c = a->c;
    if (fp) {
        if (a->nv < 8) return &c->v[a->nv++];
        void *p = (void *)a->nsaa; a->nsaa += 8; return p;
    }
    if (size > 16) {   // passed by reference
        void *p;
        if (a->nx < 8) p = (void *)c->x[a->nx++];
        else { p = *(void **)a->nsaa; a->nsaa += 8; }
        return p;
    }
    int regs = size > 8 ? 2 : 1;
    if (a->nx + regs <= 8) { void *p = &c->x[a->nx]; a->nx += regs; return p; }
    a->nx = 8;   // once an argument goes on the stack, every later integer one does too
    void *p = (void *)a->nsaa; a->nsaa += regs * 8; return p;
}
#define NXL_ARGS nxl_args nxl_a = {c, 0, 0, c->x[31]}
#define NXL_ARG(T) (*(T *)nxl_arg(&nxl_a, sizeof(T), NXL_IS_FP(T)))
static inline void nxl_ret(CPU *c, const void *r, size_t size, int fp) {
    if (fp) { memset(&c->v[0], 0, 16); memcpy(&c->v[0], r, size); return; }
    if (size > 16) { memcpy((void *)c->x[8], r, size); return; }   // the caller's result buffer is in x8
    uint64_t lo = 0, hi = 0;
    memcpy(&lo, r, size > 8 ? 8 : size);
    if (size > 8) memcpy(&hi, (const uint8_t *)r + 8, size - 8);
    c->x[0] = lo;
    if (size > 8) c->x[1] = hi;
}
#define NXL_RET(T, v) do { T nxl_r = (v); nxl_ret(c, &nxl_r, sizeof(T), NXL_IS_FP(T)); } while (0)
// A table the linker builds from entries in a section of their own: on Linux (ELF) the linker's __start_<name> and
// __stop_<name> bound it; on Windows (PE) it sorts "<name>$a" < "<name>$m" < "<name>$z", and src/sections_win32.c puts
// those two bounds in $a and $z
#ifdef _WIN32
#define NXL_SECTION(name) name "$m"
#else
#define NXL_SECTION(name) name
#endif
#define NXL_REGISTER(mangled, thunk, why, force) \
    __attribute__((used, retain, section(NXL_SECTION("nxl_hle")), aligned(8))) static const hle_ent nxl_ent_##thunk = {mangled, thunk, why, force}
