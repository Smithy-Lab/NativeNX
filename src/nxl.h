// SPDX-License-Identifier: GPL-3.0-or-later
// nxlinux: runs Nintendo Switch program modules natively on x86_64 Linux.
// The game's ARM64 code is recompiled to x86_64 by ScarletNX's recompiler library; this program supplies what the
// Switch's kernel (SVCs) and SDK (HLE functions, nxnative's model) would. Guest and host share one address space with
// guest address == host address, so host C code reads and writes the game's structures through ordinary pointers.
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <scarletnx/snx_recompiler.h>
#include "hle_abi.h"

// ---------- logging ----------
extern int nxl_verbose;
void nxl_log(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOG(...) do { if (nxl_verbose) nxl_log("[nxl] ", __VA_ARGS__); } while (0)
#define RED(...) nxl_log("[RED] ", __VA_ARGS__)   // something unimplemented or wrong (nxnative's convention)
_Noreturn void die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---------- results (nn::Result values the kernel returns) ----------
typedef uint32_t Result;
#define RES_OK              0u
#define KERR_INVALID_SIZE   0xCA01u
#define KERR_INVALID_ADDR   0xCC01u
#define KERR_OUT_OF_MEMORY  0xD001u
#define KERR_INVALID_MEM_STATE 0xD401u   // InvalidCurrentMemoryState
#define KERR_INVALID_HANDLE 0xE401u
#define KERR_TIMED_OUT      0xEA01u
#define KERR_CANCELLED      0xEC01u
#define KERR_INVALID_STATE  0xFA01u
#define KERR_NOT_IMPL       0xF601u   // "unsupported operation": also what an SVC this host lacks returns

// ---------- the guest address space (memory.c) ----------
// A 39-bit Horizon address space, from 32 GiB to 512 GiB. On x86_64 Linux that range is unused by the host (its
// libraries, mmap allocations and thread stacks live near the top of the 47-bit space, a PIE executable and its heap at
// 0x55..., a non-PIE executable and its heap below 4 GiB), so the guest's regions are reserved at their own addresses.
typedef struct { uint64_t base, size; } region_t;
extern region_t g_aslr, g_heap, g_alias, g_stack;
#define NXL_HLE_BASE  0x7FFF000000ull   // stub "addresses" of host functions: reserved, never mapped
#define NXL_HLE_SIZE  0x0000100000ull   // 256K stubs of 4 bytes
#define NXL_STOP_ADDR (NXL_HLE_BASE + NXL_HLE_SIZE - 4)   // return address that ends a run (top-level and guest_call)
void mem_init(void);
uint64_t mem_map_module(uint64_t size);               // next free, page-aligned address in the ASLR region, mapped RW
Result mem_set_heap(uint64_t size, uint64_t *out);    // svcSetHeapSize
uint64_t mem_heap_used(void);
uint64_t mem_stack_alloc(uint64_t size);              // a guest stack (thread stacks the host makes), returns its top
Result mem_map(uint64_t dst, uint64_t src, uint64_t size);    // svcMapMemory: src's contents move to dst (stack region)
Result mem_unmap(uint64_t dst, uint64_t src, uint64_t size);  // svcUnmapMemory: back to src
typedef struct { uint64_t addr, size; uint32_t type, attr, perm, ipc_refcount, device_refcount, pad; } MemoryInfo;
void mem_query(uint64_t addr, MemoryInfo *out);       // svcQueryMemory
void mem_protect(uint64_t a, uint64_t s, int writable, uint32_t type, uint32_t perm);
Result mem_map_physical(uint64_t a, uint64_t s, int map);   // svcMapPhysicalMemory (map) / svcUnmapPhysicalMemory
uint64_t mem_alias_mapped(void);

// ---------- modules (loader.c) ----------
typedef struct module {
    char name[64];
    uint8_t *base;            // == its guest address
    uint64_t size;            // whole image, page aligned (text, ro, data, bss)
    uint64_t text_off, text_size, ro_off, ro_size, data_off, data_size, bss_size;
    uint8_t build_id[32];
    const uint64_t *dynamic;  // Elf64_Dyn pairs
    const void *symtab;       // Elf64_Sym[]
    const char *strtab;
    uint32_t nsyms;
    uint32_t *symhash, symhash_mask;
    uint64_t init_array, init_arraysz, init;
    // nxnative's frame step (loader.c install_fstep): its stubs' patch area and state page, mapped after the image (0 = none)
    uint64_t fs_off, fs_size, fs_state_off;
    uint64_t *fs_hints; int fs_nhints;   // where the stubs' branches go (entries for the translator)
} module_t;
#define NXL_MAX_MODULES 32
extern module_t *g_modules[NXL_MAX_MODULES];
extern int g_nmodules;
module_t *nso_load(const char *path);                 // map one NSO (main, subsdkN, sdk or a test module)
void loader_link_all(void);                           // relocate every module once all are mapped (imports across them)
uint64_t guest_sym(const char *name);                 // a symbol some module defines (0 if none)
module_t *module_of(uint64_t addr);
int loader_is_gpu_driver(module_t *m);                  // a GPU driver of the game's (EGL / Vulkan), not run (loader.c)
const char *loader_gfx_api(void);                       // what the game draws with: "NVN", "OpenGL", "Vulkan" or "none"
int loader_unresolved(void);                          // imports bound to "missing" stubs
const char *module_symbolize(uint64_t addr, uint64_t *off, const char **mod);   // nearest function symbol (crash reports)
void hle_print_recent(FILE *f);                        // the calling thread's last host calls (crash reports)
uint64_t hle_trace_stub(const char *name, uint64_t target);   // NXL_TRACE: a logging stub in front of a guest function (or target)
void nxl_call_through(CPU *c, uint64_t fn);
const char *hle_last_call(void);                      // the calling thread's last host function called by guest code
int svc_last(void);                                   // the calling thread's last SVC number

// ---------- HLE: host implementations of guest functions (hle.c, hle.h) ----------
void hle_init(void);
uint64_t hle_lookup(const char *name, int force_only);  // stub address of a host implementation, 0 if none
uint64_t hle_missing(const char *name);                 // a stub that reports a call to an import nothing implements
int hle_dispatch(CPU *c, uint64_t pc);                  // a call to a stub: 1 if handled (c->pc is then the return address)
size_t hle_count(void);
const char *hle_name_of(uint64_t pc);

// ---------- threads and the recompiled code (cpu.c) ----------
typedef struct nxl_thread {
    CPU cpu;
    uint32_t handle;
    uint64_t id;
    pthread_t pt;
    uint64_t entry, arg, stack_top;
    int prio, core;           // ideal core: 0-2, the application's cores
    uint64_t core_mask;       // affinity mask (bookkeeping: Linux places the host threads)
    uint8_t *tls;             // the 0x200-byte TLS area TPIDRRO_EL0 points at
    int started, exited;
    void (*host_entry)(void);   // a host routine to run on this guest thread instead of guest code (the SDK boot)
    // what the thread is doing, for the -v status lines: the host function or SVC it is in (or last left)
    const char *volatile last_hle;
    volatile int last_svc, in_hle, in_svc;
    volatile uint64_t svc_arg[3];   // the SVC it is in: its first arguments (status lines: which lock or key it waits on)
    uint64_t status_cpu_ns;     // the thread's CPU time at the last status line
    _Atomic uint64_t sample_rip;   // where a status sample (SIGURG) found it running, on the host
    int sync_cancel;            // CancelSynchronization asked for its (next) WaitSynchronization to end (under g_sched)
    struct nxl_thread *next;
} nxl_thread;
void cpu_init(const char *cache_dir);                   // recompile every loaded module (after loader_link_all)
nxl_thread *thread_new(uint64_t entry, uint64_t arg, uint64_t stack_top, int prio, int core);
Result thread_start(nxl_thread *t);
_Noreturn void thread_exit_current(void);
void nxl_status_start(void);                            // -v: a status line every 10 s (frames, what each thread is doing)
uint64_t nxl_frames_presented(void);                    // nvn_null.c
nxl_thread *nxl_self(void);                             // the calling host thread's guest thread (NULL off guest threads)
int cpu_run_main(uint64_t entry, void (*host_entry)(void));   // runs the main thread to its end (guest entry, or a host routine)
uint32_t main_thread_handle(void);
// guest_callv, guest_call_full, host_callable, guest_addr_of, GUEST_CALL: hle_abi.h
#define NXL_TRAMP_BASE 0x7FFE000000ull    // host_callable stubs: reserved, mapped executable a page at a time
#define NXL_TRAMP_SIZE 0x0001000000ull
uint64_t guest_ticks(void);                             // 19.2 MHz system counter (CNTPCT_EL0, svcGetSystemTick)
int guest_sym_is_func(const char *name);                // the symbol some module defines is a function (loader.c)

// ---------- kernel objects and SVCs (svc.c, sync.c) ----------
enum { H_FREE, H_THREAD, H_EVENT_W, H_EVENT_R, H_PROCESS, H_RESLIMIT };
#define CUR_THREAD_HANDLE  0xFFFF8000u
#define CUR_PROCESS_HANDLE 0xFFFF8001u
uint32_t handle_new(int type, void *obj);
void *handle_get(uint32_t h, int type);
Result handle_close(uint32_t h);
void svc_handler(CPU *c, uint32_t imm);
// synchronization (sync.c): mutex arbitration, condition variables, events, waits
extern pthread_mutex_t g_sched;   // one lock over every kernel wait (the guest's fast paths are its own atomics)
Result sync_arbitrate_lock(uint32_t owner, uint64_t addr, uint32_t self);
Result sync_arbitrate_unlock(uint64_t addr);
Result sync_wait_key(uint64_t mutex, uint64_t key, uint32_t self, int64_t timeout_ns);
Result sync_signal_key(uint64_t key, int32_t count);
Result sync_wait_handles(const uint32_t *handles, int n, int64_t timeout_ns, int32_t *index);
Result sync_wait_for_address(uint64_t addr, int type, uint64_t value, int64_t timeout_ns);
Result sync_signal_to_address(uint64_t addr, int type, int32_t val, int32_t count);
void sync_arb_report(uint64_t addr);   // the freeze report: an address's waiters and last operations
void sync_signal_object(void *obj);   // a thread exited or an event was signaled: wake waiters
typedef struct { int signaled; } nxl_event;

// ---------- the program's own description (profile.c): its NPDM and nxnative's game profile, if any ----------
typedef struct {
    uint64_t program_id, mem_alias, mem_stack, mem_reserve, app_pool, mem_used, sys_resource;
    int mem_map_limit, main_prio, have_npdm, have_profile;
    uint32_t main_stack;
    char name[128], path[4096];
} nxl_profile;
extern nxl_profile g_profile;
void profile_load(const char *exefs, const char *profile_arg);   // before mem_init: it sizes the regions
#ifndef NXL_DEFAULT_GAMES_DIR
#define NXL_DEFAULT_GAMES_DIR ""
#endif

// process exit
_Noreturn void nxl_exit(int code);
extern uint64_t g_program_id;
