// SPDX-License-Identifier: GPL-3.0-or-later
// Exclusive pairs of different sizes, as compilers emit them: Breath of the Wild 1.9.0 increments a 32-bit counter with
// ldxr x / add w / stxr w (main+0x1e1d88). The CPU lets a store-exclusive narrower than its load-exclusive, to the same
// address, succeed; the recompiler failed it every time, and the loop never ended.
#include "nx.h"
static u64 counter64 __attribute__((aligned(8)));
static u32 incr_mixed(u64 *p) {   // returns the value before
    u64 old;
    u32 fail;
    do {
        __asm__ volatile("ldxr %0, [%2]\n\tadd w9, %w0, #1\n\tstxr %w1, w9, [%2]" : "=&r"(old), "=&r"(fail) : "r"(p) : "x9", "memory");
    } while (fail);
    return (u32)old;
}
static u32 incr_same(u32 *p) {
    u32 old, fail;
    do {
        __asm__ volatile("ldxr %w0, [%2]\n\tadd w9, %w0, #1\n\tstxr %w1, w9, [%2]" : "=&r"(old), "=&r"(fail) : "r"(p) : "x9", "memory");
    } while (fail);
    return old;
}
static u32 counter32;
int test_main(void) {
    counter64 = 0xAAAAAAAA00000005ull;   // the upper half must stay as it is
    u32 before = incr_mixed(&counter64);
    u32 before2 = incr_mixed(&counter64);
    int ok = before == 5 && before2 == 6 && counter64 == 0xAAAAAAAA00000007ull;
    puts_(ok ? "ok - ldxr x / stxr w (a narrower store-exclusive) succeeds and stores the low half\n"
             : "not ok - ldxr x / stxr w (a narrower store-exclusive) succeeds and stores the low half\n");
    counter32 = 41;
    int ok2 = incr_same(&counter32) == 41 && counter32 == 42;
    puts_(ok2 ? "ok - ldxr w / stxr w\n" : "not ok - ldxr w / stxr w\n");
    return ok && ok2 ? 0 : 1;
}
