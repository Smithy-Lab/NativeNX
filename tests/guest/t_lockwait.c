// SPDX-License-Identifier: GPL-3.0-or-later
// A thread waiting 12 s in ArbitrateLock for a lock another thread holds: the -v status line must name the holder.
#include "nx.h"
static u32 lock_word;
static u8 holder_stack[0x10000] __attribute__((aligned(16)));
static void holder(void *arg) {
    (void)arg;
    svcSleepThread(12000000000ll);
    svcArbitrateUnlock(&lock_word);   // hands the lock to the waiter
    svcExitThread();
}
int test_main(void) {
    Handle h;
    if (svcCreateThread(&h, (void *)holder, 0, holder_stack + sizeof holder_stack, 44, 0)) return 1;
    lock_word = h | 0x40000000;   // held by the holder, with waiters
    svcStartThread(h);
    svcArbitrateLock(h, &lock_word, 0x1234);
    puts_(lock_word == 0x1234 ? "ok - the lock was handed over\n" : "not ok - the lock was handed over\n");
    return 0;
}
