// SPDX-License-Identifier: GPL-3.0-or-later
// Mach thread scheduling policies (the real-time time-constraint policy nxnative asks for on the Mac): not available
// on Linux; nxnative carries on without it
#pragma once
#include <mach/mach.h>
typedef struct { uint32_t period, computation, constraint; int preemptible; } thread_time_constraint_policy_data_t;
#define THREAD_TIME_CONSTRAINT_POLICY 2
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT ((mach_msg_type_number_t)(sizeof(thread_time_constraint_policy_data_t) / sizeof(int)))
static inline kern_return_t thread_policy_set(mach_port_t t, int flavor, thread_policy_t p, mach_msg_type_number_t n) {
    (void)t; (void)flavor; (void)p; (void)n;
    return KERN_FAILURE;
}
static inline kern_return_t thread_policy_get(mach_port_t t, int flavor, thread_policy_t p, mach_msg_type_number_t *n, boolean_t *def) {
    (void)t; (void)flavor; (void)p; (void)n; (void)def;
    return KERN_FAILURE;
}
