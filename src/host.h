// SPDX-License-Identifier: GPL-3.0-or-later
// nxlinux's needs of the host system that differ between Linux (src/host_posix.c) and Windows (src/host_win32.c):
// the guest's memory at fixed addresses, the fault handler (dirty tracking of textures, crash reports), where a
// thread's code is now (the status line's samples), CPU times, the host's backtrace and the recompiled code's symbols.
// What NativeNX shares with nxlinux (processes, folders) is in platform.h.
#pragma once
#include <stddef.h>
#include <stdint.h>

// ---- the guest's memory: reserved at its own addresses, committed as the guest maps it ----
enum { HOST_VM_NONE, HOST_VM_R, HOST_VM_RW, HOST_VM_RX };
// address space kept for later (inaccessible, no memory behind it), at exactly a; 0 when it got that
int host_vm_reserve(uint64_t a, uint64_t size);
// fresh zeroed read-write pages in a reserved range (whatever was there is gone); 0 when done
int host_vm_commit(uint64_t a, uint64_t size);
// the pages given back, the range reserved again (it reads as zero when committed again)
void host_vm_decommit(uint64_t a, uint64_t size);
// committed pages made inaccessible, read-only, read-write or read-execute; 0 when done
int host_vm_protect(uint64_t a, uint64_t size, int prot);
// read-write pages at exactly a outside any reservation (the old 36-bit map region), failing when anything is there;
// host_vm_release gives them back
int host_vm_map_new(uint64_t a, uint64_t size);
void host_vm_release(uint64_t a, uint64_t size);
// the system's error for the last failed call above, as text
const char *host_vm_error(void);

// ---- faults ----
// the handler for every access fault: it returns 1 when it dealt with it (the access runs again), else nxlinux's crash
// report is printed (host_fault_crash's report function) and the program ends. is_write: the access was a write.
typedef int (*host_fault_fn)(void *addr, int is_write, void *context);
// report(addr, is_write, host instruction pointer, what: "segmentation fault" ...): before the program ends
typedef void (*host_crash_fn)(void *addr, uint64_t rip, const char *what);
void host_faults_install(host_fault_fn handler, host_crash_fn report);
// a guest thread's host thread starts: what its fault handling needs (a stack of its own for the handler, so that an
// overflow of the thread's stack still reports)
void host_thread_start(void);

// ---- threads and time ----
struct nxl_thread;
// the host instruction pointer of a running thread now, 0 when it could not be read (the status line's samples)
uint64_t host_thread_sample(struct nxl_thread *t);
// a thread's CPU time and the process's, in nanoseconds (0 when unknown)
uint64_t host_thread_cpu_ns(struct nxl_thread *t);
uint64_t host_process_cpu_ns(void);
int host_cpu_count(void);
// whether a host code address is nxlinux's own (not recompiled code), and its code's bounds
int host_addr_is_ours(uint64_t a);
void host_own_code(uint64_t *lo, uint64_t *hi);
// the host call stack into stderr (crash reports)
void host_backtrace(void);
// each recompiled function the loaded code names (F_<guest address>), its host code's range: add(lo, hi, guest)
void host_code_symbols(void (*add)(uint64_t lo, uint64_t hi, uint64_t guest));
