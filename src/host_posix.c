// SPDX-License-Identifier: GPL-3.0-or-later
// host.h on Linux: mmap and mprotect, the SIGSEGV / SIGBUS handler on an alternate stack, SIGURG samples, the ELF
// symbol tables of the loaded recompiled code (dl_iterate_phdr).
#include "nxl.h"
#include "host.h"
#include <elf.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

// ---- memory ----
static int g_vm_errno;
static int fail(void) { g_vm_errno = errno; return -1; }
const char *host_vm_error(void) { return strerror(g_vm_errno); }
int host_vm_reserve(uint64_t a, uint64_t size) {
    void *p = mmap((void *)a, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return fail();
    if (p != (void *)a) { munmap(p, size); g_vm_errno = EEXIST; return -1; }   // (a kernel without MAP_FIXED_NOREPLACE placed it elsewhere)
    return 0;
}
int host_vm_commit(uint64_t a, uint64_t size) {
    return mmap((void *)a, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED ? fail() : 0;
}
void host_vm_decommit(uint64_t a, uint64_t size) {
    if (mmap((void *)a, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) == MAP_FAILED)
        die("mmap reserve %#llx: %s", (unsigned long long)a, strerror(errno));
}
int host_vm_protect(uint64_t a, uint64_t size, int prot) {
    static const int p[] = {PROT_NONE, PROT_READ, PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC};
    return mprotect((void *)a, size, p[prot]) ? fail() : 0;
}
int host_vm_map_new(uint64_t a, uint64_t size) {
    void *p = mmap((void *)a, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return fail();
    if (p != (void *)a) { munmap(p, size); g_vm_errno = EEXIST; return -1; }
    return 0;
}
void host_vm_release(uint64_t a, uint64_t size) { munmap((void *)a, size); }

// ---- faults ----
static host_fault_fn g_fault;
static host_crash_fn g_crash;
static void on_fault(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    if (sig == SIGSEGV) {
        int is_write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;   // x86-64 page fault error code: bit 1 = write
        if (g_fault && g_fault(si->si_addr, is_write, uc)) return;
    }
    if (g_crash) g_crash(si->si_addr, (uint64_t)uc->uc_mcontext.gregs[REG_RIP], sig == SIGSEGV ? "segmentation fault" : "bus error");
    signal(sig, SIG_DFL);
    raise(sig);
}
void host_faults_install(host_fault_fn handler, host_crash_fn report) {
    g_fault = handler, g_crash = report;
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}
void host_thread_start(void) {
    stack_t ss = {.ss_sp = malloc(1 << 16), .ss_size = 1 << 16};
    sigaltstack(&ss, NULL);
}

// ---- threads and time ----
// SIGURG makes the thread note its host program counter
static void on_sample(int sig, siginfo_t *si, void *ucv) {
    (void)sig, (void)si;
    nxl_thread *t = nxl_self();
    if (t) atomic_store(&t->sample_rip, (uint64_t)((ucontext_t *)ucv)->uc_mcontext.gregs[REG_RIP]);
}
static void sample_setup(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGURG, &sa, NULL);
}
uint64_t host_thread_sample(struct nxl_thread *t) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, sample_setup);
    atomic_store(&t->sample_rip, 0);
    if (pthread_kill(t->pt, SIGURG)) return 0;
    for (int w = 0; w < 20 && !atomic_load(&t->sample_rip); w++) { struct timespec ts = {0, 250000}; nanosleep(&ts, NULL); }
    return atomic_load(&t->sample_rip);
}
uint64_t host_thread_cpu_ns(struct nxl_thread *t) {
    clockid_t id;
    struct timespec ts;
    if (pthread_getcpuclockid(t->pt, &id) || clock_gettime(id, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t host_process_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
int host_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 1 ? (int)n : 1;
}
extern const char __executable_start[], etext[];
int host_addr_is_ours(uint64_t a) { return a >= (uint64_t)__executable_start && a < (uint64_t)etext; }
void host_own_code(uint64_t *lo, uint64_t *hi) { *lo = (uint64_t)__executable_start, *hi = (uint64_t)etext; }
void host_backtrace(void) {
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "  host backtrace:\n");
    backtrace_symbols_fd(bt, n, 2);
}

// The recompiled library names each function F_<guest address> in its symbol table, with its size: read from the
// loaded library's file (dl_iterate_phdr gives its path and base)
static void (*g_add)(uint64_t, uint64_t, uint64_t);
static void elf_symbols(const char *path, uint64_t base) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    struct stat st;
    void *f = fstat(fd, &st) || st.st_size < (off_t)sizeof(Elf64_Ehdr) ? MAP_FAILED : mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (f == MAP_FAILED) return;
    const Elf64_Ehdr *eh = f;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > (uint64_t)st.st_size) { munmap(f, (size_t)st.st_size); return; }
    const Elf64_Shdr *sh = (const Elf64_Shdr *)((const uint8_t *)f + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_link >= eh->e_shnum) continue;
        const Elf64_Sym *sym = (const Elf64_Sym *)((const uint8_t *)f + sh[i].sh_offset);
        const char *str = (const char *)f + sh[sh[i].sh_link].sh_offset;
        size_t n = sh[i].sh_size / sizeof *sym;
        for (size_t k = 0; k < n; k++) {
            const char *nm = str + sym[k].st_name;
            if (nm[0] != 'F' || nm[1] != '_' || !sym[k].st_value) continue;
            uint64_t g = strtoull(nm + 2, NULL, 16);
            if (g) g_add(base + sym[k].st_value, base + sym[k].st_value + (sym[k].st_size ? sym[k].st_size : 4), g);
        }
    }
    munmap(f, (size_t)st.st_size);
}
static int each_library(struct dl_phdr_info *info, size_t size, void *arg) {
    (void)size, (void)arg;
    const char *n = info->dlpi_name;
    if (n && *n && (strstr(n, ".so") || strstr(n, "/proc/self/fd/"))) elf_symbols(n, info->dlpi_addr);
    return 0;
}
void host_code_symbols(void (*add)(uint64_t lo, uint64_t hi, uint64_t guest)) {
    g_add = add;
    dl_iterate_phdr(each_library, NULL);
}
