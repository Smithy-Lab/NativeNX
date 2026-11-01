// SPDX-License-Identifier: GPL-3.0-or-later
// Horizon's synchronization SVCs. A guest mutex is a 32-bit word in guest memory: 0 when free, else the owner's thread
// handle, with bit 30 set when other threads wait. The guest locks and unlocks it with its own atomics and calls the
// kernel only when it must wait (ArbitrateLock) or hand the mutex to a waiter (ArbitrateUnlock). Condition variables
// (WaitProcessWideKeyAtomic / SignalProcessWideKey) release the mutex while waiting and take it again on wake-up.
// Semantics follow Atmosphère's mesosphère (KConditionVariable). One lock (g_sched) and one condition variable guard
// every wait; that is simple and correct, and a per-address wait queue is on the roadmap for performance.
#include "nxl.h"
#include <errno.h>
#include <time.h>

#define HAS_WAITERS 0x40000000u
pthread_mutex_t g_sched = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv;
// the clock the waits' deadlines are on: the monotonic one where the condition variable can take it (Windows' pthreads
// time out on the real-time clock only)
#ifdef _WIN32
#define WAIT_CLOCK CLOCK_REALTIME
#else
#define WAIT_CLOCK CLOCK_MONOTONIC
#endif
__attribute__((constructor)) static void sync_init(void) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, WAIT_CLOCK);
    pthread_cond_init(&g_cv, &a);
}

enum { W_LOCK, W_KEY, W_ADDR };
typedef struct waiter {
    uint64_t addr;      // the mutex word (W_LOCK) or the condition variable's key (W_KEY)
    uint64_t mutex;     // W_KEY: the mutex to take again when signaled
    uint32_t handle;    // the waiting thread's handle: what the mutex word holds once it owns it
    int kind, granted;
    struct waiter *next;
} waiter;
static waiter *g_head, **g_tail = &g_head;   // FIFO

static void push(waiter *w) { w->next = NULL; *g_tail = w; g_tail = &w->next; }
// Takes a waiter off the wait queue.
static void unlink_w(waiter *w) {
    for (waiter **p = &g_head; *p; p = &(*p)->next)
        if (*p == w) {
            *p = w->next;
            if (g_tail == &w->next) g_tail = p;
            return;
        }
}
static waiter *first(int kind, uint64_t addr, waiter *after) {
    for (waiter *w = after ? after->next : g_head; w; w = w->next)
        if (w->kind == kind && w->addr == addr && !w->granted) return w;
    return NULL;
}
static uint32_t *word(uint64_t a) { return (uint32_t *)a; }

// The absolute time (on WAIT_CLOCK) ns nanoseconds from now.
static void deadline(struct timespec *ts, int64_t ns) {
    clock_gettime(WAIT_CLOCK, ts);
    ts->tv_sec += ns / 1000000000;
    ts->tv_nsec += ns % 1000000000;
    if (ts->tv_nsec >= 1000000000) ts->tv_sec++, ts->tv_nsec -= 1000000000;
}

// hand the mutex at addr to its first waiter, or free it (caller holds g_sched)
static void release(uint64_t addr) {
    waiter *w = first(W_LOCK, addr, NULL);
    if (!w) { __atomic_store_n(word(addr), 0, __ATOMIC_RELEASE); return; }
    uint32_t v = w->handle | (first(W_LOCK, addr, w) ? HAS_WAITERS : 0);
    __atomic_store_n(word(addr), v, __ATOMIC_RELEASE);
    w->granted = 1;
    pthread_cond_broadcast(&g_cv);
}

// SVC ArbitrateLock: waits to be given a guest mutex its owner holds (the word still saying it has waiters).
Result sync_arbitrate_lock(uint32_t owner, uint64_t addr, uint32_t self) {
    if (addr & 3) return KERR_INVALID_ADDR;
    pthread_mutex_lock(&g_sched);
    if (__atomic_load_n(word(addr), __ATOMIC_ACQUIRE) != (owner | HAS_WAITERS)) {   // changed meanwhile: the guest retries
        pthread_mutex_unlock(&g_sched);
        return RES_OK;
    }
    waiter w = {.addr = addr, .handle = self, .kind = W_LOCK};
    push(&w);
    while (!w.granted) pthread_cond_wait(&g_cv, &g_sched);
    unlink_w(&w);
    pthread_mutex_unlock(&g_sched);
    return RES_OK;
}

// SVC ArbitrateUnlock: gives a guest mutex to the first thread waiting for it.
Result sync_arbitrate_unlock(uint64_t addr) {
    if (addr & 3) return KERR_INVALID_ADDR;
    pthread_mutex_lock(&g_sched);
    release(addr);
    pthread_mutex_unlock(&g_sched);
    return RES_OK;
}

// NXL_TEST_CV_GAP=<microseconds> (tests only): a pause right after the mutex is released, where another thread taking
// the mutex in user mode could slip in; it makes the lost wake-up below certain if the order were wrong
static int cv_gap_us(void) { static int us = -1; if (us < 0) { const char *e = getenv("NXL_TEST_CV_GAP"); us = e ? atoi(e) : 0; } return us; }
// SVC WaitProcessWideKeyAtomic: releases a mutex and waits on a condition key, then takes the mutex again.
Result sync_wait_key(uint64_t mutex, uint64_t key, uint32_t self, int64_t timeout_ns) {
    if (mutex & 3) return KERR_INVALID_ADDR;
    pthread_mutex_lock(&g_sched);
    // The order matters (as mesosphère's KConditionVariable::Wait, and nxnative's hle_thread.c 0x1C): the key word says
    // "has waiters" and the waiter is queued before the mutex is let go. The SDK's Signal reads the key in user mode and
    // skips SignalProcessWideKey when it is 0, and a thread takes a free mutex in user mode, without this lock. Released
    // first, another thread could take the mutex, change the condition, read 0 and not signal: the waiter then slept for
    // good (Breath of the Wild froze with every thread waiting on a condition variable whose lock was free)
    __atomic_store_n(word(key), 1, __ATOMIC_RELEASE);
    waiter w = {.addr = key, .mutex = mutex, .handle = self, .kind = W_KEY};
    push(&w);
    release(mutex);
    if (cv_gap_us()) { struct timespec gap = {0, cv_gap_us() * 1000L}; nanosleep(&gap, NULL); }
    struct timespec ts;
    if (timeout_ns > 0) deadline(&ts, timeout_ns);
    Result r = RES_OK;
    while (!w.granted) {
        if (w.kind == W_KEY && timeout_ns == 0) { r = KERR_TIMED_OUT; break; }
        if (w.kind == W_KEY && timeout_ns > 0) {
            if (pthread_cond_timedwait(&g_cv, &g_sched, &ts) == ETIMEDOUT && w.kind == W_KEY && !w.granted) { r = KERR_TIMED_OUT; break; }
        } else pthread_cond_wait(&g_cv, &g_sched);   // no timeout, or signaled and now waiting for the mutex
    }
    unlink_w(&w);
    if (r == KERR_TIMED_OUT && !first(W_KEY, key, NULL)) __atomic_store_n(word(key), 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_sched);
    return r;   // on a timeout the guest takes the mutex itself (libnx and the SDK do)
}

// SVC SignalProcessWideKey: wakes up to count waiters of a condition key (all when count <= 0).
Result sync_signal_key(uint64_t key, int32_t count) {
    pthread_mutex_lock(&g_sched);
    int n = 0;
    for (waiter *w = first(W_KEY, key, NULL), *next; w && (count <= 0 || n < count); w = next) {
        next = first(W_KEY, key, w);
        n++;
        // take the mutex for it: free -> it owns it; held -> it waits for it as a lock waiter
        uint32_t *m = word(w->mutex), v = __atomic_load_n(m, __ATOMIC_ACQUIRE);
        for (;;) {
            if (v == 0) {
                if (__atomic_compare_exchange_n(m, &v, w->handle, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { w->granted = 1; break; }
            } else if (__atomic_compare_exchange_n(m, &v, v | HAS_WAITERS, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                w->kind = W_LOCK, w->addr = w->mutex;
                break;
            }
        }
    }
    if (!first(W_KEY, key, NULL)) __atomic_store_n(word(key), 0, __ATOMIC_RELEASE);
    if (n) pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_sched);
    return RES_OK;
}

// ---------- address arbitration (svcWaitForAddress / svcSignalToAddress), as nxnative's hle_thread.c and mesosphère's
// KAddressArbiter do it ----------
// the last address-arbiter operations, for the freeze report (sync_arb_report): which waits and signals reached an
// address, with what values and results (under g_sched)
typedef struct { uint64_t addr, tid, ns; int signal, type, count; int64_t value; uint32_t before, after, result; } arb_op;
static arb_op g_arb[256];
static uint64_t g_narb;
// Notes an address arbiter operation in a ring (for a report when a thread waits too long).
static void arb_note(uint64_t addr, int signal, int type, int64_t value, int count, uint32_t before, Result r) {
    nxl_thread *t = nxl_self();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_arb[g_narb++ % 256] = (arb_op){addr, t ? t->id : 0, (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec, signal, type, count, value, before,
                                     __atomic_load_n(word(addr), __ATOMIC_RELAXED), r};
}
// Reports an address's value, its waiters and the last operations on it.
void sync_arb_report(uint64_t addr) {
    pthread_mutex_lock(&g_sched);
    int nw = 0;
    for (waiter *w = first(W_ADDR, addr, NULL); w; w = first(W_ADDR, addr, w)) nw++;
    fprintf(stderr, "      address %#llx holds %#x, %d waiters; its last operations (newest last):\n", (unsigned long long)addr, __atomic_load_n(word(addr), __ATOMIC_RELAXED), nw);
    uint64_t lo = g_narb > 256 ? g_narb - 256 : 0;
    for (uint64_t i = lo; i < g_narb; i++) {
        const arb_op *o = &g_arb[i % 256];
        if (o->addr != addr) continue;
        fprintf(stderr, "        %.3f s thread %llu %s type %d value %#llx count %d: word %#x -> %#x, result %#x\n", o->ns / 1e9, (unsigned long long)o->tid,
                o->signal ? "SignalToAddress" : "WaitForAddress", o->type, (unsigned long long)o->value, o->count, o->before, o->after, o->result);
    }
    pthread_mutex_unlock(&g_sched);
}
// SVC WaitForAddress: waits while the word at an address is below / equal to a value (decrementing it for some types).
Result sync_wait_for_address(uint64_t addr, int type, uint64_t value, int64_t timeout_ns) {
    if (addr & 3 || (type == 3 && addr & 7)) return KERR_INVALID_ADDR;
    if (type < 0 || type > 3) return 0xF001;   // invalid enum value
    pthread_mutex_lock(&g_sched);
    uint32_t v = __atomic_load_n(word(addr), __ATOMIC_ACQUIRE);
    int32_t val = (int32_t)value;
    // 0: wait if less than; 1: decrement if less than, then wait; 2: wait if equal; 3: wait if the 64-bit value is equal
    int ok = type == 3 ? __atomic_load_n((uint64_t *)addr, __ATOMIC_ACQUIRE) == value : type == 2 ? (int32_t)v == val : (int32_t)v < val;
    if (ok && type == 1)
        while (!__atomic_compare_exchange_n(word(addr), &v, v - 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            if (!((int32_t)v < val)) { ok = 0; break; }
    if (!ok) { arb_note(addr, 0, type, (int64_t)value, 0, v, KERR_INVALID_STATE); pthread_mutex_unlock(&g_sched); return KERR_INVALID_STATE; }
    if (timeout_ns == 0) { pthread_mutex_unlock(&g_sched); return KERR_TIMED_OUT; }
    arb_note(addr, 0, type, (int64_t)value, 0, v, 0xFFFFFFFFu);   // waiting (the result is noted when it wakes)
    waiter w = {.addr = addr, .kind = W_ADDR};
    push(&w);
    struct timespec ts;
    if (timeout_ns > 0) deadline(&ts, timeout_ns);
    Result r = RES_OK;
    while (!w.granted) {
        if (timeout_ns > 0) { if (pthread_cond_timedwait(&g_cv, &g_sched, &ts) == ETIMEDOUT && !w.granted) { r = KERR_TIMED_OUT; break; } }
        else pthread_cond_wait(&g_cv, &g_sched);
    }
    unlink_w(&w);
    arb_note(addr, 0, type, (int64_t)value, 0, v, r);
    pthread_mutex_unlock(&g_sched);
    return r;
}

// SVC SignalToAddress: wakes threads waiting on an address (changing the word first for some types).
Result sync_signal_to_address(uint64_t addr, int type, int32_t val, int32_t count) {
    if (addr & 3) return KERR_INVALID_ADDR;
    if (type < 0 || type > 2) return 0xF001;
    pthread_mutex_lock(&g_sched);
    int nw = 0;
    for (waiter *w = first(W_ADDR, addr, NULL); w; w = first(W_ADDR, addr, w)) nw++;
    Result r = RES_OK;
    uint32_t before = __atomic_load_n(word(addr), __ATOMIC_RELAXED);
    if (type != 0) {   // 1: increment if equal; 2: if equal, modify by the number of waiters
        int32_t nv = val + 1;
        if (type == 2) {
            // waking all (count <= 0) with waiters subtracts 2, not 1 (mesosphere's SignalAndModifyByWaitingCountIfEqual;
            // nxnative found Unity's job fences in Fire Emblem Engage depend on it)
            if (count <= 0) nv = nw > 0 ? val - 2 : val + 1;
            else nv = nw == 0 ? val + 1 : nw <= count ? val - 1 : val;
        }
        uint32_t e = (uint32_t)val;
        if (!__atomic_compare_exchange_n(word(addr), &e, (uint32_t)nv, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) r = KERR_INVALID_STATE;
    }
    if (!r) {
        int n = 0;
        for (waiter *w; (count <= 0 || n < count) && (w = first(W_ADDR, addr, NULL)); n++) w->granted = 1;
        if (n) pthread_cond_broadcast(&g_cv);
    }
    arb_note(addr, 1, type, val, count, before, r);
    pthread_mutex_unlock(&g_sched);
    return r;
}

int handle_type(uint32_t h);
static int is_signaled(uint32_t h, Result *bad) {   // caller holds g_sched
    switch (handle_type(h)) {
    case H_THREAD: { nxl_thread *t = handle_get(h, H_THREAD); return t && t->exited; }
    case H_EVENT_R: case H_EVENT_W: {
        nxl_event *e = handle_get(h, H_EVENT_R);
        if (!e) e = handle_get(h, H_EVENT_W);
        return e && e->signaled;
    }
    default: *bad = KERR_INVALID_HANDLE; return 0;
    }
}

// SVC WaitSynchronization: waits until one of the handles is signalled (or the timeout, or a cancel); its index.
Result sync_wait_handles(const uint32_t *handles, int n, int64_t timeout_ns, int32_t *index) {
    struct timespec ts;
    if (timeout_ns > 0) deadline(&ts, timeout_ns);
    pthread_mutex_lock(&g_sched);
    nxl_thread *self = nxl_self();
    for (;;) {
        if (self && self->sync_cancel) { self->sync_cancel = 0; *index = -1; pthread_mutex_unlock(&g_sched); return KERR_CANCELLED; }
        Result bad = RES_OK;
        for (int i = 0; i < n; i++)
            if (is_signaled(handles[i], &bad)) { *index = i; pthread_mutex_unlock(&g_sched); return RES_OK; }
        if (bad) { pthread_mutex_unlock(&g_sched); return bad; }
        if (timeout_ns == 0) break;
        if (timeout_ns > 0) { if (pthread_cond_timedwait(&g_cv, &g_sched, &ts) == ETIMEDOUT) break; }
        else pthread_cond_wait(&g_cv, &g_sched);
    }
    pthread_mutex_unlock(&g_sched);
    return KERR_TIMED_OUT;
}

void sync_signal_object(void *obj) { (void)obj; pthread_cond_broadcast(&g_cv); }
