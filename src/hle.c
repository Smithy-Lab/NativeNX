// SPDX-License-Identifier: GPL-3.0-or-later
// The registry of host implementations. Every hle_ent the generated thunks place in the "nxl_hle" section gets a stub
// address in the reserved HLE region; the loader binds imports to those addresses. A call to one reaches the recompiler's
// miss hook (no guest code is there), which runs the thunk and returns to the caller's x30.
#include "nxl.h"
#include <stdatomic.h>

extern const hle_ent __start_nxl_hle[], __stop_nxl_hle[];
static const hle_ent **g_ents;   // sorted by name; index i has the stub NXL_HLE_BASE + 4 * i
static size_t g_nents;
// imports nothing implements: each gets its own stub after the implemented ones, so a call reports the name
static const char **g_missing;
// NXL_TRACE: guest functions one module imports from another, logged on every call (stubs from index TRACE0 on)
#define TRACE0 0x30000u
#define TRACE_MAX 0xFFF0u
static const char *g_trace_name[TRACE_MAX];
static uint64_t g_trace_target[TRACE_MAX];
static unsigned g_ntrace;
static uint8_t *g_missing_mode;   // MISS_FATAL, or a soft stub (hle_soft_stub): returns 0, reported once or quiet
static _Atomic uint8_t *g_missing_seen;
static size_t g_nmissing, g_capmissing;
enum { MISS_FATAL, MISS_REPORT, MISS_QUIET };
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static int by_name(const void *a, const void *b) {   // by name, a forced implementation before a plain one
    const hle_ent *x = *(const hle_ent *const *)a, *y = *(const hle_ent *const *)b;
    int c = strcmp(x->name, y->name);
    return c ? c : (int)(y->force != 0) - (int)(x->force != 0);
}

// Collects the host stand-ins (the HLE section) and sorts them by name for lookups.
void hle_init(void) {
    g_nents = __stop_nxl_hle - __start_nxl_hle;
    g_ents = malloc((g_nents + 1) * sizeof *g_ents);
    for (size_t i = 0; i < g_nents; i++) g_ents[i] = &__start_nxl_hle[i];
    qsort(g_ents, g_nents, sizeof *g_ents, by_name);
    // the same name twice: a forced implementation replaces a plain one (nxnative's hle_time.c forces localtime_r,
    // which the libc list also serves); two forced ones are a mistake
    size_t n = 0;
    for (size_t i = 0; i < g_nents; i++) {
        if (n && !strcmp(g_ents[i]->name, g_ents[n - 1]->name)) {
            if (g_ents[i]->force) die("two forced host implementations of %s", g_ents[i]->name);
            continue;
        }
        g_ents[n++] = g_ents[i];
    }
    g_nents = n;
    LOG("%zu host implementations", g_nents);
}
size_t hle_count(void) { return g_nents; }

// The guest address of a host stand-in for a function name (force_only: only those that replace the game's own code); 0
// when none.
uint64_t hle_lookup(const char *name, int force_only) {
    size_t lo = 0, hi = g_nents;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(name, g_ents[mid]->name);
        if (!c) return force_only && !g_ents[mid]->force ? 0 : NXL_HLE_BASE + 4 * mid;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

// An address for an import nothing provides: calling it reports the import by name (once per name and kind).
static uint64_t missing_stub(const char *name, int mode) {
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_nmissing; i++)
        if (g_missing_mode[i] == mode && !strcmp(g_missing[i], name)) { pthread_mutex_unlock(&g_mu); return NXL_HLE_BASE + 4 * (g_nents + i); }
    if (!g_missing) {   // sized for the whole stub region once: no reallocation while other threads read them
        g_capmissing = TRACE0;   // below the trace stubs
        g_missing = calloc(g_capmissing, sizeof *g_missing);
        g_missing_mode = calloc(g_capmissing, 1);
        g_missing_seen = calloc(g_capmissing, 1);
    }
    if (g_nents + g_nmissing >= g_capmissing - 1) die("HLE stub region full");
    g_missing[g_nmissing] = strdup(name);
    g_missing_mode[g_nmissing] = (uint8_t)mode;
    uint64_t a = NXL_HLE_BASE + 4 * (g_nents + g_nmissing++);
    pthread_mutex_unlock(&g_mu);
    if (a >= NXL_STOP_ADDR) die("HLE stub region full");
    return a;
}
uint64_t hle_missing(const char *name) { return missing_stub(name, MISS_FATAL); }
// a stub for a function the host knows of but does not implement (NVN's unimplemented entry points): returns 0, and
// unless quiet reports the name the first time it is called (nxnative's nvn_unimpl_entry does the same)
uint64_t hle_soft_stub(const char *name, int quiet) { return missing_stub(name, quiet ? MISS_QUIET : MISS_REPORT); }

#include <fnmatch.h>
// whether NXL_TRACE (comma-separated shell patterns of symbol names) asks for this function
static int trace_wanted(const char *name) {
    static const char *pats;
    static int init;
    if (!init) init = 1, pats = getenv("NXL_TRACE");
    if (!pats || !*pats) return 0;
    char buf[1024];
    for (const char *p = pats; *p;) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && n < sizeof buf) {
            memcpy(buf, p, n), buf[n] = 0;
            if (!fnmatch(buf, name, 0)) return 1;
        }
        p += n + (e != NULL);
    }
    return 0;
}
// When an import's calls are traced (NXL_TRACE): a stub that logs each call and goes on to the real target.
uint64_t hle_trace_stub(const char *name, uint64_t target) {
    if (!trace_wanted(name)) return target;
    pthread_mutex_lock(&g_mu);
    if (g_ntrace == TRACE_MAX) { pthread_mutex_unlock(&g_mu); return target; }
    for (unsigned i = 0; i < g_ntrace; i++)
        if (g_trace_target[i] == target) { pthread_mutex_unlock(&g_mu); return NXL_HLE_BASE + 4ull * (TRACE0 + i); }
    unsigned i = g_ntrace++;
    g_trace_name[i] = strdup(name), g_trace_target[i] = target;
    pthread_mutex_unlock(&g_mu);
    return NXL_HLE_BASE + 4ull * (TRACE0 + i);
}
static __thread int t_trace_depth;
// Runs a traced call: logged, then the real function called.
static int trace_dispatch(CPU *c, size_t i) {
    if (i < TRACE0 || i - TRACE0 >= g_ntrace) return 0;
    unsigned k = (unsigned)(i - TRACE0);
    nxl_thread *self = nxl_self();
    uint64_t lr = c->x[30];
    module_t *m = module_of(lr);
    int d = t_trace_depth++;
    fprintf(stderr, "[trace] %llu %*s> %s(%#llx, %#llx, %#llx, %#llx, %#llx, %#llx) from %s+%#llx\n", self ? (unsigned long long)self->id : 0ull, 2 * d, "",
            g_trace_name[k], (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
            (unsigned long long)c->x[4], (unsigned long long)c->x[5], m ? m->name : "?", m ? (unsigned long long)(lr - (uint64_t)(uintptr_t)m->base) : (unsigned long long)lr);
    nxl_call_through(c, g_trace_target[k]);
    t_trace_depth--;
    fprintf(stderr, "[trace] %llu %*s< %s = %#llx\n", self ? (unsigned long long)self->id : 0ull, 2 * d, "", g_trace_name[k], (unsigned long long)c->x[0]);
    c->pc = lr;
    return 1;
}

// The name of the host stand-in at an address, or NULL.
const char *hle_name_of(uint64_t pc) {
    if (pc < NXL_HLE_BASE || pc >= NXL_STOP_ADDR || (pc & 3)) return NULL;
    size_t i = (pc - NXL_HLE_BASE) / 4;
    if (i >= TRACE0) return i - TRACE0 < g_ntrace ? g_trace_name[i - TRACE0] : NULL;
    if (i < g_nents) return g_ents[i]->name;
    if (i - g_nents < g_nmissing) return g_missing[i - g_nents];
    return NULL;
}

static __thread const char *t_last_hle;
const char *hle_last_call(void) { return t_last_hle; }

// the calling thread's last host calls, for crash reports: name, return address, first arguments and the result
#define RECENT 24
typedef struct { const char *name; uint64_t lr, a[4], r; } recent_call;
static __thread recent_call t_recent[RECENT];
static __thread unsigned t_nrecent;
// Prints the current thread's last host calls (for a crash or abort report).
void hle_print_recent(FILE *f) {
    unsigned n = t_nrecent < RECENT ? t_nrecent : RECENT;
    if (!n) return;
    fprintf(f, "  its last %u host calls (oldest first; x0..x3 in, x0 out, return address):\n", n);
    for (unsigned k = t_nrecent - n; k < t_nrecent; k++) {
        recent_call *e = &t_recent[k % RECENT];
        module_t *m = module_of(e->lr);
        fprintf(f, "    %-48s (%#llx, %#llx, %#llx, %#llx) -> %#llx  from %#llx", e->name, (unsigned long long)e->a[0], (unsigned long long)e->a[1],
                (unsigned long long)e->a[2], (unsigned long long)e->a[3], (unsigned long long)e->r, (unsigned long long)e->lr);
        if (m) fprintf(f, " (%s+%#llx)", m->name, (unsigned long long)(e->lr - (uint64_t)(uintptr_t)m->base));
        fputc('\n', f);
    }
}

// The guest reached a host stand-in's address: runs it (or reports a missing import); 1 when the address was one.
int hle_dispatch(CPU *c, uint64_t pc) {
    if (pc < NXL_HLE_BASE || pc >= NXL_STOP_ADDR || (pc & 3)) return 0;
    size_t i = (pc - NXL_HLE_BASE) / 4;
    if (trace_dispatch(c, i)) return 1;
    if (i >= g_nents) {
        const char *name = hle_name_of(pc);
        size_t k = i - g_nents;
        if (name && g_missing_mode[k] != MISS_FATAL) {   // a soft stub: return 0
            if (g_missing_mode[k] == MISS_REPORT && !atomic_exchange(&g_missing_seen[k], 1))
                RED("unimplemented function called: %s (x0=%#llx x1=%#llx x2=%#llx, from %#llx)", name, (unsigned long long)c->x[0],
                    (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[30]);
            c->x[0] = 0;
            c->pc = c->x[30];
            return 1;
        }
        die("the game called %s, which no module defines and the host does not implement (from %#llx)",
            name ? name : "an unknown host stub", (unsigned long long)c->x[30]);
    }
    uint64_t ret = c->x[30];
    t_last_hle = g_ents[i]->name;
    recent_call *e = &t_recent[t_nrecent % RECENT];
    *e = (recent_call){g_ents[i]->name, ret, {c->x[0], c->x[1], c->x[2], c->x[3]}, 0};
    t_nrecent++;
    nxl_thread *self = nxl_self();
    if (self) self->last_hle = g_ents[i]->name, self->in_hle = 1;
    g_ents[i]->thunk(c);
    if (self) self->in_hle = 0;
    e->r = c->x[0];
    c->pc = ret;   // return to the caller, as the guest function would have
    return 1;
}
