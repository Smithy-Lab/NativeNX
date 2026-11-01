// SPDX-License-Identifier: GPL-3.0-or-later
// Two modules that import from each other, as a game's main, subsdk and sdk modules do.
#include "nx.h"
long lib_scale(long);              // defined in t_multi_lib
extern long lib_counter;           // a variable defined in t_multi_lib (GLOB_DAT)
long (*lib_get_callback(void))(long);
__attribute__((visibility("default"))) long main_offset(long v) { return v + 100; }   // the library imports this back
int test_main(void) {
    CHECK("call into the other module", lib_scale(5) == 5 * 3 + 100);
    lib_counter = 41;
    CHECK("shared variable across modules", lib_scale(0) == 100 + 41 * 0 && lib_counter == 42);
    CHECK("function pointer from the other module", lib_get_callback()(4) == 16);
    return 0;
}
