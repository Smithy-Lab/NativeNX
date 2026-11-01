// SPDX-License-Identifier: GPL-3.0-or-later
// The Mach calls nxnative's sources make, on Linux. Statistics and scheduling-policy queries (task_info, thread_info,
// thread_policy_*) report failure, which nxnative treats as "not available" and skips. vm_read_overwrite is a guarded
// read (process_vm_readv on ourselves). vm_remap, used to alias pages for sparse GPU memory pools, reports failure:
// nxnative then keeps those mappings in its page table only (a few games, such as Monster Hunter Rise, use them).
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <pthread.h>
#ifndef _WIN32
#include <sys/uio.h>
#endif
#include <unistd.h>
#include <mach/mach_time.h>
typedef uintptr_t vm_address_t, vm_offset_t;
typedef uintptr_t vm_size_t;
typedef unsigned mach_port_t, mach_msg_type_number_t, natural_t, thread_act_t, task_t;
typedef int kern_return_t, vm_prot_t, vm_inherit_t, boolean_t, integer_t, thread_flavor_t, task_flavor_t, thread_policy_flavor_t;
typedef int *vm_region_info_t, *task_info_t, *thread_info_t, *thread_policy_t;
typedef struct { int protection, max_protection; } vm_region_basic_info_data_64_t;
#define KERN_SUCCESS 0
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif
#define KERN_FAILURE 5
#define MACH_PORT_NULL 0u
#define VM_REGION_BASIC_INFO_64 9
#define VM_REGION_BASIC_INFO_COUNT_64 ((mach_msg_type_number_t)(sizeof(vm_region_basic_info_data_64_t) / sizeof(int)))
#define VM_FLAGS_FIXED 0x0000
#define VM_FLAGS_OVERWRITE 0x4000
#define VM_INHERIT_NONE 2
static inline mach_port_t mach_task_self(void) { return 0; }
static inline mach_port_t mach_thread_self(void) { return 0; }
static inline kern_return_t mach_port_deallocate(mach_port_t t, mach_port_t p) { (void)t; (void)p; return KERN_SUCCESS; }
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { (void)t; return 0; }
static inline kern_return_t vm_region_64(mach_port_t t, vm_address_t *a, vm_size_t *s, int flavor, vm_region_info_t i,
                                         mach_msg_type_number_t *n, mach_port_t *o) {
    (void)t; (void)a; (void)s; (void)flavor; (void)i; (void)n; (void)o;
    return KERN_FAILURE;
}
// a copy that fails instead of faulting when the source is not readable
#ifdef _WIN32
int nxl_read_memory(const void *from, void *to, size_t n, size_t *got);   // ReadProcessMemory (src/compat_win32.c)
static inline kern_return_t vm_read_overwrite(mach_port_t t, vm_address_t a, vm_size_t n, vm_address_t to, vm_size_t *got) {
    (void)t;
    size_t r;
    if (nxl_read_memory((const void *)a, (void *)to, n, &r)) return KERN_FAILURE;
    *got = (vm_size_t)r;
    return KERN_SUCCESS;
}
#else
static inline kern_return_t vm_read_overwrite(mach_port_t t, vm_address_t a, vm_size_t n, vm_address_t to, vm_size_t *got) {
    (void)t;
    struct iovec local = {(void *)to, n}, remote = {(void *)a, n};
    ssize_t r = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    if (r < 0) return KERN_FAILURE;
    *got = (vm_size_t)r;
    return KERN_SUCCESS;
}
#endif
static inline kern_return_t vm_remap(mach_port_t t, vm_address_t *to, vm_size_t n, vm_address_t mask, int flags,
                                     mach_port_t src_task, vm_address_t from, boolean_t copy, vm_prot_t *cur,
                                     vm_prot_t *max, vm_inherit_t inh) {
    (void)t; (void)to; (void)n; (void)mask; (void)flags; (void)src_task; (void)from; (void)copy; (void)cur; (void)max; (void)inh;
    return KERN_FAILURE;
}
// task and thread statistics: not available
typedef struct { uint64_t phys_footprint, resident_size, virtual_size; } task_vm_info_data_t;
typedef struct {
    struct { uint64_t total_user, total_system, task_interrupt_wakeups, task_platform_idle_wakeups, task_timer_wakeups_bin_1, task_timer_wakeups_bin_2; } cpu_energy;
    struct { uint64_t task_gpu_utilisation, task_gpu_stat_reserved0, task_gpu_stat_reserved1, task_gpu_stat_reserved2; } gpu_energy;
    uint64_t task_energy, task_ptime, task_pset_switches;
} task_power_info_v2_data_t;
#define TASK_VM_INFO 22
#define TASK_VM_INFO_COUNT ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(natural_t)))
#define TASK_POWER_INFO_V2 26
#define TASK_POWER_INFO_V2_COUNT ((mach_msg_type_number_t)(sizeof(task_power_info_v2_data_t) / sizeof(natural_t)))
static inline kern_return_t task_info(mach_port_t t, int flavor, task_info_t info, mach_msg_type_number_t *n) {
    (void)t; (void)flavor; (void)info; (void)n;
    return KERN_FAILURE;
}
typedef struct { struct { int seconds, microseconds; } user_time, system_time; int cpu_usage, policy, run_state, flags, suspend_count, sleep_time; } thread_basic_info_data_t;
typedef struct { uint64_t pth_user_time, pth_system_time; int pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time, pth_curpri, pth_priority, pth_maxpriority; char pth_name[64]; } thread_extended_info_data_t;
#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(natural_t)))
#define THREAD_EXTENDED_INFO 5
#define THREAD_EXTENDED_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_extended_info_data_t) / sizeof(natural_t)))
static inline kern_return_t thread_info(mach_port_t t, int flavor, thread_info_t info, mach_msg_type_number_t *n) {
    (void)t; (void)flavor; (void)info; (void)n;
    return KERN_FAILURE;
}
#include <mach/thread_policy.h>
