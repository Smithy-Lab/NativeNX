// SPDX-License-Identifier: GPL-3.0-or-later
// A minimal guest runtime for the test programs: SVC wrappers with the kernel's register ABI (as libnx's), mutexes and
// condition variables with Horizon's semantics (as libnx's), threads, and output. Compiled for aarch64, freestanding.
#pragma once
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef signed long s64;
typedef signed int s32;
#define CUR_THREAD_HANDLE 0xFFFF8000u
typedef unsigned long size_t;
typedef u32 Result;
typedef u32 Handle;
#define NULL ((void *)0)

// ---- SVCs: arguments in x0-x7, results back in them ----
typedef struct { u64 x[8]; } Regs;
#define SVCALL(imm, r)                                                                                            \
    do {                                                                                                          \
        register u64 x0 __asm__("x0") = (r).x[0], x1 __asm__("x1") = (r).x[1], x2 __asm__("x2") = (r).x[2],       \
                     x3 __asm__("x3") = (r).x[3], x4 __asm__("x4") = (r).x[4], x5 __asm__("x5") = (r).x[5],       \
                     x6 __asm__("x6") = (r).x[6], x7 __asm__("x7") = (r).x[7];                                    \
        __asm__ volatile("svc " #imm : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7) \
                         :: "memory");                                                                            \
        (r).x[0] = x0, (r).x[1] = x1, (r).x[2] = x2, (r).x[3] = x3, (r).x[4] = x4, (r).x[5] = x5, (r).x[6] = x6, (r).x[7] = x7; \
    } while (0)

static inline Result svcSetHeapSize(u64 *out, u64 size) { Regs r = {{0, size}}; SVCALL(0x01, r); *out = r.x[1]; return r.x[0]; }
static inline Result svcMapMemory(u64 dst, u64 src, u64 size) { Regs r = {{dst, src, size}}; SVCALL(0x04, r); return r.x[0]; }
static inline Result svcUnmapMemory(u64 dst, u64 src, u64 size) { Regs r = {{dst, src, size}}; SVCALL(0x05, r); return r.x[0]; }
typedef struct { u64 addr, size; u32 type, attr, perm, ipc, dev, pad; } MemoryInfo;
static inline Result svcQueryMemory(MemoryInfo *mi, u64 addr) { Regs r = {{(u64)mi, 0, addr}}; SVCALL(0x06, r); return r.x[0]; }
static inline void svcExitProcess(void) { Regs r = {{0}}; SVCALL(0x07, r); __builtin_unreachable(); }
static inline Result svcCreateThread(Handle *out, void *entry, void *arg, void *stack_top, int prio, int core) {
    Regs r = {{0, (u64)entry, (u64)arg, (u64)stack_top, (u64)prio, (u64)(long)core}};
    SVCALL(0x08, r);
    *out = r.x[1];
    return r.x[0];
}
static inline Result svcStartThread(Handle h) { Regs r = {{h}}; SVCALL(0x09, r); return r.x[0]; }
static inline void svcExitThread(void) { Regs r = {{0}}; SVCALL(0x0A, r); __builtin_unreachable(); }
static inline void svcSleepThread(s64 ns) { Regs r = {{(u64)ns}}; SVCALL(0x0B, r); }
static inline Result svcSignalEvent(Handle h) { Regs r = {{h}}; SVCALL(0x11, r); return r.x[0]; }
static inline Result svcClearEvent(Handle h) { Regs r = {{h}}; SVCALL(0x12, r); return r.x[0]; }
static inline Result svcCloseHandle(Handle h) { Regs r = {{h}}; SVCALL(0x16, r); return r.x[0]; }
static inline Result svcWaitSync(int *index, const Handle *h, int n, s64 timeout) {
    Regs r = {{0, (u64)h, (u64)n, (u64)timeout}};
    SVCALL(0x18, r);
    *index = (int)r.x[1];
    return r.x[0];
}
static inline Result svcCancelSynchronization(Handle h) { Regs r = {{h}}; SVCALL(0x19, r); return r.x[0]; }
static inline Result svcArbitrateLock(Handle owner, u32 *addr, Handle self) { Regs r = {{owner, (u64)addr, self}}; SVCALL(0x1A, r); return r.x[0]; }
static inline Result svcArbitrateUnlock(u32 *addr) { Regs r = {{(u64)addr}}; SVCALL(0x1B, r); return r.x[0]; }
static inline Result svcWaitProcessWideKeyAtomic(u32 *m, u32 *cv, Handle self, s64 timeout) {
    Regs r = {{(u64)m, (u64)cv, self, (u64)timeout}};
    SVCALL(0x1C, r);
    return r.x[0];
}
static inline Result svcSignalProcessWideKey(u32 *cv, int n) { Regs r = {{(u64)cv, (u64)(long)n}}; SVCALL(0x1D, r); return r.x[0]; }
static inline Result svcGetThreadCoreMask(s32 *core, u64 *mask, Handle h) { Regs r = {{0, 0, h}}; SVCALL(0x0E, r); *core = (s32)r.x[1]; *mask = r.x[2]; return r.x[0]; }
static inline Result svcSetThreadCoreMask(Handle h, s32 core, u64 mask) { Regs r = {{h, (u64)(s64)core, mask}}; SVCALL(0x0F, r); return r.x[0]; }
static inline u32 svcGetCurrentProcessorNumber(void) { Regs r = {{0}}; SVCALL(0x10, r); return (u32)r.x[0]; }
static inline u64 svcGetSystemTick(void) { Regs r = {{0}}; SVCALL(0x1E, r); return r.x[0]; }
static inline Result svcGetThreadId(u64 *out, Handle h) { Regs r = {{0, h}}; SVCALL(0x25, r); *out = r.x[1]; return r.x[0]; }
static inline void svcBreak(u64 reason) { Regs r = {{reason}}; SVCALL(0x26, r); }
static inline Result svcOutputDebugString(const char *s, u64 n) { Regs r = {{(u64)s, n}}; SVCALL(0x27, r); return r.x[0]; }
static inline Result svcGetInfo(u64 *out, u32 id0, Handle h, u64 id1) { Regs r = {{0, id0, h, id1}}; SVCALL(0x29, r); *out = r.x[1]; return r.x[0]; }
static inline Result svcMapPhysicalMemory(u64 addr, u64 size) { Regs r = {{addr, size}}; SVCALL(0x2C, r); return r.x[0]; }
static inline Result svcUnmapPhysicalMemory(u64 addr, u64 size) { Regs r = {{addr, size}}; SVCALL(0x2D, r); return r.x[0]; }
static inline Result svcWaitForAddress(void *addr, int type, s64 value, s64 timeout) { Regs r = {{(u64)addr, (u64)type, (u64)value, (u64)timeout}}; SVCALL(0x34, r); return r.x[0]; }
static inline Result svcSignalToAddress(void *addr, int type, int value, int count) { Regs r = {{(u64)addr, (u64)type, (u64)(long)value, (u64)(long)count}}; SVCALL(0x35, r); return r.x[0]; }
#define KERR_INVALID_STATE 0xFA01
static inline Result svcCreateEvent(Handle *w, Handle *rd) { Regs r = {{0}}; SVCALL(0x45, r); *w = r.x[1]; *rd = r.x[2]; return r.x[0]; }
#define KERR_CANCELLED 0xEC01
#define KERR_TIMED_OUT 0xEA01

// ---- the thread's TLS (TPIDRRO_EL0): its handle is kept at +0x1F0 ----
static inline u8 *tls(void) { u8 *p; __asm__("mrs %0, tpidrro_el0" : "=r"(p)); return p; }
static inline Handle cur_handle(void) { return *(u32 *)(tls() + 0x1F0); }

// ---- mutex and condition variable (libnx's algorithm) ----
#define HAS_LISTENERS 0x40000000u
typedef u32 Mutex;
typedef u32 CondVar;
static inline void mutexLock(Mutex *m) {
    Handle self = cur_handle();
    for (;;) {
        u32 cur = __atomic_load_n(m, __ATOMIC_ACQUIRE);
        if (cur == 0) {
            if (__atomic_compare_exchange_n(m, &cur, self, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (!(cur & HAS_LISTENERS) &&
            !__atomic_compare_exchange_n(m, &cur, cur | HAS_LISTENERS, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) continue;
        svcArbitrateLock(cur & ~HAS_LISTENERS, m, self);
        if ((__atomic_load_n(m, __ATOMIC_ACQUIRE) & ~HAS_LISTENERS) == self) return;   // the kernel handed it over
    }
}
static inline void mutexUnlock(Mutex *m) {
    u32 self = cur_handle();
    if (__atomic_compare_exchange_n(m, &self, 0, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return;   // nobody waits
    svcArbitrateUnlock(m);
}
static inline Result condvarWaitTimeout(CondVar *c, Mutex *m, s64 ns) {
    Result r = svcWaitProcessWideKeyAtomic(m, c, cur_handle(), ns);
    if (r == KERR_TIMED_OUT) mutexLock(m);   // on a timeout the caller takes the mutex itself
    return r;
}
static inline void condvarWait(CondVar *c, Mutex *m) { condvarWaitTimeout(c, m, -1); }
static inline void condvarWakeOne(CondVar *c) { svcSignalProcessWideKey(c, 1); }
static inline void condvarWakeAll(CondVar *c) { svcSignalProcessWideKey(c, -1); }

// ---- threads ----
typedef struct { Handle h; void (*fn)(void *); void *arg; u8 stack[256 * 1024] __attribute__((aligned(16))); } Thread;
static void thread_entry(Thread *t, Handle h) {   // the host passes the handle in x1
    *(u32 *)(tls() + 0x1F0) = h;
    t->fn(t->arg);
    svcExitThread();
}
static inline Result threadCreate(Thread *t, void (*fn)(void *), void *arg) {
    t->fn = fn, t->arg = arg;
    Result r = svcCreateThread(&t->h, (void *)thread_entry, t, t->stack + sizeof t->stack, 44, -2);
    return r ? r : svcStartThread(t->h);
}
static inline Result threadJoin(Thread *t) { int i; return svcWaitSync(&i, &t->h, 1, -1); }

// ---- output ----
static inline size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static inline void puts_(const char *s) { svcOutputDebugString(s, slen(s)); }
static inline char *fmt_u(char *p, u64 v, int base) {
    char t[24]; int n = 0;
    do t[n++] = "0123456789abcdef"[v % base], v /= base; while (v);
    while (n) *p++ = t[--n];
    return p;
}
// printf with %s %d %u %x %ld %lu %lx only
static void print(const char *f, ...) {
    char buf[512], *p = buf;
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    for (; *f && p < buf + 480; f++) {
        if (*f != '%') { *p++ = *f; continue; }
        int lng = 0;
        if (*++f == 'l') lng = 1, f++;
        if (*f == 's') { const char *s = __builtin_va_arg(ap, const char *); while (*s && p < buf + 480) *p++ = *s++; }
        else if (*f == 'd') { s64 v = lng ? __builtin_va_arg(ap, s64) : __builtin_va_arg(ap, int); if (v < 0) *p++ = '-', v = -v; p = fmt_u(p, v, 10); }
        else if (*f == 'u') p = fmt_u(p, lng ? __builtin_va_arg(ap, u64) : __builtin_va_arg(ap, u32), 10);
        else if (*f == 'x') p = fmt_u(p, lng ? __builtin_va_arg(ap, u64) : __builtin_va_arg(ap, u32), 16);
        else *p++ = *f;
    }
    __builtin_va_end(ap);
    svcOutputDebugString(buf, p - buf);
}

// ---- the test harness: each CHECK prints ok / not ok, and the summary line decides the result ----
static int g_pass, g_fail;
static Mutex g_report_mu;
#define CHECK(name, cond)                                                                          \
    do {                                                                                           \
        int ok_ = !!(cond);                                                                        \
        mutexLock(&g_report_mu);                                                                   \
        if (ok_) g_pass++; else g_fail++;                                                          \
        print("%s - %s\n", ok_ ? "ok" : "not ok", name);                                           \
        mutexUnlock(&g_report_mu);                                                                 \
    } while (0)
// a check that cannot pass on this host, by design: reported (with why), not counted
#define SKIP(name, why) do { mutexLock(&g_report_mu); print("ok - %s # SKIP %s\n", name, why); mutexUnlock(&g_report_mu); } while (0)
int test_main(void);
void __nx_start(Handle main_thread) {
    *(u32 *)(tls() + 0x1F0) = main_thread;
    test_main();
    print("nxl-tests: %d passed, %d failed\n", g_pass, g_fail);
    svcExitProcess();
}
