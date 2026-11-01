// SPDX-License-Identifier: GPL-3.0-or-later
// A global set in one function and read in another, through its GOT slot (default visibility): --xref must list both and
// mark the one that writes it.
#include "nx.h"
__attribute__((visibility("default"))) void *g_lock_ptr;
static int lock_obj;
__attribute__((noinline, visibility("default"))) void xref_setter(void) { g_lock_ptr = &lock_obj; }
__attribute__((noinline, visibility("default"))) int xref_reader(void) { return g_lock_ptr != 0; }
int test_main(void) {
    xref_setter();
    return xref_reader() ? 0 : 1;
}
