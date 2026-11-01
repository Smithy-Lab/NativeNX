// SPDX-License-Identifier: GPL-3.0-or-later
// The parts of nxnative's host API (dm/host/dm.h, hle.h) that its service sources call, implemented on nxlinux.
// On the Mac these live in files tied to its way of running guest code (hle_thread.c, loader.c, hle_os.c); the
// signatures are nxnative's. This file is compiled against nxlinux's headers, not nxnative's, so it spells their types
// out as plain C.
#include "nxl.h"
#include <sys/random.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdatomic.h>
#include <time.h>

// ---------- clocks, sleep, entropy ----------
uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
uint64_t guest_ns(void) { return now_ns(); }   // nxnative's guest clock can freeze (background, keyboard): not yet here
void dm_guest_sleep_ns(int64_t ns) {
    if (ns <= 0) { sched_yield(); return; }
    struct timespec ts = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    nanosleep(&ts, NULL);
}
uint64_t dm_entropy64(void) {
    uint64_t v;
    if (getrandom(&v, sizeof v, 0) != sizeof v) die("getrandom failed");
    return v;
}

// ---------- guest symbols: functions come back as native pointers (host_callable) ----------
// nxnative's code calls what guest_sym returns as a C function; for data symbols the address itself is returned.
void *dm_compat_guest_sym(const char *name);
void *dm_compat_guest_sym(const char *name) {
    uint64_t a = guest_sym(name);
    if (!a) return NULL;
    return guest_sym_is_func(name) ? host_callable(a) : (void *)a;
}

// ---------- guest object -> host object map (nxnative's objmap) ----------
typedef struct { const void *k; void *v; } om_ent;
static om_ent *g_om;
static size_t g_om_cap, g_om_n;
static pthread_mutex_t g_om_mu = PTHREAD_MUTEX_INITIALIZER;
static size_t om_slot(const void *k) {
    size_t i = ((uintptr_t)k >> 3) * 0x9E3779B97F4A7C15ull & (g_om_cap - 1);
    while (g_om[i].k && g_om[i].k != k) i = (i + 1) & (g_om_cap - 1);
    return i;
}
void *objmap_get(const void *k) {
    pthread_mutex_lock(&g_om_mu);
    void *v = g_om_cap ? g_om[om_slot(k)].v : NULL;
    pthread_mutex_unlock(&g_om_mu);
    return v;
}
// A pointer to pointer map (open addressing, grown as it fills) used by nxnative's code: sets a key.
void objmap_put(const void *k, void *v) {
    pthread_mutex_lock(&g_om_mu);
    if ((g_om_n + 1) * 2 > g_om_cap) {   // grow and rehash
        om_ent *old = g_om;
        size_t oc = g_om_cap;
        g_om_cap = oc ? oc * 2 : 1024;
        g_om = calloc(g_om_cap, sizeof *g_om);
        for (size_t i = 0; i < oc; i++) if (old[i].k) g_om[om_slot(old[i].k)] = old[i];
        free(old);
    }
    size_t i = om_slot(k);
    if (!g_om[i].k) g_om_n++;
    g_om[i] = (om_ent){k, v};
    pthread_mutex_unlock(&g_om_mu);
}
// Removes a key from the pointer map; its value.
void *objmap_del(const void *k) {
    pthread_mutex_lock(&g_om_mu);
    void *v = NULL;
    if (g_om_cap) {
        size_t i = om_slot(k);
        if (g_om[i].k) {
            v = g_om[i].v;
            g_om[i] = (om_ent){0};
            g_om_n--;
            // re-insert the cluster after the hole (linear probing)
            for (size_t j = (i + 1) & (g_om_cap - 1); g_om[j].k; j = (j + 1) & (g_om_cap - 1)) {
                om_ent e = g_om[j];
                g_om[j] = (om_ent){0};
                g_om[om_slot(e.k)] = e;
            }
        }
    }
    pthread_mutex_unlock(&g_om_mu);
    return v;
}

// ---------- service-layer SystemEvents (nxnative's hle_thread.c, ported) ----------
// A service hands its client an event the client waits on through the SDK's nn::os. As nxnative does: on a guest
// thread, call the SDK's own CreateSystemEvent (inter-process), which makes a kernel event through svcCreateEvent; keep
// its writable handle, which any thread can then signal.
Result kevent_signal(uint32_t wh, int on);
typedef struct { void *ev; uint32_t wh; uint64_t id; } se_bind;
static se_bind g_se[1024];
static int g_nse;
static uint64_t g_next_se = 1;
static pthread_mutex_t g_se_mu = PTHREAD_MUTEX_INITIALIZER;
static se_bind *se_find(void *ev) {
    for (int i = 0; i < g_nse; i++) if (g_se[i].ev == ev) return &g_se[i];
    return NULL;
}
// Binds a guest system event to a kernel event here, so the game can wait on it with WaitSynchronization.
void hle_bind_system_event(void *ev, int autoclear) {
    pthread_mutex_lock(&g_se_mu);
    se_bind *b = se_find(ev);
    pthread_mutex_unlock(&g_se_mu);
    if (b) return;
    if (!nxl_self()) die("hle_bind_system_event must be called on a guest thread");
    static uint64_t create, getw;
    if (!create) {
        create = guest_sym("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
        getw = guest_sym("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE");
        if (!create || !getw) die("the SDK has no nn::os::CreateSystemEvent: cannot bind a service event");
    }
    uint32_t r = (uint32_t)GUEST_CALL(create, (uint64_t)ev, (uint64_t)autoclear, 1);
    if (r) die("CreateSystemEvent failed %#x", r);
    uint32_t wh = (uint32_t)GUEST_CALL(getw, (uint64_t)ev);
    pthread_mutex_lock(&g_se_mu);
    if (g_nse == (int)(sizeof g_se / sizeof *g_se)) die("SystemEvent binding table full");
    g_se[g_nse++] = (se_bind){ev, wh, g_next_se++};
    pthread_mutex_unlock(&g_se_mu);
}
// CloseHandle of an event's write end (svc.c): the bindings to it go (a SystemEvent destroyed and made again at the same
// address is bound again, to its new kernel event)
void compat_event_closed(uint32_t wh);
void compat_event_closed(uint32_t wh) {
    pthread_mutex_lock(&g_se_mu);
    for (int i = 0; i < g_nse;) if (g_se[i].wh == wh) g_se[i] = g_se[--g_nse]; else i++;
    pthread_mutex_unlock(&g_se_mu);
}
// The id of a system event's binding, or 0.
uint64_t hle_system_event_binding(void *ev) {
    pthread_mutex_lock(&g_se_mu);
    se_bind *b = se_find(ev);
    uint64_t id = b ? b->id : 0;
    pthread_mutex_unlock(&g_se_mu);
    return id;
}
// Signals the kernel event of a binding by its id; 1 when there was one.
int hle_signal_system_event_binding(uint64_t id) {
    uint32_t wh = 0;
    pthread_mutex_lock(&g_se_mu);
    for (int i = 0; i < g_nse; i++) if (g_se[i].id == id) wh = g_se[i].wh;
    pthread_mutex_unlock(&g_se_mu);
    return wh && kevent_signal(wh, 1) == RES_OK;
}
// Signals the kernel event bound to a system event; 1 when there was one.
int hle_signal_bound_system_event(void *ev) {
    pthread_mutex_lock(&g_se_mu);
    se_bind *b = se_find(ev);
    uint32_t wh = b ? b->wh : 0;
    pthread_mutex_unlock(&g_se_mu);
    return wh && kevent_signal(wh, 1) == RES_OK;
}
void hle_signal_system_event(void *ev) {   // an unbound one is bound on the spot (manual clear), on a guest thread only
    if (hle_signal_bound_system_event(ev)) return;
    if (!nxl_self()) return;
    hle_bind_system_event(ev, 0);
    hle_signal_bound_system_event(ev);
}
// Clears the kernel event bound to a system event.
void hle_clear_system_event(void *ev) {
    pthread_mutex_lock(&g_se_mu);
    se_bind *b = se_find(ev);
    uint32_t wh = b ? b->wh : 0;
    pthread_mutex_unlock(&g_se_mu);
    if (wh) kevent_signal(wh, 0);
}

// nxnative's hle_thread.c: the calling guest thread's handle (0 off guest threads)
uint32_t thr_cur_handle(void);
uint32_t thr_cur_handle(void) { nxl_thread *t = nxl_self(); return t ? t->handle : 0; }

// Host mappings the game may see (linux_shim.h): a 64 GiB window at 256 GiB, below Horizon's 39-bit limit and clear of
// the guest's regions (modules, alias, heap, stacks: 32-144 GiB) and of the HLE stubs and trampolines near 512 GiB
#define HOSTMAP_BASE 0x4000000000ull
#define HOSTMAP_SIZE 0x1000000000ull
static _Atomic uint64_t g_hostmap_next = HOSTMAP_BASE;
#undef mmap
// mmap for nxnative's host code: 64 KiB aligned and out of the guest's regions (retried at other places when it lands
// in one).
void *nxl_host_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    if (addr || (flags & MAP_FIXED) || !len) return mmap(addr, len, prot, flags, fd, off);
    uint64_t size = (len + 0xFFFF) & ~0xFFFFull;   // 64 KiB granules, as Horizon's
    for (int tries = 0; tries < 64; tries++) {
        uint64_t a = atomic_fetch_add(&g_hostmap_next, size + 0x10000);   // a guard gap after each
        if (a + size > HOSTMAP_BASE + HOSTMAP_SIZE) break;
        void *p = mmap((void *)(uintptr_t)a, len, prot, flags | MAP_FIXED_NOREPLACE, fd, off);
        if (p != MAP_FAILED && (uint64_t)(uintptr_t)p == a) return p;
        if (p != MAP_FAILED) munmap(p, len);   // an old kernel took it as a hint
        else if (errno != EEXIST) return MAP_FAILED;
    }
    return mmap(NULL, len, prot, flags, fd, off);   // window full: wherever the kernel puts it
}
