// SPDX-License-Identifier: GPL-3.0-or-later
// host.h on Windows: VirtualAlloc and VirtualProtect, a vectored exception handler for the memory hooks (it runs before
// any other handler, as the SIGSEGV handler does on Linux) and the unhandled exception filter for the crash report,
// samples by suspending the thread and reading its context.
#include "nxl.h"
#include "host.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdatomic.h>

// ---- memory ----
// A reservation's start must be on the allocation granularity (64 KiB); the guest's regions are. Committing and
// protecting work on 4 KiB pages inside one reservation, as mmap and mprotect do.
static DWORD g_vm_err;
static int fail(void) { g_vm_err = GetLastError(); return -1; }
const char *host_vm_error(void) {
    static char b[256];
    if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, g_vm_err, 0, b, sizeof b, NULL))
        snprintf(b, sizeof b, "Windows error %lu", (unsigned long)g_vm_err);
    for (char *c = b + strlen(b); c > b && (c[-1] == '\n' || c[-1] == '\r' || c[-1] == '.'); ) *--c = 0;
    return b;
}
int host_vm_reserve(uint64_t a, uint64_t size) {
    void *p = VirtualAlloc((void *)a, size, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) return fail();
    if (p != (void *)a) { VirtualFree(p, 0, MEM_RELEASE); g_vm_err = ERROR_INVALID_ADDRESS; return -1; }
    return 0;
}
int host_vm_commit(uint64_t a, uint64_t size) {
    // committed pages that are committed already keep what they hold: given back first, so these read as zero
    VirtualFree((void *)a, size, MEM_DECOMMIT);
    return VirtualAlloc((void *)a, size, MEM_COMMIT, PAGE_READWRITE) ? 0 : fail();
}
void host_vm_decommit(uint64_t a, uint64_t size) {
    if (!VirtualFree((void *)a, size, MEM_DECOMMIT)) die("cannot give back %#llx+%#llx: %s", (unsigned long long)a, (unsigned long long)size, (fail(), host_vm_error()));
}
int host_vm_protect(uint64_t a, uint64_t size, int prot) {
    static const DWORD p[] = {PAGE_NOACCESS, PAGE_READONLY, PAGE_READWRITE, PAGE_EXECUTE_READ};
    DWORD old;
    return VirtualProtect((void *)a, size, p[prot], &old) ? 0 : fail();
}
// (a start inside a 64 KiB granule is not possible on Windows: the SDK's random addresses that are not on one are
// refused, and it picks another)
int host_vm_map_new(uint64_t a, uint64_t size) {
    if (a & 0xFFFF) { g_vm_err = ERROR_INVALID_ADDRESS; return -1; }
    void *p = VirtualAlloc((void *)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) return fail();
    if (p != (void *)a) { VirtualFree(p, 0, MEM_RELEASE); g_vm_err = ERROR_INVALID_ADDRESS; return -1; }
    return 0;
}
void host_vm_release(uint64_t a, uint64_t size) { (void)size; VirtualFree((void *)a, 0, MEM_RELEASE); }

// ---- faults ----
static host_fault_fn g_fault;
static host_crash_fn g_crash;
// first, before any other handler: an access violation the memory hooks deal with (a write to a texture's page) runs again
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || !g_fault) return EXCEPTION_CONTINUE_SEARCH;
    int is_write = er->ExceptionInformation[0] == 1;   // 0 read, 1 write, 8 execution
    return g_fault((void *)er->ExceptionInformation[1], is_write, ep->ContextRecord) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
// last, when nothing handled it: the crash report
static LONG WINAPI on_unhandled(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    const char *what = "exception";
    char b[64];
    switch (er->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: what = "segmentation fault (access violation)"; break;
    case EXCEPTION_STACK_OVERFLOW: what = "stack overflow"; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: what = "illegal instruction"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: what = "division by zero"; break;
    default: snprintf(b, sizeof b, "exception %#lx", (unsigned long)er->ExceptionCode); what = b;
    }
    void *addr = er->NumberParameters >= 2 && er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? (void *)er->ExceptionInformation[1] : er->ExceptionAddress;
    if (g_crash) g_crash(addr, ep->ContextRecord->Rip, what);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;   // the process ends
}
void host_faults_install(host_fault_fn handler, host_crash_fn report) {
    g_fault = handler, g_crash = report;
    AddVectoredExceptionHandler(1, on_exception);
    SetUnhandledExceptionFilter(on_unhandled);
    host_thread_start();
}
void host_thread_start(void) {
    ULONG keep = 64 << 10;   // room for the crash report when the thread's stack overflows
    SetThreadStackGuarantee(&keep);
}

// ---- threads and time ----
uint64_t host_thread_sample(struct nxl_thread *t) {
    HANDLE h = pthread_gethandle(t->pt);
    if (!h || SuspendThread(h) == (DWORD)-1) return 0;
    CONTEXT c = {.ContextFlags = CONTEXT_CONTROL};
    uint64_t rip = GetThreadContext(h, &c) ? c.Rip : 0;
    ResumeThread(h);
    return rip;
}
static uint64_t ft_ns(FILETIME f) { return (((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) * 100; }
uint64_t host_thread_cpu_ns(struct nxl_thread *t) {
    FILETIME c, e, k, u;
    HANDLE h = pthread_gethandle(t->pt);
    return h && GetThreadTimes(h, &c, &e, &k, &u) ? ft_ns(k) + ft_ns(u) : 0;
}
uint64_t host_process_cpu_ns(void) {
    FILETIME c, e, k, u;
    return GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? ft_ns(k) + ft_ns(u) : 0;
}
int host_cpu_count(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 1 ? (int)n : 1;
}
// this program's own image (its code and the static libraries in it)
void host_own_code(uint64_t *lo, uint64_t *hi) {
    static uint64_t l, h;
    if (!l) {
        MODULEINFO mi;
        if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(NULL), &mi, sizeof mi))
            l = (uint64_t)mi.lpBaseOfDll, h = l + mi.SizeOfImage;
    }
    *lo = l, *hi = h;
}
int host_addr_is_ours(uint64_t a) {
    uint64_t lo, hi;
    host_own_code(&lo, &hi);
    return a >= lo && a < hi;
}
// the return addresses up the host's stack (Windows walks it by the unwind tables, the recompiled code's included),
// each as module+offset
void host_backtrace(void) {
    void *bt[32];
    USHORT n = RtlCaptureStackBackTrace(1, 32, bt, NULL);
    fprintf(stderr, "  host backtrace:\n");
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = NULL;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, bt[i], &m) && m) {
            GetModuleFileNameA(m, name, sizeof name);
            const char *b = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
            fprintf(stderr, "    %s+%#llx\n", b, (unsigned long long)((uint64_t)bt[i] - (uint64_t)m));
        } else fprintf(stderr, "    %p\n", bt[i]);
    }
}
// (the recompiled code's function names are not read on Windows yet: a sample maps back through the dispatch table)
void host_code_symbols(void (*add)(uint64_t lo, uint64_t hi, uint64_t guest)) { (void)add; }
