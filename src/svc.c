// SPDX-License-Identifier: GPL-3.0-or-later
// Horizon's supervisor calls. Register use follows the kernel's ABI as libnx's and the SDK's wrappers see it: an SVC
// that returns values puts them in w1/x1 (and x2), with the result code in w0. Outputs through pointers are the
// wrappers' business: they keep the pointer on the stack and store w1 themselves.
// nxnative's dm/host/hle_svc.c has the same dispatch (svc_dispatch(num, regs, ...)) for the Mac, with the fuller
// semantics games need; this file is the start of its port.
#include "nxl.h"
#include <time.h>
#include <sys/random.h>

uint64_t g_program_id = 0x0100000000001000ull;

// ---------- handles ----------
typedef struct { int type; void *obj; } hslot;
static hslot g_handles[4096];
static pthread_mutex_t g_hmu = PTHREAD_MUTEX_INITIALIZER;
#define HANDLE_BASE 0x0C00u   // handle values the guest sees: base | index

// A new kernel handle for an object; 0 when the table is full.
uint32_t handle_new(int type, void *obj) {
    pthread_mutex_lock(&g_hmu);
    for (uint32_t i = 1; i < sizeof g_handles / sizeof *g_handles; i++)
        if (g_handles[i].type == H_FREE) {
            g_handles[i] = (hslot){type, obj};
            pthread_mutex_unlock(&g_hmu);
            return (HANDLE_BASE << 16) | i;
        }
    pthread_mutex_unlock(&g_hmu);
    die("out of handles");
}
// The object behind a handle when it is of the type asked (the pseudo-handle of the current thread too).
void *handle_get(uint32_t h, int type) {
    if (h == CUR_THREAD_HANDLE && type == H_THREAD) return nxl_self();
    if ((h >> 16) != HANDLE_BASE) return NULL;
    uint32_t i = h & 0xFFFF;
    if (i >= sizeof g_handles / sizeof *g_handles) return NULL;
    pthread_mutex_lock(&g_hmu);
    void *o = g_handles[i].type == type || (type < 0 && g_handles[i].type != H_FREE) ? g_handles[i].obj : NULL;
    pthread_mutex_unlock(&g_hmu);
    return o;
}
int handle_type(uint32_t h);
int handle_type(uint32_t h) {
    if (h == CUR_THREAD_HANDLE) return H_THREAD;
    if ((h >> 16) != HANDLE_BASE || (h & 0xFFFF) >= sizeof g_handles / sizeof *g_handles) return H_FREE;
    return g_handles[h & 0xFFFF].type;
}
// for the status lines: what a handle a thread waits on is (an event and whether it is signaled, a thread and its id)
void handle_describe(uint32_t h, char *buf, size_t n);
// A handle described for a report ("thread 12", "event", ...).
void handle_describe(uint32_t h, char *buf, size_t n) {
    int ty = handle_type(h);
    if (ty == H_THREAD) {
        nxl_thread *t = handle_get(h, H_THREAD);
        snprintf(buf, n, t ? "thread %llu%s" : "thread ?", t ? (unsigned long long)t->id : 0ull, t && t->exited ? " (exited)" : "");
    } else if (ty == H_EVENT_R || ty == H_EVENT_W) {
        nxl_event *e = handle_get(h, ty);
        snprintf(buf, n, "event %#x (%s)", h, e && e->signaled ? "signaled" : "not signaled");
    } else snprintf(buf, n, "handle %#x (type %d)", h, ty);
}
// SVC CloseHandle.
Result handle_close(uint32_t h) {
    if ((h >> 16) != HANDLE_BASE) return h == CUR_THREAD_HANDLE || h == CUR_PROCESS_HANDLE ? RES_OK : KERR_INVALID_HANDLE;
    uint32_t i = h & 0xFFFF;
    pthread_mutex_lock(&g_hmu);
    Result r = i < sizeof g_handles / sizeof *g_handles && g_handles[i].type != H_FREE ? RES_OK : KERR_INVALID_HANDLE;
    int was_event_w = !r && g_handles[i].type == H_EVENT_W;
    if (!r) g_handles[i].type = H_FREE;   // the objects themselves stay: a thread may still run after its handle closes
    pthread_mutex_unlock(&g_hmu);
    // a service event bound to this write end (compat.c) is unbound with it: the number is handed out again at once, and
    // a binding left behind sent the service's signals to whatever event got the number next, never to the one the game
    // made again in its place (nxnative's hle_thread.c se_unbind)
    void compat_event_closed(uint32_t wh);
    if (was_event_w) compat_event_closed(h);
    return r;
}

static void unimplemented(CPU *c, uint32_t imm) {
    RED("svc %#x is not implemented (x0 %#llx x1 %#llx, from %#llx)", imm, (unsigned long long)c->x[0],
        (unsigned long long)c->x[1], (unsigned long long)c->pc);
    c->x[0] = KERR_NOT_IMPL;
}

static uint64_t g_entropy[4];
static pthread_once_t g_entropy_once = PTHREAD_ONCE_INIT;
static void entropy_fill(void) {
    if (getrandom(g_entropy, sizeof g_entropy, 0) != sizeof g_entropy) die("getrandom failed");
}

int thread_count_live(void);
static uint64_t g_user_exc_ctx;   // a page in guest memory: newer SDKs' SetDyingMessageRegion writes at +0x1c0
// svcGetInfo, answered as nxnative's hle_svc.c does (its comments give the reasons game by game)
static uint64_t get_info(uint32_t id0, uint32_t handle, uint64_t id1, Result *r) {
    (void)handle;
    *r = RES_OK;
    uint64_t used = mem_heap_used() + g_profile.mem_used + (g_profile.mem_map_limit ? mem_alias_mapped() : 0);
    switch (id0) {
    case 0: return 7;                           // CoreMask: an application's cores 0-2
    case 1: return 0;                           // PriorityMask
    case 2: return g_alias.base;                // AliasRegionAddress
    case 3: return g_alias.size;
    case 4: return g_heap.base;                 // HeapRegionAddress
    case 5: return g_heap.size;
    case 6: case 21: return g_profile.app_pool; // TotalMemorySize, TotalNonSystemMemorySize: the application pool
    case 7: case 22: return used;               // UsedMemorySize: the heap, plus the profile's mem_used
    case 8: return 0;                           // DebuggerAttached
    case 9: return 0;                           // ResourceLimit: no handle
    case 10: return 0;                          // IdleTickCount
    case 11:                                    // RandomEntropy, id1 0-3: fixed for the process, as Horizon's is
        pthread_once(&g_entropy_once, entropy_fill);
        if (id1 > 3) { *r = KERR_INVALID_STATE; return 0; }
        return g_entropy[id1];
    // the address space: one range holding every region, as on Horizon
    case 12: return g_aslr.base;
    case 13: return g_stack.base + g_stack.size - g_aslr.base;
    case 14: return g_stack.base;               // StackRegionAddress
    case 15: return g_stack.size;
    // the NPDM's system resource: the SDK's IsVirtualAddressMemoryEnabled is "total != 0" (with 0 it never sets up its
    // VammManager, and a game that allocates address space through it reads a null manager)
    case 16: return g_profile.sys_resource;
    case 17: return g_profile.sys_resource ? 0x400000 : 0;
    case 18: return g_program_id;               // ProgramId
    case 19: return 0;                          // InitialProcessIdRange
    case 20:                                    // UserExceptionContextAddress
        if (!g_user_exc_ctx) g_user_exc_ctx = mem_stack_alloc(0x1000) - 0x1000;
        return g_user_exc_ctx;
    case 23: return 1;                          // IsApplication
    case 24: { int u = thread_count_live(); return u < 96 ? 96 - u : 0; }   // FreeThreadCount: pm gives an application 96
    case 26: return 1;                          // IsSvcPermitted (19.0.0): every SVC this host has
    case 27: return 0;                          // IoRegionHint (no I/O regions)
    case 28: return 0;                          // AliasRegionExtraSize (18.0.0): none; SDK 20 aborts on an error here
    case 0xF0000002: return 0;                  // ThreadTickCount (an approximation)
    default: {   // an id this host does not know: 0 and success, as nxnative's hle_svc.c answers (an error is fatal to SDKs)
        static uint8_t seen[64];
        if (id0 >= 64 || !seen[id0]) { if (id0 < 64) seen[id0] = 1; RED("GetInfo id %#x/%#llx is not implemented: answering 0", id0, (unsigned long long)id1); }
        return 0;
    }
    }
}

static __thread int t_last_svc = -1;
int svc_last(void) { return t_last_svc; }

// the thread's last SVCs, for abort and crash reports: an answer the SDK rejected shows here
#define SVC_RING 64
typedef struct { uint32_t imm; uint64_t in[4], out[2], lr; } svc_rec;
static __thread svc_rec t_ring[SVC_RING];
static __thread unsigned t_nring;
void svc_print_recent(FILE *f);
// Prints the current thread's last SVCs (for a crash or abort report).
void svc_print_recent(FILE *f) {
    unsigned n = t_nring < SVC_RING ? t_nring : SVC_RING;
    if (!n) return;
    fprintf(f, "  its last %u SVCs (oldest first; x0..x3 in, x0 x1 out, return address):\n", n);
    for (unsigned k = t_nring - n; k < t_nring; k++) {
        const svc_rec *r = &t_ring[k % SVC_RING];
        fprintf(f, "    svc %#04x (%#llx, %#llx, %#llx, %#llx) -> %#llx, %#llx  from %#llx\n", r->imm, (unsigned long long)r->in[0],
                (unsigned long long)r->in[1], (unsigned long long)r->in[2], (unsigned long long)r->in[3], (unsigned long long)r->out[0],
                (unsigned long long)r->out[1], (unsigned long long)r->lr);
    }
}
// NXL_SVC_LOG: every SVC as it returns (1), or only the listed numbers ("0x04,0x06,0x29"), with its caller
static int g_svc_log = -1;
static uint8_t g_svc_want[256];
// Reads NXL_SVC_LOG once: which SVCs to log (1: all, or a list of numbers).
static void svc_log_init(void) {
    const char *e = getenv("NXL_SVC_LOG");
    g_svc_log = e && *e && strcmp(e, "0");
    if (!g_svc_log) return;
    if (!strcmp(e, "1")) { memset(g_svc_want, 1, sizeof g_svc_want); return; }
    for (const char *p = e; *p;) {
        char *end;
        unsigned long v = strtoul(p, &end, 0);
        if (end == p) break;
        if (v < 256) g_svc_want[v] = 1;
        p = *end ? end + 1 : end;
    }
}
// Logs an SVC: its thread, number, arguments, results and the code that made it.
static void svc_log(const svc_rec *r) {
    uint64_t off = 0;
    const char *mod = NULL, *sym = module_symbolize(r->lr, &off, &mod);
    nxl_thread *t = nxl_self();
    fprintf(stderr, "[svc] %llu %#04x (%#llx, %#llx, %#llx, %#llx) -> %#llx, %#llx  from %s+%#llx\n", t ? (unsigned long long)t->id : 0ull, r->imm,
            (unsigned long long)r->in[0], (unsigned long long)r->in[1], (unsigned long long)r->in[2], (unsigned long long)r->in[3],
            (unsigned long long)r->out[0], (unsigned long long)r->out[1], sym ? sym : (mod ? mod : "?"), (unsigned long long)off);
}
static void svc_dispatch(CPU *c, uint32_t imm);
// The entry for every SVC the game makes: noted for reports, dispatched, logged when asked.
void svc_handler(CPU *c, uint32_t imm) {
    if (g_svc_log < 0) svc_log_init();
    t_last_svc = (int)imm;
    nxl_thread *t = nxl_self();
    if (t) t->last_svc = (int)imm, t->svc_arg[0] = c->x[0], t->svc_arg[1] = c->x[1], t->svc_arg[2] = c->x[2], t->in_svc = 1;
    svc_rec *r = &t_ring[t_nring++ % SVC_RING];
    *r = (svc_rec){imm, {c->x[0], c->x[1], c->x[2], c->x[3]}, {0, 0}, c->x[30]};
    if (imm != 0x26) svc_dispatch(c, imm);   // (Break reports, then exits: its record shows no result)
    r->out[0] = c->x[0], r->out[1] = c->x[1];
    if (g_svc_log && g_svc_want[imm & 0xFF]) svc_log(r);
    if (imm == 0x26) svc_dispatch(c, imm);
    if (t) t->in_svc = 0;
}
// Runs an SVC: memory, threads, synchronisation, handles, time, debug output and the rest Horizon gives a game.
static void svc_dispatch(CPU *c, uint32_t imm) {
    uint64_t *x = c->x;
    switch (imm) {
    case 0x01: {   // SetHeapSize(size) -> x1 address
        uint64_t a = 0;
        x[0] = mem_set_heap(x[1], &a);
        x[1] = a;
        break;
    }
    case 0x02: case 0x03: x[0] = RES_OK; break;   // SetMemoryPermission, SetMemoryAttribute: accepted (attributes such as uncached mean nothing here)
    case 0x04: x[0] = mem_map(x[0], x[1], x[2]); break;    // MapMemory(dst, src, size)
    case 0x05: x[0] = mem_unmap(x[0], x[1], x[2]); break;  // UnmapMemory(dst, src, size)
    case 0x06: {   // QueryMemory(MemoryInfo *out, PageInfo *, addr) -> x1 page info
        mem_query(x[2], (MemoryInfo *)x[0]);
        x[0] = RES_OK, x[1] = 0;
        break;
    }
    case 0x07: fflush(stdout); nxl_exit(0);   // ExitProcess
    case 0x08: {   // CreateThread(entry, arg, stack_top, priority, core) -> w1 handle
        if (x[4] > 0x3F) { x[0] = 0xE001; break; }
        nxl_thread *t = thread_new(x[1], x[2], x[3], (int)x[4], (int)(int32_t)x[5]);
        x[0] = RES_OK, x[1] = t->handle;
        break;
    }
    case 0x09: {   // StartThread(handle)
        nxl_thread *t = handle_get((uint32_t)x[0], H_THREAD);
        x[0] = t ? thread_start(t) : KERR_INVALID_HANDLE;
        break;
    }
    case 0x0A: thread_exit_current();   // ExitThread
    case 0x0B: {   // SleepThread(ns): 0, -1 and -2 yield
        int64_t ns = (int64_t)x[0];
        if (ns <= 0) sched_yield();
        else { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, NULL); }
        break;
    }
    case 0x0C: {   // GetThreadPriority(handle) -> w1
        nxl_thread *t = handle_get((uint32_t)x[1], H_THREAD);
        x[0] = t ? RES_OK : KERR_INVALID_HANDLE, x[1] = t ? (uint64_t)t->prio : 0;
        break;
    }
    case 0x0D: {   // SetThreadPriority(handle, priority): kept, not applied (the host scheduler is Linux's)
        nxl_thread *t = handle_get((uint32_t)x[0], H_THREAD);
        if (t) t->prio = (int)x[1];
        x[0] = t ? RES_OK : KERR_INVALID_HANDLE;
        break;
    }
    case 0x0E: {   // GetThreadCoreMask(handle) -> w1 ideal core, x2 mask
        nxl_thread *t = (uint32_t)x[2] == CUR_THREAD_HANDLE ? nxl_self() : handle_get((uint32_t)x[2], H_THREAD);
        if (!t) { x[0] = KERR_INVALID_HANDLE; break; }
        x[0] = RES_OK, x[1] = (uint32_t)t->core, x[2] = t->core_mask;
        break;
    }
    case 0x0F: {   // SetThreadCoreMask(handle, ideal core, mask): -2 = the process default (core 0 alone), -1 = any, -3 = keep the ideal core
        nxl_thread *t = (uint32_t)x[0] == CUR_THREAD_HANDLE ? nxl_self() : handle_get((uint32_t)x[0], H_THREAD);
        if (!t) { x[0] = KERR_INVALID_HANDLE; break; }
        int core = (int)(int32_t)x[1];
        if (core == -2) t->core = 0, t->core_mask = 1;
        else {
            if (core >= 0 && core <= 3) t->core = core;
            if (x[2]) t->core_mask = x[2];
        }
        // an ideal core outside the new mask moves to the lowest core in it, as in Horizon
        if (t->core_mask && !(t->core_mask >> t->core & 1)) t->core = __builtin_ctzll(t->core_mask);
        x[0] = RES_OK;
        break;
    }
    case 0x10: {   // GetCurrentProcessorNumber: the thread's ideal core, as nxnative reports it. Never the host CPU the thread
                   // happens to run on: Super Mario Bros. Wonder indexes its per-core font engines (3) by it, and core 3 crashed
        nxl_thread *t = nxl_self();
        x[0] = t ? (uint32_t)t->core : 0;
        break;
    }
    case 0x11: {   // SignalEvent(handle)
        nxl_event *e = handle_get((uint32_t)x[0], H_EVENT_W);
        if (!e) { x[0] = KERR_INVALID_HANDLE; break; }
        pthread_mutex_lock(&g_sched);
        e->signaled = 1;
        sync_signal_object(e);
        pthread_mutex_unlock(&g_sched);
        x[0] = RES_OK;
        break;
    }
    case 0x12: case 0x17: {   // ClearEvent(handle), ResetSignal(handle)
        nxl_event *e = handle_get((uint32_t)x[0], H_EVENT_W);
        if (!e) e = handle_get((uint32_t)x[0], H_EVENT_R);
        if (!e) { x[0] = KERR_INVALID_HANDLE; break; }
        pthread_mutex_lock(&g_sched);
        Result r = imm == 0x17 && !e->signaled ? KERR_INVALID_STATE : RES_OK;
        e->signaled = 0;
        pthread_mutex_unlock(&g_sched);
        x[0] = r;
        break;
    }
    case 0x16: x[0] = handle_close((uint32_t)x[0]); break;   // CloseHandle
    case 0x18: {   // WaitSynchronization(handles, count, timeout) -> w1 index
        int32_t idx = -1;
        if (x[2] > 0x40) { x[0] = 0xEE01; break; }
        x[0] = sync_wait_handles((const uint32_t *)x[1], (int)x[2], (int64_t)x[3], &idx);
        x[1] = (uint32_t)idx;
        break;
    }
    case 0x19: {   // CancelSynchronization(thread)
        // ends the thread's WaitSynchronization with ResultCancelled, or its next one if it is not in one (mesosphère's
        // KThread::WaitCancel: the flag is kept until a wait takes it). nn::os's multi-wait uses it to wake a thread
        // waiting on kernel handles when a user-mode object linked to the same wait (an event, a message queue) is
        // signaled; ignoring it left such a thread asleep, and with an unbounded wait the game froze
        nxl_thread *t = (uint32_t)x[0] == CUR_THREAD_HANDLE ? nxl_self() : handle_get((uint32_t)x[0], H_THREAD);
        if (!t) { x[0] = KERR_INVALID_HANDLE; break; }
        pthread_mutex_lock(&g_sched);
        t->sync_cancel = 1;
        sync_signal_object(t);
        pthread_mutex_unlock(&g_sched);
        x[0] = RES_OK;
        break;
    }
    case 0x1A: x[0] = sync_arbitrate_lock((uint32_t)x[0], x[1], (uint32_t)x[2]); break;   // ArbitrateLock(owner, addr, self)
    case 0x1B: x[0] = sync_arbitrate_unlock(x[0]); break;                                // ArbitrateUnlock(addr)
    case 0x1C: x[0] = sync_wait_key(x[0], x[1], (uint32_t)x[2], (int64_t)x[3]); break;   // WaitProcessWideKeyAtomic
    case 0x1D: x[0] = sync_signal_key(x[0], (int32_t)x[1]); break;                       // SignalProcessWideKey(key, count)
    case 0x1E: x[0] = guest_ticks(); break;   // GetSystemTick
    case 0x25: {   // GetThreadId(handle) -> x1
        nxl_thread *t = handle_get((uint32_t)x[1], H_THREAD);
        x[0] = t ? RES_OK : KERR_INVALID_HANDLE, x[1] = t ? t->id : 0;
        break;
    }
    case 0x26: {   // Break(reason, buffer, size): the SDK's fatal abort
        fflush(stdout);
        void nxl_break_report(CPU *c);
        fprintf(stderr, "[RED] the game's SDK aborted (svcBreak)\n");
        nxl_break_report(c);
        die("svcBreak(reason %#llx, %#llx, %#llx) from %#llx", (unsigned long long)x[0], (unsigned long long)x[1],
            (unsigned long long)x[2], (unsigned long long)x[30]);
    }
    case 0x27: {   // OutputDebugString(string, size)
        fwrite((const void *)x[0], 1, x[1], stdout);
        fflush(stdout);
        x[0] = RES_OK;
        break;
    }
    case 0x28: die("svcReturnFromException(%#llx): the game hit an exception it could not handle", (unsigned long long)x[0]);
    case 0x29: {   // GetInfo(id0, handle, id1) -> x1
        Result r;
        uint64_t v = get_info((uint32_t)x[1], (uint32_t)x[2], x[3], &r);
        x[0] = r, x[1] = v;
        break;
    }
    case 0x2C: x[0] = mem_map_physical(x[0], x[1], 1); break;   // MapPhysicalMemory(addr, size): in the alias region
    case 0x2D: x[0] = mem_map_physical(x[0], x[1], 0); break;   // UnmapPhysicalMemory(addr, size)
    case 0x34: x[0] = sync_wait_for_address(x[0], (int)x[1], x[2], (int64_t)x[3]); break;               // WaitForAddress(addr, type, value, timeout)
    case 0x35: x[0] = sync_signal_to_address(x[0], (int)x[1], (int32_t)x[2], (int32_t)x[3]); break;      // SignalToAddress(addr, type, value, count)
    case 0x45: {   // CreateEvent -> w1 writable, w2 readable
        nxl_event *e = calloc(1, sizeof *e);
        x[0] = RES_OK;
        x[1] = handle_new(H_EVENT_W, e);
        x[2] = handle_new(H_EVENT_R, e);
        break;
    }
    default: unimplemented(c, imm);
    }
}

// signal (on) or clear (off) an event by a handle of either end, from any host thread (compat/nxnative's service events)
Result kevent_signal(uint32_t h, int on);
// Signals or clears a kernel event (waking its waiters).
Result kevent_signal(uint32_t h, int on) {
    nxl_event *e = handle_get(h, H_EVENT_W);
    if (!e) e = handle_get(h, H_EVENT_R);
    if (!e) return KERR_INVALID_HANDLE;
    pthread_mutex_lock(&g_sched);
    e->signaled = on;
    if (on) sync_signal_object(e);
    pthread_mutex_unlock(&g_sched);
    return RES_OK;
}
