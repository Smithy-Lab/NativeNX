// SPDX-License-Identifier: GPL-3.0-or-later
// A thread spinning in guest code past a -v status line (every 10 s): the line must say which function it is in.
#include "nx.h"
static volatile u64 sink;
__attribute__((noinline, visibility("default"))) void spin_here(u64 until) {
    while (svcGetSystemTick() < until) for (int k = 0; k < 200000; k++) sink += (u64)k;   // mostly guest code: a sample rarely lands in the SVC
}
int test_main(void) {
    spin_here(svcGetSystemTick() + 12ull * 19200000ull);   // 12 s at 19.2 MHz
    puts_("ok - spun for 12 s\n");
    return 0;
}
