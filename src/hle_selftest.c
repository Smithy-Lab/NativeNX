// SPDX-License-Identifier: GPL-3.0-or-later
// Host functions the guest test programs (tests/guest) call, one per argument-passing case of AAPCS64, so the
// generated thunks are checked against code clang compiled for the Switch's ABI. Nothing else uses them.
#include "hle.h"
// (the guest's long is 64 bits: int64_t here, as the host's long is 32 bits on Windows)

struct pair { int64_t a, b; };
struct big { int64_t v[4]; };

HLE("nxltest_sum9", int64_t, t_sum9, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g, int64_t h, int64_t i) {
    return a + b + c + d + e + f + g + h + i * 1000;   // i is the first stack argument
}
HLE("nxltest_mix", double, t_mix, int a, double b, float c, char d, int64_t e, double f) {
    return a + b + c + d + e + f;
}
HLE("nxltest_fsum10", float, t_fsum10, float a, float b, float c, float d, float e, float f, float g, float h, float i, float j) {
    return a + b + c + d + e + f + g + h + i * 100 + j * 1000;   // i and j on the stack
}
HLE("nxltest_u8", unsigned char, t_u8, unsigned v) { return (unsigned char)(v + 1); }
HLE("nxltest_neg", int, t_neg, int v) { return -v; }
HLE("nxltest_pair", struct pair, t_pair, int64_t a, int64_t b) { return (struct pair){a * 2, b * 3}; }
HLE("nxltest_pairsum", int64_t, t_pairsum, int64_t x, struct pair p, int64_t y) { return x + p.a * 10 + p.b * 100 + y * 1000; }
HLE("nxltest_big", struct big, t_big, int64_t base) { return (struct big){{base, base + 1, base + 2, base + 3}}; }
HLE("nxltest_bigsum", int64_t, t_bigsum, struct big b, int64_t k) { return (b.v[0] + b.v[1] + b.v[2] + b.v[3]) * k; }
HLE("nxltest_strdup_upper", char *, t_upper, char *dst, const char *src) {   // host code writing guest memory
    char *d = dst;
    while (*src) *d++ = (*src >= 'a' && *src <= 'z') ? *src - 32 : *src, src++;
    *d = 0;
    return dst;
}
// a guest callback that itself calls host functions: guest -> host -> guest -> host
HLE("nxltest_twice", int64_t, t_twice, guest_fn fn, int64_t x) { return (int64_t)GUEST_CALL(fn, x) + (int64_t)GUEST_CALL(fn, x + 1); }
// a forced implementation: the guest test program defines nxltest_forced itself (returning 1); this one must win
HLE_FORCE("nxltest_forced", "the selftest of HLE_FORCE's precedence over a module's definition", int, t_forced, void) { return 7; }
HLE("nxltest_tid", uint64_t, t_tid, void) { nxl_thread *t = nxl_self(); return t ? t->id : 0; }
// host code calling guest functions through plain C function pointers (host_callable), as nxnative's code does
HLE("nxltest_native_call", double, t_native_call, const char *name) {
    double (*f)(int64_t, double, int, float) = host_callable(guest_sym(name));
    return f(3, 0.5, 4, 0.25f);
}
HLE("nxltest_native_pair", int64_t, t_native_pair, const char *name) {
    struct pair (*f)(int64_t, int64_t) = host_callable(guest_sym(name));
    struct pair p = f(6, 7);
    return p.a * 100 + p.b;
}
HLE("nxltest_native_call1", int64_t, t_native_call1, const char *name, int64_t v) {   // one integer argument and result
    int64_t (*f)(int64_t) = host_callable(guest_sym(name));
    return f(v);
}
HLE("nxltest_host_windows", int, t_host_windows, void) {   // what the host-callable stubs cannot do there (tramp_win64.S)
#ifdef _WIN32
    return 1;
#else
    return 0;
#endif
}
// a stub address handed to the guest, which calls it: the miss hook must continue at the guest function behind it
HLE("nxltest_get_callable", guest_fn, t_get_callable, const char *name) { return (guest_fn)host_callable(guest_sym(name)); }
// a host function handed to the guest, which calls it (as the SDK calls a host thread entry nxnative passes it)
static int64_t host_add3(int64_t a, int64_t b, int64_t c) { return a * 100 + b * 10 + c; }
HLE("nxltest_host_fnptr", guest_fn, t_host_fnptr, void) { return (guest_fn)host_add3; }
