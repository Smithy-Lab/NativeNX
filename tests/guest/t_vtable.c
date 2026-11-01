// SPDX-License-Identifier: GPL-3.0-or-later
// Functions reached only through a table of pointers in data, as C++ virtual functions are: the recompile must find them
// from the table (cpu.c, code addresses in data), so none is compiled at run time.
#include "nx.h"
static __attribute__((noinline)) int v_add(int a, int b) { return a + b; }
static __attribute__((noinline)) int v_mul(int a, int b) { return a * b; }
static __attribute__((noinline)) int v_sub(int a, int b) { return a - b; }
typedef int (*op_fn)(int, int);
struct vtable { op_fn op[3]; };
static const struct vtable table = {{v_add, v_mul, v_sub}};
static const struct vtable *volatile vt = &table;   // opaque to the compiler: only data leads to the functions
int test_main(void) {
    int r = vt->op[0](2, 3) + vt->op[1](4, 5) + vt->op[2](9, 1);   // 5 + 20 + 8
    puts_(r == 33 ? "ok - functions reached only through a table\n" : "not ok - functions reached only through a table\n");
    return r == 33 ? 0 : 1;
}
