// SPDX-License-Identifier: GPL-3.0-or-later
// The library half of the two-module test: no harness, no entry of its own.
long main_offset(long);            // imported from the main module
__attribute__((visibility("default"))) long lib_counter;
static long sq(long v) { return v * v; }
__attribute__((visibility("default"))) long lib_scale(long v) { lib_counter++; return main_offset(v * 3); }
__attribute__((visibility("default"))) long (*lib_get_callback(void))(long) { return sq; }
