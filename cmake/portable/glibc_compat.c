// SPDX-License-Identifier: GPL-3.0-or-later
// The functions of newer glibc versions that the static C++ runtime (libstdc++.a) and the recompiler's library call,
// for a portable build (glibc_compat.h): thin versions over the older ones every glibc 2.34 has. _dl_find_object (2.35)
// is the C++ exception unwinder's: here from dl_iterate_phdr, as the unwinder did before it.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>

long __isoc23_strtol(const char *s, char **e, int b) { return strtol(s, e, b); }
long long __isoc23_strtoll(const char *s, char **e, int b) { return strtoll(s, e, b); }
unsigned long __isoc23_strtoul(const char *s, char **e, int b) { return strtoul(s, e, b); }
unsigned long long __isoc23_strtoull(const char *s, char **e, int b) { return strtoull(s, e, b); }
int __isoc23_sscanf(const char *s, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = vsscanf(s, f, ap);
    va_end(ap);
    return r;
}
int __isoc23_fscanf(FILE *fp, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = vfscanf(fp, f, ap);
    va_end(ap);
    return r;
}
uint32_t arc4random(void) {   // (glibc 2.36): from the kernel's random source
    uint32_t v = 0;
    while (getrandom(&v, sizeof v, 0) != (ssize_t)sizeof v) {}
    return v;
}

// _dl_find_object (glibc 2.35): the loaded object that holds an address, and its unwind table (PT_GNU_EH_FRAME)
typedef struct { void *pc; struct dl_find_object *r; int found; } dlfo_q;
static int dlfo_each(struct dl_phdr_info *info, size_t size, void *arg) {
    (void)size;
    dlfo_q *q = arg;
    uintptr_t pc = (uintptr_t)q->pc, lo = UINTPTR_MAX, hi = 0;
    int in = 0;
    void *eh = NULL;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        uintptr_t a = info->dlpi_addr + ph->p_vaddr;
        if (ph->p_type == PT_LOAD) {
            if (a < lo) lo = a;
            if (a + ph->p_memsz > hi) hi = a + ph->p_memsz;
            if (pc >= a && pc < a + ph->p_memsz) in = 1;
        } else if (ph->p_type == PT_GNU_EH_FRAME) eh = (void *)a;
    }
    if (!in) return 0;
    q->r->dlfo_flags = 0, q->r->dlfo_map_start = (void *)lo, q->r->dlfo_map_end = (void *)hi;
    q->r->dlfo_link_map = NULL, q->r->dlfo_eh_frame = eh;
    q->found = 1;
    return 1;
}
int _dl_find_object(void *pc, struct dl_find_object *r) {
    dlfo_q q = {pc, r, 0};
    dl_iterate_phdr(dlfo_each, &q);
    return q.found ? 0 : -1;
}
