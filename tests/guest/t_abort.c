// SPDX-License-Identifier: GPL-3.0-or-later
// An SDK-style abort (svcBreak) on purpose: the report must name the functions on the way to it and the host call before.
#include "nx.h"
size_t strlen(const char *);
__attribute__((noinline, visibility("default"))) void abort_inner(void) { svcBreak(0); __asm__ volatile("" ::: "memory"); }
__attribute__((noinline, visibility("default"))) void abort_outer(void) { abort_inner(); __asm__ volatile("" ::: "memory"); }
int test_main(void) {
    puts_("aborting on purpose\n");
    (void)strlen("a host call first");
    abort_outer();
    return 0;
}
