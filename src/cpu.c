// SPDX-License-Identifier: GPL-3.0-or-later
// The recompiled code: every module's text is recompiled into one ScarletNX module (one dispatch table over all of
// them), and each guest thread is a host thread running its own CPU state on it.
// Note on stacks: each guest function is a host function, so guest call depth is host stack depth. Guest threads get
// large host stacks for that reason (the memory is committed only as it is used).
#include "nxl.h"
#include "host.h"
#include "perf.h"
#include <elf.h>
#include <sched.h>
#include <stdatomic.h>
#include <time.h>

static struct snx_recompiler *g_rec;
static struct snx_recompiled *g_mod;
static RtAPI g_rt;
static GuestFn g_default_miss;
static __thread nxl_thread *t_self;
// where a guest thread's host thread leaves the generated code when the guest thread exits (svcExitThread), with a jump
// that unwinds nothing: on Linux the compiler's own setjmp; on Windows the C runtime's with no frame (its longjmp then
// restores the registers without unwinding; the compiler's leaves rsi and rdi, which Windows keeps across calls, as
// they were at the jump)
#ifdef _WIN32
#include <setjmp.h>
typedef jmp_buf exit_jmp;
#define EXIT_SETJMP(b) _setjmp(b, NULL)
#define EXIT_LONGJMP(b) longjmp(b, 1)
#else
typedef void *exit_jmp[5];
#define EXIT_SETJMP(b) __builtin_setjmp(b)
#define EXIT_LONGJMP(b) __builtin_longjmp(b, 1)
#endif
static __thread exit_jmp *t_exit_jmp;
static pthread_mutex_t g_threads_mu = PTHREAD_MUTEX_INITIALIZER;
static nxl_thread *g_threads;
static uint64_t g_next_tid = 0x51;
#define HOST_STACK (64u << 20)

nxl_thread *nxl_self(void) { return t_self; }

// -v: every 10 s, how many frames the game presented and what each live guest thread is doing, so a run that shows
// nothing (no window yet) still shows whether it is loading, waiting or stuck
_Atomic uint64_t nxl_perf_n[NXL_PERF_N], nxl_perf_ns[NXL_PERF_N], nxl_perf_upload_bytes;
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) __attribute__((weak));
// where the time went since the last line: per 10 s, counts and milliseconds
static void perf_lines(double secs) {
    static uint64_t pn[NXL_PERF_N], pns[NXL_PERF_N], pbytes, pfaults, pproc;
    uint64_t n[NXL_PERF_N], ns[NXL_PERF_N];
    for (int k = 0; k < NXL_PERF_N; k++) n[k] = atomic_load(&nxl_perf_n[k]) - pn[k], ns[k] = atomic_load(&nxl_perf_ns[k]) - pns[k], pn[k] += n[k], pns[k] += ns[k];
    uint64_t bytes = atomic_load(&nxl_perf_upload_bytes) - pbytes;
    pbytes += bytes;
    uint64_t faults = 0, preps = 0;
    if (nvn_dirty_stats) nvn_dirty_stats(&faults, &preps);
    uint64_t df = faults - pfaults;
    pfaults = faults;
    uint64_t proc = host_process_cpu_ns(), dp = proc - pproc;
    pproc = proc;
#define MS(k) (ns[k] / 1e6)
    fprintf(stderr, "    time: process CPU %.0f%% (of one core)\n", dp / 1e9 / secs * 100);
    fprintf(stderr, "    gpu: %llu submissions waited %.0f ms | %llu frames shown %.0f ms | %llu translations %.0f ms | %llu pipelines %.0f ms\n",
            (unsigned long long)n[NXL_PERF_SUBMIT], MS(NXL_PERF_SUBMIT), (unsigned long long)n[NXL_PERF_PRESENT], MS(NXL_PERF_PRESENT),
            (unsigned long long)n[NXL_PERF_TRANSLATE], MS(NXL_PERF_TRANSLATE), (unsigned long long)n[NXL_PERF_PIPELINE], MS(NXL_PERF_PIPELINE));
    fprintf(stderr, "    replay: %llu submissions recorded in %.0f ms, %llu segments handed to the GPU early\n",
            (unsigned long long)n[NXL_PERF_REPLAY], MS(NXL_PERF_REPLAY), (unsigned long long)n[NXL_PERF_SEGMENT]);
    fprintf(stderr, "    draws: %llu recorded: finding shaders %.0f ms, descriptors %.0f ms, pipelines and passes %.0f ms, commands %.0f ms\n",
            (unsigned long long)n[NXL_PERF_D_SHADER], MS(NXL_PERF_D_SHADER), MS(NXL_PERF_D_DESC), MS(NXL_PERF_D_PIPE), MS(NXL_PERF_D_CMD));
    fprintf(stderr, "    textures: %llu uploads %.0f MB %.0f ms (ASTC decoded on the CPU: %llu, %.0f ms) | %llu page faults | %llu JIT misses\n",
            (unsigned long long)n[NXL_PERF_UPLOAD], bytes / 1048576.0, MS(NXL_PERF_UPLOAD), (unsigned long long)n[NXL_PERF_ASTC], MS(NXL_PERF_ASTC),
            (unsigned long long)df, (unsigned long long)n[NXL_PERF_JIT]);
#undef MS
}
// where a busy thread is: its host program counter (host_thread_sample), which maps back to the guest function the
// recompiled code belongs to (sampled, so a thread spinning in a wait loop shows the loop's function)
static uint64_t guest_fn_of_host(const CPU *c, uint64_t rip);
static void print_guest(const char *what, uint64_t a);
static int guest_readable(uint64_t a);
static void wait_chain(const nxl_thread *t);
static pthread_once_t g_hfn_once;
static void hfn_load(void);
// Where a guest thread is now: three samples 5 ms apart of its host instruction pointer (for the status line's "busy
// in" report).
static void sample_thread(nxl_thread *t) {
    uint64_t seen[3] = {0};
    int n = 0;
    for (int k = 0; k < 3; k++) {   // three looks, 5 ms apart
        uint64_t rip = host_thread_sample(t);
        if (!rip || host_addr_is_ours(rip)) continue;   // in nxlinux itself, not guest code
        uint64_t fn = guest_fn_of_host(&t->cpu, rip);
        if (fn && (!n || seen[n - 1] != fn)) seen[n++] = fn;
        struct timespec ts = {0, 5000000};
        nanosleep(&ts, NULL);
    }
    for (int k = 0; k < n; k++) print_guest(k ? "    and" : "    in", seen[k]);
    if (n) {
        wait_chain(t);   // lr and the frame records: the callers, nearer than the sampled function (which is approximate)
        fprintf(stderr, "      x0 %#llx x1 %#llx x2 %#llx x8 %#llx x19 %#llx x20 %#llx\n", (unsigned long long)t->cpu.x[0], (unsigned long long)t->cpu.x[1],
                (unsigned long long)t->cpu.x[2], (unsigned long long)t->cpu.x[8], (unsigned long long)t->cpu.x[19], (unsigned long long)t->cpu.x[20]);
    }
}
// a waiting thread's way there: its last few guest return addresses (lr, then the frame records from x29), compactly
static int g_chain_depth = 5;   // the freeze report asks for more
// The return addresses up a guest thread's frame-pointer chain (for the status report of a waiting thread).
static void wait_chain(const nxl_thread *t) {
    uint64_t r[16];
    int n = 0, depth = g_chain_depth < 16 ? g_chain_depth : 16;
    if (t->cpu.x[30]) r[n++] = t->cpu.x[30];
    uint64_t fp = t->cpu.x[29];
    for (int k = 0; k < 24 && n < depth && guest_readable(fp) && guest_readable(fp + 8); k++) {
        uint64_t next = ((uint64_t *)fp)[0], ret = ((uint64_t *)fp)[1];
        if (!ret) break;
        if (!n || ret != r[n - 1]) r[n++] = ret;
        if (next <= fp) break;
        fp = next;
    }
    if (!n) return;
    fprintf(stderr, "      from");
    for (int k = 0; k < n; k++) {
        module_t *m = module_of(r[k]);
        if (m) fprintf(stderr, "%s%s+%#llx", k ? " < " : " ", m->name, (unsigned long long)(r[k] - (uint64_t)(uintptr_t)m->base));
        else fprintf(stderr, "%s%#llx", k ? " < " : " ", (unsigned long long)r[k]);
    }
    fputc('\n', stderr);
}
// The status thread: a line every few seconds of what the game is doing (frames, threads, where they wait), for the
// log.
static void *status_main(void *arg) {
    (void)arg;
    struct timespec t0, prev;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    prev = t0;
    uint64_t pframes = 0;
    pthread_once(&g_hfn_once, hfn_load);   // the map for samples, read before the first line (it logs its size)
    for (;;) {
        struct timespec ts = {10, 0};
        nanosleep(&ts, NULL);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double secs = (double)(now.tv_sec - prev.tv_sec) + (now.tv_nsec - prev.tv_nsec) / 1e9;
        prev = now;
        uint64_t frames = nxl_frames_presented();
        // no frame for two status lines after the game had shown some: once, a fuller report (every waiting thread's
        // calls, the address it waits on and that address's last operations)
        static int still, freeze_done;
        still = frames && frames == pframes ? still + 1 : 0;
        int freeze = still == 2 && !freeze_done;
        if (freeze) freeze_done = 1, g_chain_depth = 12, fprintf(stderr, "[nxl] no frame for 20 s: the threads in detail\n");
        else g_chain_depth = 5;
        pthread_mutex_lock(&g_threads_mu);
        int live = 0;
        for (nxl_thread *t = g_threads; t; t = t->next) live += t->started && !t->exited;
        fprintf(stderr, "[nxl] status at %.0f s: %llu frames presented (%.1f fps), %d guest threads\n", (double)(now.tv_sec - t0.tv_sec),
                (unsigned long long)frames, (frames - pframes) / secs, live);
        pframes = frames;
        for (nxl_thread *t = g_threads; t; t = t->next) {
            if (!t->started || t->exited) continue;
            uint64_t c = host_thread_cpu_ns(t);
            int pct = c && t->status_cpu_ns ? (int)((c - t->status_cpu_ns) / 1e7 / secs + 0.5) : -1;
            t->status_cpu_ns = c;
            char cpu[24] = "";
            if (pct >= 0) snprintf(cpu, sizeof cpu, ", CPU %d%%", pct);
            const char *h = t->last_hle;
            if (t->in_hle) {
                fprintf(stderr, "    thread %llu (priority %d%s): in host function %s\n", (unsigned long long)t->id, t->prio, cpu, h ? h : "?");
                wait_chain(t);
            }
            else if (t->in_svc && (t->last_svc == 0x1A || t->last_svc == 0x1C)) {
                // a lock it waits for (ArbitrateLock: x1; WaitProcessWideKeyAtomic: its mutex x0, key x1): the lock word
                // holds the owner's handle, so the holder can be named
                uint64_t lock = t->last_svc == 0x1A ? t->svc_arg[1] : t->svc_arg[0], key = t->last_svc == 0x1C ? t->svc_arg[1] : 0;
                uint32_t word = lock && !(lock & 3) && guest_readable(lock & ~7ull) ? __atomic_load_n((uint32_t *)(uintptr_t)lock, __ATOMIC_RELAXED) : 0;
                nxl_thread *o = NULL;
                for (nxl_thread *u = g_threads; u && word; u = u->next) if (u->handle == (word & ~0x40000000u)) o = u;
                char who[64] = "";
                if (word && o) snprintf(who, sizeof who, ", held by thread %llu", (unsigned long long)o->id);
                else if (word) snprintf(who, sizeof who, ", held (handle %#x)", word & ~0x40000000u);
                else snprintf(who, sizeof who, ", free");
                if (t->last_svc == 0x1A)
                    fprintf(stderr, "    thread %llu (priority %d%s): in SVC 0x1a, waiting for the lock at %#llx%s\n", (unsigned long long)t->id, t->prio, cpu, (unsigned long long)lock, who);
                else
                    fprintf(stderr, "    thread %llu (priority %d%s): in SVC 0x1c, waiting on condition %#llx (its lock %#llx%s)\n", (unsigned long long)t->id, t->prio, cpu, (unsigned long long)key, (unsigned long long)lock, who);
                if (t->last_svc == 0x1A || freeze) wait_chain(t);
            }
            else if (t->in_svc && t->last_svc == 0x18 && t->svc_arg[2] <= 0x40 && guest_readable(t->svc_arg[1] & ~7ull)) {
                // WaitSynchronization: the handles it waits on
                void handle_describe(uint32_t h, char *buf, size_t n);
                char line[512]; int k = 0;
                for (uint64_t i = 0; i < t->svc_arg[2] && k < (int)sizeof line - 64; i++) {
                    char d[64];
                    handle_describe(((const uint32_t *)(uintptr_t)t->svc_arg[1])[i], d, sizeof d);
                    k += snprintf(line + k, sizeof line - (size_t)k, "%s%s", i ? ", " : "", d);
                }
                line[k] = 0;
                fprintf(stderr, "    thread %llu (priority %d%s): in SVC 0x18, waiting on %llu handles: %s\n", (unsigned long long)t->id, t->prio, cpu, (unsigned long long)t->svc_arg[2], line);
            }
            else if (t->in_svc && t->last_svc == 0x34) {   // WaitForAddress(addr, type, value): the word and what it waits for
                uint64_t a = t->svc_arg[0];
                uint32_t w = !(a & 3) && guest_readable(a & ~7ull) ? __atomic_load_n((uint32_t *)(uintptr_t)a, __ATOMIC_RELAXED) : 0;
                static const char *ty[4] = {"while less than", "decremented, while less than", "while equal to", "while equal to (64-bit)"};
                fprintf(stderr, "    thread %llu (priority %d%s): in SVC 0x34, waiting on address %#llx %s %#x (it holds %#x)\n", (unsigned long long)t->id, t->prio, cpu,
                        (unsigned long long)a, t->svc_arg[1] < 4 ? ty[t->svc_arg[1]] : "?", (uint32_t)t->svc_arg[2], w);
                wait_chain(t);
                if (freeze) sync_arb_report(a);
            }
            else if (t->in_svc) fprintf(stderr, "    thread %llu (priority %d%s): in SVC %#x\n", (unsigned long long)t->id, t->prio, cpu, t->last_svc);
            else {
                fprintf(stderr, "    thread %llu (priority %d%s): running guest code (last host call %s)\n", (unsigned long long)t->id, t->prio, cpu, h ? h : "none");
                if (pct >= 50 || pct < 0) sample_thread(t);   // busy (or not measured yet): where
            }
        }
        pthread_mutex_unlock(&g_threads_mu);
        perf_lines(secs);
    }
    return NULL;
}
// Starts the status thread.
void nxl_status_start(void) {
    pthread_t pt;
    if (!pthread_create(&pt, NULL, status_main, NULL)) pthread_detach(pt);
}

uint64_t guest_ticks(void) {   // 19.2 MHz, Horizon's system counter
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 19200000ull + (uint64_t)ts.tv_nsec * 12 / 625;
}

// ---------- host-callable guest functions ----------
// nxnative's host code calls guest functions through plain C function pointers (the game's code is ARM64 there, like
// the host's). Here each such pointer is a 32-byte x86_64 stub in a reserved region: it loads the guest address into r11
// and jumps to nxl_tramp_common (tramp.S), which maps the System V arguments to AAPCS64 registers and runs the function.
// Limits: up to 8 integer and 8 floating-point arguments, scalar or two-register results (a struct returned through
// x8, or a struct passed in floating-point registers, needs an explicit GUEST_CALL).
void nxl_tramp_common(void);
static pthread_mutex_t g_tramp_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t *g_tramp_target;   // stub index -> guest address
static uint32_t g_ntramp, *g_tramp_hash, g_tramp_hmask;
#define TRAMP_STUB 32
#define TRAMP_MAX (NXL_TRAMP_SIZE / TRAMP_STUB)

static void tramp_hash_put(uint64_t g, uint32_t idx) {
    uint32_t j = (uint32_t)((g >> 2) * 0x9E3779B1u) & g_tramp_hmask;
    while (g_tramp_hash[j]) j = (j + 1) & g_tramp_hmask;
    g_tramp_hash[j] = idx + 1;
}
// A host function pointer for a guest function (a trampoline that runs it on the recompiled code), made once per
// function.
void *host_callable(uint64_t g) {
    if (!g) return NULL;
    if (g >= NXL_TRAMP_BASE && g < NXL_TRAMP_BASE + NXL_TRAMP_SIZE) return (void *)g;   // already one
    pthread_mutex_lock(&g_tramp_mu);
    if (!g_tramp_hash) {
        g_tramp_target = calloc(TRAMP_MAX, sizeof *g_tramp_target);
        g_tramp_hmask = TRAMP_MAX * 2 - 1;
        g_tramp_hash = calloc(TRAMP_MAX * 2, sizeof *g_tramp_hash);
    }
    for (uint32_t j = (uint32_t)((g >> 2) * 0x9E3779B1u) & g_tramp_hmask; g_tramp_hash[j]; j = (j + 1) & g_tramp_hmask)
        if (g_tramp_target[g_tramp_hash[j] - 1] == g) {
            void *p = (void *)(NXL_TRAMP_BASE + (uint64_t)(g_tramp_hash[j] - 1) * TRAMP_STUB);
            pthread_mutex_unlock(&g_tramp_mu);
            return p;
        }
    if (g_ntramp == TRAMP_MAX) die("too many host-callable guest functions");
    uint32_t idx = g_ntramp++;
    uint8_t *s = (uint8_t *)(NXL_TRAMP_BASE + (uint64_t)idx * TRAMP_STUB);
    uint64_t page = (uint64_t)s & ~0xFFFull;
    if (idx % (0x1000 / TRAMP_STUB) == 0 && host_vm_commit(page, 0x1000)) die("the host-callable stubs' page: %s", host_vm_error());   // a page's first stub
    host_vm_protect(page, 0x1000, HOST_VM_RW);
    uint64_t common = (uint64_t)nxl_tramp_common;
    s[0] = 0x49, s[1] = 0xBB; memcpy(s + 2, &g, 8);         // movabs r11, guest address
    s[10] = 0x48, s[11] = 0xB8; memcpy(s + 12, &common, 8);  // movabs rax, nxl_tramp_common
    s[20] = 0xFF, s[21] = 0xE0;                               // jmp rax
    memset(s + 22, 0xCC, TRAMP_STUB - 22);
    host_vm_protect(page, 0x1000, HOST_VM_RX);
    g_tramp_target[idx] = g;
    tramp_hash_put(g, idx);
    pthread_mutex_unlock(&g_tramp_mu);
    return s;
}
static uint64_t tramp_target(uint64_t p) {   // the guest function behind a stub address, 0 if p is none
    if (p < NXL_TRAMP_BASE || p >= NXL_TRAMP_BASE + (uint64_t)g_ntramp * TRAMP_STUB || (p - NXL_TRAMP_BASE) % TRAMP_STUB) return 0;
    return g_tramp_target[(p - NXL_TRAMP_BASE) / TRAMP_STUB];
}
uint64_t guest_addr_of(const void *fn) { uint64_t g = tramp_target((uint64_t)fn); return g ? g : (uint64_t)fn; }

// called by nxl_tramp_common: ints[0-7] are x0-x7, fps[0-7] are v0-v7; x0, x1 and v0 are written back
void nxl_tramp_call(uint64_t fn, uint64_t *ints, V128 *fps);
// Called by a trampoline: runs the guest function with the integer and vector arguments it was given, the results back
// in them.
void nxl_tramp_call(uint64_t fn, uint64_t *ints, V128 *fps) {
    V128 v[8];
    memcpy(v, fps, sizeof v);
    uint64_t x1;
    ints[0] = guest_call_full(fn, ints, v, &x1);
    ints[1] = x1;
    fps[0] = v[0];
}

// host functions handed to guest code (a thread entry or callback host code passes to the SDK, as nxnative's
// dm_guest_thread_spawn does): a call to an address in this executable's code runs it natively, with integer arguments
// from x0-x7 and the result in x0 (floating-point arguments are not carried)
typedef uint64_t (*host_fn8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
// When guest code jumps to a host function (one handed to it as a callback), calls it with the guest's argument
// registers.
static int call_host_fn(CPU *c, uint64_t pc) {
    if (!host_addr_is_ours(pc)) return 0;
    uint64_t ret = c->x[30];
    c->x[0] = ((host_fn8)pc)(c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], c->x[5], c->x[6], c->x[7]);
    c->pc = ret;
    return 1;
}

// The recompiled code reached an address it has no code for: a host stand-in, a host function, a trampoline's target,
// or code to compile now.
static void on_miss(CPU *c, uint64_t pc) {
    if (hle_dispatch(c, pc)) return;
    if (call_host_fn(c, pc)) return;
    uint64_t g = tramp_target(pc);
    if (g) {   // guest code reached a stub the host handed it: continue at the guest function behind it
        GuestFn f = snx_recompiled_lookup(g_mod, g);
        f(c, g);
        return;
    }
    if (atomic_fetch_add_explicit(&nxl_perf_n[NXL_PERF_JIT], 1, memory_order_relaxed) == 0 && nxl_verbose) {   // (the miss also runs the code: no time)
        uint64_t off = 0;
        const char *mod = NULL, *sym = module_symbolize(pc, &off, &mod);
        LOG("JIT: the first function compiled at run time: %#llx (%s+%#llx)", (unsigned long long)pc, sym ? sym : (mod ? mod : "?"), (unsigned long long)off);
    }
    g_default_miss(c, pc);   // guest code the ahead-of-time pass did not find: the recompiler's JIT compiles it now
}
// The guest ran an undefined instruction (or jumped into a host stub): reported and stopped.
static void on_undef(CPU *c, uint64_t pc, uint32_t word) {
    const char *h = hle_name_of(pc);
    if (h) die("jumped into the host stub of %s at %#llx", h, (unsigned long long)pc);
    module_t *m = module_of(pc);
    die("undefined instruction %#010x at %#llx (%s+%#llx), lr %#llx", word, (unsigned long long)pc, m ? m->name : "?",
        m ? (unsigned long long)(pc - (uint64_t)m->base) : 0ull, (unsigned long long)c->x[30]);
}
static void on_brk(CPU *c, uint64_t pc, uint32_t imm) {
    module_t *m = module_of(pc);
    die("BRK #%#x at %#llx (%s+%#llx), lr %#llx", imm, (unsigned long long)pc, m ? m->name : "?",
        m ? (unsigned long long)(pc - (uint64_t)m->base) : 0ull, (unsigned long long)c->x[30]);
}
static void on_wfe(CPU *c) { (void)c; sched_yield(); }

// page fault hooks (nxnative's graphics layer write-protects texture pages to see the game's writes): a hook that
// handles the fault (unprotects the page, notes it dirty) returns 1, and the faulting instruction runs again
#define MAX_FAULT_HOOK 4
static int (*g_fault_hook[MAX_FAULT_HOOK])(void *addr, int is_write, void *uc);
static _Atomic int g_nfault_hook;
void nxl_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc));
void nxl_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) {
    int i = atomic_fetch_add(&g_nfault_hook, 1);
    if (i >= MAX_FAULT_HOOK) die("too many page fault hooks");
    g_fault_hook[i] = fn;
}

// ---------- crash reports ----------
static void print_guest(const char *what, uint64_t a) {
    uint64_t off;
    const char *mod = NULL, *sym = module_symbolize(a, &off, &mod), *hle = hle_name_of(a);
    if (hle) fprintf(stderr, "  %-6s %#llx  host function %s\n", what, (unsigned long long)a, hle);
    else if (sym) {
        module_t *m = module_of(a);
        fprintf(stderr, "  %-6s %#llx  %s+%#llx  %s+%#llx\n", what, (unsigned long long)a, mod, (unsigned long long)(a - (uint64_t)(uintptr_t)m->base),
                sym, (unsigned long long)off);
    }
    else if (mod) fprintf(stderr, "  %-6s %#llx  %s+%#llx\n", what, (unsigned long long)a, mod, (unsigned long long)off);
    else fprintf(stderr, "  %-6s %#llx\n", what, (unsigned long long)a);
}
// the guest function whose recompiled code holds the host address rip: the one whose code starts nearest below it.
// The recompiled code names each function F_<guest address>, with its size (host_code_symbols): a host address maps
// to the exact guest function
typedef struct { uint64_t lo, hi, guest; } host_fn;
static host_fn *g_hfn;
static size_t g_nhfn, g_hfn_cap;
static pthread_once_t g_hfn_once = PTHREAD_ONCE_INIT;   // (declared before the status thread)
static int hfn_cmp(const void *a, const void *b) { const host_fn *x = a, *y = b; return x->lo < y->lo ? -1 : x->lo > y->lo; }
static void hfn_add(uint64_t lo, uint64_t hi, uint64_t guest) {
    if (g_nhfn == g_hfn_cap) g_hfn = realloc(g_hfn, (g_hfn_cap = g_hfn_cap ? g_hfn_cap * 2 : 4096) * sizeof *g_hfn);
    g_hfn[g_nhfn++] = (host_fn){lo, hi, guest};
}
static void hfn_load(void) {
    host_code_symbols(hfn_add);
    qsort(g_hfn, g_nhfn, sizeof *g_hfn, hfn_cmp);
    if (nxl_verbose) LOG("host-to-guest map: %zu recompiled functions by name", g_nhfn);
}
static uint64_t guest_fn_exact(uint64_t rip) {   // 0 when rip is not in a named recompiled function
    pthread_once(&g_hfn_once, hfn_load);
    size_t lo = 0, hi = g_nhfn;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (g_hfn[mid].lo <= rip) lo = mid + 1; else hi = mid; }
    return lo && rip < g_hfn[lo - 1].hi ? g_hfn[lo - 1].guest : 0;
}
// The guest function a host instruction address belongs to (by the dispatch table, the nearest function at or below
// it).
static uint64_t guest_fn_of_host(const CPU *c, uint64_t rip) {
    uint64_t exact = guest_fn_exact(rip);
    if (exact) return exact;
    if (!c->disp) return 0;
    uint64_t best_host = 0, best_guest = 0;
    for (uint64_t g = c->code_lo; g < c->code_hi; g += 4) {
        uint64_t h = (uint64_t)c->disp[(g - c->code_lo) >> 2];
        if (h && h <= rip && h > best_host) best_host = h, best_guest = g;
    }
    return rip - best_host < (1u << 20) ? best_guest : 0;   // nearer than 1 MiB: within its code
}
static int guest_readable(uint64_t a) {   // a guest stack or data address worth reading in a crash report
    return a >= g_aslr.base && a < (1ull << 39) && !(a & 7) && a - NXL_HLE_BASE >= NXL_HLE_SIZE && a - NXL_TRAMP_BASE >= NXL_TRAMP_SIZE;
}
// a guest code dump for the report: the instruction words of [a, a + n*4), 8 to a line, with module offsets
static void dump_code(const char *what, uint64_t a, int n) {
    module_t *m = module_of(a);
    if (!m || (a & 3)) return;
    uint64_t lo = (uint64_t)(uintptr_t)m->base + m->text_off, hi = lo + m->text_size;
    if (a < lo) a = lo;
    fprintf(stderr, "  code %s:\n", what);
    for (int k = 0; k < n && a + 4 * k < hi; k += 8) {
        fprintf(stderr, "    %s+%07llx:", m->name, (unsigned long long)(a + 4 * k - (uint64_t)(uintptr_t)m->base));
        for (int j = k; j < k + 8 && j < n && a + 4 * j < hi; j++) fprintf(stderr, " %08x", ((uint32_t *)(uintptr_t)a)[j]);
        fputc('\n', stderr);
    }
}
// what a call goes to: through an import stub (adrp x16; ldr x17, [x16, #off]; add x16, ...; br x17) the address its
// GOT slot holds (a host function or another module's), else the function itself
static void describe_target(uint64_t t, char *out, size_t n) {
    uint64_t off = 0, tgt = t;
    const char *mod = NULL, *how = "";
    module_t *m = module_of(t);
    if (m && !(t & 3)) {
        const uint32_t *w = (const uint32_t *)(uintptr_t)t;
        if ((w[0] & 0x9F00001F) == 0x90000010 && (w[1] & 0xFFC003FF) == 0xF9400211) {   // adrp x16 / ldr x17, [x16]
            int64_t imm = (int64_t)((((uint64_t)w[0] >> 29) & 3) | (((uint64_t)w[0] >> 3) & 0x1FFFFC));
            if (imm & 0x100000) imm -= 0x200000;
            uint64_t got = (t & ~0xFFFull) + (uint64_t)(imm << 12) + (((w[1] >> 10) & 0xFFF) << 3);
            tgt = *(const uint64_t *)(uintptr_t)got, how = "import → ";
        }
    }
    const char *h = hle_name_of(tgt);
    if (h) { snprintf(out, n, "%shost function %s", how, h); return; }
    const char *sym = module_symbolize(tgt, &off, &mod);
    if (sym) snprintf(out, n, "%s%s+%#llx", how, sym, (unsigned long long)off);
    else snprintf(out, n, "%s%#llx (%s)", how, (unsigned long long)tgt, mod ? mod : "no module");
}
static void dump_calls(uint64_t a, int n) {   // the calls (bl) in n words from a, with what they go to
    module_t *m = module_of(a);
    if (!m) return;
    for (int k = 0; k < n; k++) {
        uint32_t w = ((const uint32_t *)(uintptr_t)a)[k];
        if ((w & 0xFC000000) != 0x94000000) continue;
        int64_t imm = (int64_t)(w & 0x3FFFFFF);
        if (imm & 0x2000000) imm -= 0x4000000;
        char d[320];
        describe_target(a + 4 * k + (uint64_t)(imm * 4), d, sizeof d);
        fprintf(stderr, "    +%#05x bl %s\n", 4 * k, d);
    }
}
// the guest's call chain from its frame records ([fp] = the caller's fp, [fp + 8] = its return address), symbolized
static void guest_frames(const CPU *c, uint64_t frames[3], int *nframes) {
    uint64_t fp = c->x[29];
    *nframes = 0;
    print_guest("lr", c->x[30]);
    for (int k = 0; k < 24 && guest_readable(fp) && guest_readable(fp + 8); k++) {
        uint64_t next = ((uint64_t *)fp)[0], ret = ((uint64_t *)fp)[1];
        if (!ret) break;
        char what[16];
        snprintf(what, sizeof what, "#%d", k);
        print_guest(what, ret);
        if (frames && k < 3 && ret >= 0x60) frames[(*nframes)++] = ret;
        if (next <= fp) break;
        fp = next;
    }
}
// an SDK abort (svcBreak): where it came from, by name, and the host calls just before it (an answer the SDK rejected)
void nxl_break_report(CPU *c);
// Reports where the game aborted (nn::diag's abort, an SVC Break): its call chain, the last host calls and SVCs.
void nxl_break_report(CPU *c) {
    fprintf(stderr, "  the abort's call chain:\n");
    print_guest("at", c->pc);
    guest_frames(c, NULL, &(int){0});
    hle_print_recent(stderr);
    void svc_print_recent(FILE *f);
    svc_print_recent(stderr);
    // the code of the function that gave up: the first caller outside the SDK's abort machinery (nn::diag, nn::svc's Break),
    // from its start to the call that aborted, so its check can be read
    uint64_t fp = c->x[29];
    for (int k = 0; k < 24 && guest_readable(fp) && guest_readable(fp + 8); k++) {
        uint64_t next = ((uint64_t *)fp)[0], ret = ((uint64_t *)fp)[1], off = 0;
        const char *mod = NULL, *sym = ret ? module_symbolize(ret, &off, &mod) : NULL;
        if (!ret) break;
        if (sym && !strstr(sym, "4diag") && !strstr(sym, "3svc") && off < 0x800) {
            char what[256];
            snprintf(what, sizeof what, "of %s, from its start to the aborting call", sym);
            dump_code(what, ret - off, (int)(off / 4) + 2);
            fprintf(stderr, "  its calls (offsets from its start):\n");
            dump_calls(ret - off, (int)(off / 4));
            break;
        }
        if (next <= fp) break;
        fp = next;
    }
    fprintf(stderr, "  registers:");
    for (int k = 0; k < 31; k++) fprintf(stderr, "%s x%d %#llx", k % 4 ? " " : "\n   ", k, (unsigned long long)c->x[k]);
    fputc('\n', stderr);
}
// Reports a crash: the fault, the guest thread and function, its registers and code, the guest and host call chains.
static void crash_report(void *addr, uint64_t rip, const char *what) {
    nxl_thread *t = t_self;
    fflush(stdout);
    fprintf(stderr, "[RED] %s at address %p", what, addr);
    if (!t) { fprintf(stderr, " on a host thread\n"); }
    else {
        CPU *c = &t->cpu;
        fprintf(stderr, " in guest thread %llu\n", (unsigned long long)t->id);
        int in_guest = !host_addr_is_ours(rip);
        if (in_guest) print_guest("in", guest_fn_of_host(c, rip));
        else fprintf(stderr, "  in host code (backtrace below)\n");
        print_guest("lr", c->x[30]);
        const char *h = hle_last_call();
        if (h) fprintf(stderr, "  the thread's last host call: %s\n", h);
        if (svc_last() >= 0) fprintf(stderr, "  its last SVC: %#x\n", svc_last());
        hle_print_recent(stderr);
        fprintf(stderr, "  registers (as last stored by recompiled code, may lag the crash):");
        for (int k = 0; k < 32; k++) fprintf(stderr, "%s%s%d %#llx", k % 4 ? "  " : "\n    ", k == 31 ? "sp" : "x", k == 31 ? 0 : k, (unsigned long long)c->x[k]);
        fputc('\n', stderr);
        uint64_t fn = in_guest ? guest_fn_of_host(c, rip) : 0;
        if (fn) dump_code("of the faulting function, from its start", fn, 96);
        if (c->x[30] >= 0x80) dump_code("before lr", c->x[30] - 0x80, 40);
        uint64_t frames[3];
        int nframes = 0;
        guest_frames(c, frames, &nframes);
        for (int k = 0; k < nframes; k++) {
            char what[32];
            snprintf(what, sizeof what, "before frame #%d's return", k);
            dump_code(what, frames[k] - 0x60, 32);
        }
    }
    host_backtrace();
}

// The fault handler: the memory hooks first (dirty tracking, lazily mapped memory); 0 leads to the crash report.
static int on_fault(void *addr, int is_write, void *context) {
    for (int i = 0; i < atomic_load(&g_nfault_hook) && i < MAX_FAULT_HOOK; i++)
        if (g_fault_hook[i] && g_fault_hook[i](addr, is_write, context)) return 1;
    return 0;
}

// A function starts where the code before it cannot fall through: after a ret, an unconditional branch, a brk or udf, or
// padding (nop, zero). An address inside a function (a code address kept in data for another reason) mostly follows an
// ordinary instruction, and offering it as an entry makes the recompiler build an overlapping function from there
static int looks_like_entry(uint64_t v, uint64_t text) {
    if (v == text) return 1;
    uint32_t p = ((const uint32_t *)(uintptr_t)v)[-1];
    return p == 0xD65F03C0                       // ret
        || (p & 0xFC000000) == 0x14000000        // b
        || (p & 0xFFFFFC1F) == 0xD61F0000        // br xn
        || (p & 0xFFE0001F) == 0xD4200000        // brk
        || p == 0xD503201F || p == 0             // nop, zero padding
        || (p & 0xFFFF0000) == 0;                // udf
}
static int hint_cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
// Starts the recompiler (LLVM), with the code cache and a job per CPU core, and the fault handlers.
void cpu_init(const char *cache_dir) {
    g_rec = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    if (!g_rec) die("the recompiler has no backend for this machine");
    snx_recompiler_set_jobs(g_rec, (unsigned)host_cpu_count());
    if (cache_dir && snx_recompiler_set_cache(g_rec, cache_dir)) RED("cannot use %s as the code cache", cache_dir);
    snx_recompiler_set_verbose(g_rec, nxl_verbose > 1);
    struct snx_code_range ranges[2 * NXL_MAX_MODULES];
    size_t nh = 0, caph = 1024, nr = 0;
    uint64_t *hints = malloc(caph * 8);
    for (int i = 0; i < g_nmodules; i++) {
        module_t *m = g_modules[i];
        uint64_t text = (uint64_t)m->base + m->text_off;
        ranges[nr++] = (struct snx_code_range){text, (void *)text, m->text_size & ~3ull};
        if (m->fs_size) {   // nxnative's frame step stubs (loader.c install_fstep) and where their branches go
            uint64_t fa = (uint64_t)m->base + m->fs_off;
            ranges[nr++] = (struct snx_code_range){fa, (void *)fa, m->fs_size};
            for (int k = 0; k < m->fs_nhints; k++) {
                if (nh == caph) hints = realloc(hints, (caph *= 2) * 8);
                hints[nh++] = m->fs_hints[k];
            }
        }
        if (nh == caph) hints = realloc(hints, (caph *= 2) * 8);
        hints[nh++] = text;   // the module's entry
        const Elf64_Sym *st = m->symtab;
        for (uint32_t k = 1; k < m->nsyms; k++) {   // every function the module names
            if (st[k].st_shndx == SHN_UNDEF || ELF64_ST_TYPE(st[k].st_info) != STT_FUNC) continue;
            uint64_t a = (uint64_t)m->base + st[k].st_value;
            if (a - text >= m->text_size || (a & 3)) continue;
            if (nh == caph) hints = realloc(hints, (caph *= 2) * 8);
            hints[nh++] = a;
        }
        for (uint64_t k = 0; m->init_array && k < m->init_arraysz / 8; k++) {
            if (nh == caph) hints = realloc(hints, (caph *= 2) * 8);
            hints[nh++] = ((uint64_t *)(m->base + m->init_array))[k];
        }
    }
    // functions only data points at: vtables, callback tables, function pointers in .data. A C++ game reaches most of
    // its code through them, which following calls from the entries does not see; each was compiled when first reached,
    // one at a time, while the game waited (Breath of the Wild 1.9.0: a few hundred JIT misses every 10 s for minutes,
    // the main thread blocked behind them). Every aligned word of a module's read-only data and data that holds a
    // 4-aligned address inside any module's code is offered as an entry. NXL_DATA_HINTS=0: not
    const char *dh = getenv("NXL_DATA_HINTS");
    if (!dh || atoi(dh)) {
        size_t before = nh;
        for (int i = 0; i < g_nmodules; i++) {
            module_t *m = g_modules[i];
            const uint64_t *w = (const uint64_t *)(uintptr_t)((uint64_t)m->base + m->ro_off);
            uint64_t nw = ((m->data_off + m->data_size) - m->ro_off) / 8;   // ro, then data (relocated)
            for (uint64_t k = 0; k < nw; k++) {
                uint64_t v = w[k];
                if (v & 3) continue;
                for (int j = 0; j < g_nmodules; j++) {
                    uint64_t text = (uint64_t)g_modules[j]->base + g_modules[j]->text_off;
                    if (v - text < g_modules[j]->text_size) {
                        if (looks_like_entry(v, text)) {
                            if (nh == caph) hints = realloc(hints, (caph *= 2) * 8);
                            hints[nh++] = v;
                        }
                        break;
                    }
                }
            }
        }
        // duplicates (a function in many vtables) cost the recompiler nothing but a lookup; sort and drop them anyway
        qsort(hints, nh, 8, hint_cmp);
        size_t u = 0;
        for (size_t k = 0; k < nh; k++) if (!u || hints[k] != hints[u - 1]) hints[u++] = hints[k];
        LOG("%zu entry points (%zu more from function addresses in data)", u, nh - before);
        nh = u;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    g_mod = snx_recompile_ranges(g_rec, ranges, nr, hints, nh);
    if (!g_mod) die("recompiling failed: %s", snx_recompiler_error(g_rec));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    LOG("recompiled %zu functions in %.2f s", snx_recompiled_functions(g_mod),
        (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    free(hints);
    snx_runtime_defaults(&g_rt);
    g_default_miss = g_rt.miss;
    g_rt.svc = svc_handler;
    g_rt.undef = on_undef;
    g_rt.brk = on_brk;
    g_rt.miss = on_miss;
    g_rt.wfe = on_wfe;
    g_rt.ticks = guest_ticks;
    host_faults_install(on_fault, crash_report);
}

#define NXL_ELF_TLS_SIZE 0x2000
// A guest CPU's state at its start: identity memory, the runtime, its TLS (with the thread object Horizon puts there).
static void cpu_setup(CPU *c, uint8_t *tls) {
    memset(c, 0, sizeof *c);
    c->mem = NULL;   // identity: guest address a is host address a
    c->rt = &g_rt;
    c->tpidrro = (uint64_t)tls;
    // TLR+0x1f8: the SDK's current thread object (nn::os::GetCurrentThread). The SDK fills it in for the threads it
    // creates; nxnative gives every thread a zeroed object of its own first (0x400 bytes, here in the same page, after
    // the 0x200-byte region), so code that takes the current thread as an owner key never sees 0 (Super Mario Bros.
    // Wonder's per-thread allocator cache at main+0xd1ec94 matched a zero owner against zeroed TLS and read NULL+0x28)
    *(uint64_t *)(tls + 0x1f8) = (uint64_t)(tls + 0x200);
    // TPIDR_EL0: the thread pointer of the modules' ELF TLS. Horizon gives it no value, the game's code reads
    // tpidr_el0 + a link-time offset (Super Mario Bros. Wonder's main: +0x68 on the main thread). As nxnative does, each
    // thread starts with a zeroed 8 KiB block of its own; the guest may set its own pointer later
    c->tpidr = mem_stack_alloc(NXL_ELF_TLS_SIZE) - NXL_ELF_TLS_SIZE;
    c->fpcr = 0;
    if (snx_recompiled_bind(g_mod, c)) die("cannot bind a CPU to the recompiled code");
}

// A new guest thread (not started): its CPU state, TLS page in the guest's space, handle, priority and core.
nxl_thread *thread_new(uint64_t entry, uint64_t arg, uint64_t stack_top, int prio, int core) {
    nxl_thread *t = calloc(1, sizeof *t);
    t->entry = entry, t->arg = arg, t->stack_top = stack_top, t->prio = prio, t->core = core >= 0 && core <= 3 ? core : 0, t->core_mask = 1ull << t->core;   // -2: the process's default (core 0)
    t->tls = (uint8_t *)(mem_stack_alloc(0x1000) - 0x1000);   // in the guest's space, as Horizon's TLS pages are
    cpu_setup(&t->cpu, t->tls);
    pthread_mutex_lock(&g_threads_mu);
    t->id = g_next_tid++;
    t->next = g_threads, g_threads = t;
    pthread_mutex_unlock(&g_threads_mu);
    t->handle = handle_new(H_THREAD, t);
    return t;
}

// A guest thread's host thread: its fault handling, then the guest entry run until the thread exits.
static void *thread_main(void *p) {
    nxl_thread *t = p;
    t_self = t;
    host_thread_start();
    exit_jmp jb;
    t_exit_jmp = &jb;
    if (!EXIT_SETJMP(jb)) {
        CPU *c = &t->cpu;
        c->x[31] = t->stack_top & ~15ull;
        if (t->host_entry) {
            t->host_entry();
            goto done;
        }
        c->x[0] = t->arg;
        c->x[1] = t->handle;
        c->x[31] = t->stack_top & ~15ull;
        c->x[30] = NXL_STOP_ADDR;
        snx_recompiled_run(g_mod, c, t->entry, NXL_STOP_ADDR);
        LOG("thread %llu returned from its entry", (unsigned long long)t->id);
    }
done:
    t = t_self;   // (after the jump the locals are not to be trusted: the thread's own pointer again, from its TLS)
    pthread_mutex_lock(&g_sched);
    t->exited = 1;
    sync_signal_object(t);
    pthread_mutex_unlock(&g_sched);
    return NULL;
}

// Starts a guest thread on a host thread (detached, with a large stack).
Result thread_start(nxl_thread *t) {
    if (t->started) return KERR_INVALID_STATE;
    t->started = 1;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, HOST_STACK);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int e = pthread_create(&t->pt, &a, thread_main, t);
    pthread_attr_destroy(&a);
    return e ? KERR_OUT_OF_MEMORY : RES_OK;
}

// The number of guest threads that have not exited.
int thread_count_live(void) {
    int n = 0;
    pthread_mutex_lock(&g_threads_mu);
    for (nxl_thread *t = g_threads; t; t = t->next) n += !t->exited;
    pthread_mutex_unlock(&g_threads_mu);
    return n;
}

_Noreturn void thread_exit_current(void) {
    if (!t_exit_jmp) die("svcExitThread outside a guest thread");
    EXIT_LONGJMP(*t_exit_jmp);   // leaves the generated code's frames: the thread is over
}

static uint32_t g_main_handle;
uint32_t main_thread_handle(void) { return g_main_handle; }

// Runs the game's main thread (priority and stack from its NPDM) until it exits; the process exit code.
int cpu_run_main(uint64_t entry, void (*host_entry)(void)) {
    // the main thread's priority and stack size come from the NPDM (at least 1 MiB: the null NPDM of a test program)
    uint32_t stack = g_profile.main_stack < (1u << 20) ? (1u << 20) : (g_profile.main_stack + 0xFFF) & ~0xFFFu;
    nxl_thread *t = thread_new(entry, 0, mem_stack_alloc(stack), g_profile.main_prio, 0);
    t->host_entry = host_entry;
    g_main_handle = t->handle;
    // x0 = 0 (a normal application start), x1 = the main thread's handle: what Horizon's loader passes
    thread_start(t);
    pthread_mutex_lock(&g_sched);
    while (!t->exited) {
        int32_t idx;
        pthread_mutex_unlock(&g_sched);
        sync_wait_handles(&t->handle, 1, -1, &idx);
        pthread_mutex_lock(&g_sched);
    }
    pthread_mutex_unlock(&g_sched);
    return 0;
}

// a call from host code into guest code (a callback the game registered, a guest function the host wraps), on the
// calling guest thread's own CPU: the state is saved, the function runs to its return, and the state is restored
static CPU *call_cpu(void) {
    nxl_thread *t = t_self;
    static __thread CPU *foreign;   // a host thread with no guest thread of its own (an audio callback, say)
    if (t) return &t->cpu;
    if (!foreign) {
        foreign = malloc(sizeof *foreign);
        uint8_t *tls = (uint8_t *)(mem_stack_alloc(0x1000) - 0x1000);
        cpu_setup(foreign, tls);
        foreign->x[31] = mem_stack_alloc(1u << 20);
    }
    return foreign;
}

// x0-x7 from ints (8 values), v0-v7 from fps (8 values, may be NULL); returns x0, *x1 gets x1, fps[0] gets v0
// a traced call (NXL_TRACE): run the guest function as the original call would have, with every register as the
// caller left it, and return with them as the callee left them (the caller's return address is put back after)
void nxl_call_through(CPU *c, uint64_t fn) {
    uint64_t lr = c->x[30];
    c->x[30] = NXL_STOP_ADDR;
    snx_recompiled_run(g_mod, c, fn, NXL_STOP_ADDR);
    c->x[30] = lr;
}

// Calls a guest function from the host, with up to 8 integer and 8 vector arguments; x0 and x1 back.
uint64_t guest_call_full(uint64_t fn, const uint64_t *ints, V128 *fps, uint64_t *x1) {
    fn = guest_addr_of((const void *)fn);
    CPU *c = call_cpu();
    CPU save;
    memcpy(&save, c, sizeof save);
    for (int i = 0; i < 8; i++) c->x[i] = ints[i];
    if (fps) memcpy(c->v, fps, 8 * sizeof *fps);
    c->x[30] = NXL_STOP_ADDR;
    c->x[31] &= ~15ull;
    snx_recompiled_run(g_mod, c, fn, NXL_STOP_ADDR);
    uint64_t r = c->x[0];
    if (x1) *x1 = c->x[1];
    if (fps) fps[0] = c->v[0];
    uint64_t tp = c->tpidr;
    memcpy(c, &save, sizeof save);
    // TPIDR_EL0 is the thread's, not the call's: the SDK sets the thread's ELF TLS pointer in its start-up calls
    // (sdk_boot runs them through here) and it must stay set when they return
    c->tpidr = tp;
    return r;
}

// Calls a guest function from the host with up to 8 integer arguments.
uint64_t guest_callv(uint64_t fn, int n, const uint64_t *args) {
    if (n > 8) die("guest_call with more than 8 arguments");
    uint64_t ints[8] = {0};
    memcpy(ints, args, n * sizeof *args);
    return guest_call_full(fn, ints, NULL, NULL);
}
