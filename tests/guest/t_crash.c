// SPDX-License-Identifier: GPL-3.0-or-later
// A guest crash on purpose: the report must name the guest function it happened in and its caller.
#include "nx.h"
size_t strlen(const char *);
struct obj { char pad[0xc0]; long field; };
__attribute__((noinline, visibility("default"))) long crash_here(struct obj *o) { return o->field + 1; }
__attribute__((noinline, visibility("default"))) long crash_caller(struct obj *o) { long r = crash_here(o); __asm__ volatile("" ::: "memory"); return r + 2; }
int test_main(void) {
    puts_("crashing on purpose\n");
    (void)strlen("a host call first");
    return (int)crash_caller((struct obj *)0);
}
