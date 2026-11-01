// SPDX-License-Identifier: GPL-3.0-or-later
// Guest threads on host threads: a contended mutex, a producer/consumer queue on condition variables, timeouts, joins.
#include "nx.h"

unsigned long nxltest_tid(void);

#define NT 4
#define ITER 20000
static Thread th[NT];
static Mutex mu;
static long shared_counter;
static u64 ids[NT];

static void worker(void *arg) {
    long k = (long)arg;
    ids[k] = nxltest_tid();
    for (int i = 0; i < ITER; i++) {
        mutexLock(&mu);
        long v = shared_counter;   // a read-modify-write that loses updates unless the mutex works
        if ((i & 255) == 0) svcSleepThread(0);
        shared_counter = v + 1;
        mutexUnlock(&mu);
    }
}

// a bounded queue
#define QN 8
static Mutex qmu;
static CondVar not_empty, not_full;
static long q[QN];
static int qhead, qcount;
static long consumed_sum;
#define ITEMS 5000
static void producer(void *arg) {
    (void)arg;
    for (long i = 1; i <= ITEMS; i++) {
        mutexLock(&qmu);
        while (qcount == QN) condvarWait(&not_full, &qmu);
        q[(qhead + qcount++) % QN] = i;
        condvarWakeOne(&not_empty);
        mutexUnlock(&qmu);
    }
}
static void consumer(void *arg) {
    (void)arg;
    for (int i = 0; i < ITEMS / 2; i++) {
        mutexLock(&qmu);
        while (qcount == 0) condvarWait(&not_empty, &qmu);
        long v = q[qhead];
        qhead = (qhead + 1) % QN, qcount--;
        consumed_sum += v;
        condvarWakeOne(&not_full);
        mutexUnlock(&qmu);
    }
}

static Thread late;
static volatile int late_ran;
static void late_fn(void *a) { (void)a; svcSleepThread(20000000); late_ran = 1; }

// address arbitration (svcWaitForAddress / svcSignalToAddress)
static volatile int arb_word = 7, arb_woke, arb2_word = 10, arb2_woke;
static void arb_waiter(void *a) { (void)a; if (svcWaitForAddress((void *)&arb_word, 2, 7, -1) == 0) arb_woke = 1; }
static void arb2_waiter(void *a) { (void)a; if (svcWaitForAddress((void *)&arb2_word, 2, 10, -1) == 0) __atomic_fetch_add(&arb2_woke, 1, __ATOMIC_SEQ_CST); }
static volatile int dec_word = 0, dec_woke;
static void dec_waiter(void *a) { (void)a; if (svcWaitForAddress((void *)&dec_word, 1, 1, -1) == 0) dec_woke = 1; }

// A condition variable signalled as the SDK does (nn::os's InternalConditionVariable): SignalProcessWideKey only when
// the key word says someone waits. The kernel must set that word before it releases the waiter's mutex: set after, a
// thread taking the mutex in between (in user mode, without the kernel) changes the condition, reads 0, skips the
// signal, and the waiter sleeps for good. Two threads hand a turn back and forth; a wait that times out with its
// turn already there is such a lost wake-up
static Mutex pp_mu; static CondVar pp_cv; static volatile int pp_turn; static volatile int pp_lost;
enum { PP_ROUNDS = 20000 };
static void pp_signal(void) { if (__atomic_load_n(&pp_cv, __ATOMIC_ACQUIRE)) svcSignalProcessWideKey(&pp_cv, -1); }
static void pp_run(int me) {
    for (int i = 0; i < PP_ROUNDS; i++) {
        mutexLock(&pp_mu);
        while (pp_turn != me)
            if (condvarWaitTimeout(&pp_cv, &pp_mu, 200000000) == KERR_TIMED_OUT && pp_turn == me) pp_lost++;
        pp_turn = !me;
        mutexUnlock(&pp_mu);
        pp_signal();
    }
}
static void pp_other(void *a) { (void)a; pp_run(1); }
// CancelSynchronization: a thread waiting on an event with no timeout is woken with ResultCancelled
static Handle cs_ev_r; static volatile u32 cs_res = 1, cs_idx = 7;
static void cs_waiter(void *a) { (void)a; int i = 7; cs_res = svcWaitSync(&i, &cs_ev_r, 1, -1); cs_idx = (u32)i; }
int test_main(void) {
    for (long k = 0; k < NT; k++) CHECK("create and start a thread", threadCreate(&th[k], worker, (void *)k) == 0);
    for (int k = 0; k < NT; k++) CHECK("join", threadJoin(&th[k]) == 0);
    CHECK("mutex: no lost updates across 4 threads", shared_counter == (long)NT * ITER);
    int distinct = 1;
    for (int a = 0; a < NT; a++) for (int b = a + 1; b < NT; b++) if (ids[a] == ids[b] || !ids[a]) distinct = 0;
    CHECK("each guest thread has its own id", distinct);

    static Thread p, c1, c2;
    threadCreate(&c1, consumer, NULL);
    threadCreate(&c2, consumer, NULL);
    threadCreate(&p, producer, NULL);
    threadJoin(&p), threadJoin(&c1), threadJoin(&c2);
    CHECK("condition variables: every item consumed once", consumed_sum == (long)ITEMS * (ITEMS + 1) / 2 && qcount == 0);

    mutexLock(&qmu);
    u64 t0 = svcGetSystemTick();
    Result r = condvarWaitTimeout(&not_empty, &qmu, 5000000);   // nobody signals: 5 ms timeout, mutex held again after
    u64 t1 = svcGetSystemTick();
    CHECK("condvar wait times out", r == KERR_TIMED_OUT && t1 - t0 >= 19200 * 4);
    CHECK("the mutex is held after the timeout", (qmu & ~HAS_LISTENERS) == cur_handle());
    mutexUnlock(&qmu);

    // WaitForAddress / SignalToAddress
    CHECK("WaitForAddress: the value is not what the wait expects", svcWaitForAddress((void *)&arb_word, 2, 8, -1) == KERR_INVALID_STATE);
    CHECK("WaitForAddress: a timeout", svcWaitForAddress((void *)&arb_word, 2, 7, 2000000) == KERR_TIMED_OUT);
    static Thread aw, d1, w1, w2;
    threadCreate(&aw, arb_waiter, NULL);
    svcSleepThread(20000000);
    CHECK("a waiter is not woken by itself", !arb_woke);
    CHECK("SignalToAddress wakes it", svcSignalToAddress((void *)&arb_word, 0, 0, 1) == 0 && threadJoin(&aw) == 0 && arb_woke);
    threadCreate(&d1, dec_waiter, NULL);
    svcSleepThread(20000000);
    CHECK("WaitForAddress type 1 decrements before waiting", dec_word == -1 && !dec_woke);
    svcSignalToAddress((void *)&dec_word, 0, 0, -1);
    threadJoin(&d1);
    CHECK("... and is woken", dec_woke);
    volatile int inc = 5;
    CHECK("SignalToAddress type 1 increments if equal", svcSignalToAddress((void *)&inc, 1, 5, 1) == 0 && inc == 6);
    CHECK("... and refuses when not equal", svcSignalToAddress((void *)&inc, 1, 5, 1) == KERR_INVALID_STATE && inc == 6);
    threadCreate(&w1, arb2_waiter, NULL);
    threadCreate(&w2, arb2_waiter, NULL);
    svcSleepThread(30000000);
    CHECK("SignalToAddress type 2, waking all of 2 waiters, subtracts 2 (mesosphere)", svcSignalToAddress((void *)&arb2_word, 2, 10, -1) == 0 && arb2_word == 8);
    threadJoin(&w1), threadJoin(&w2);
    CHECK("... and both wake", arb2_woke == 2);
    volatile int none = 3;
    CHECK("SignalToAddress type 2 with no waiters adds 1", svcSignalToAddress((void *)&none, 2, 3, 1) == 0 && none == 4);

    // cores: an application thread is on cores 0-2, and reports its ideal core (SMBW's per-core font engines)
    int core_ok = 1;
    for (int k = 0; k < 200; k++) { if (svcGetCurrentProcessorNumber() > 2) core_ok = 0; svcSleepThread(0); }
    CHECK("GetCurrentProcessorNumber stays on the application's cores", core_ok && svcGetCurrentProcessorNumber() == 0);
    s32 core; u64 mask;
    CHECK("SetThreadCoreMask moves the ideal core", svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 4) == 0 && svcGetCurrentProcessorNumber() == 2
          && svcGetThreadCoreMask(&core, &mask, CUR_THREAD_HANDLE) == 0 && core == 2 && mask == 4);
    CHECK("an ideal core outside the mask moves into it", svcSetThreadCoreMask(CUR_THREAD_HANDLE, -3, 2) == 0 && svcGetCurrentProcessorNumber() == 1);
    CHECK("-2: the process's default core", svcSetThreadCoreMask(CUR_THREAD_HANDLE, -2, 0) == 0 && svcGetCurrentProcessorNumber() == 0);

    {
        Thread pt;
        threadCreate(&pt, pp_other, NULL);
        pp_run(0);
        threadJoin(&pt);
        print("# condition variable ping-pong: %d lost wake-ups in %d rounds\n", pp_lost, PP_ROUNDS);
        CHECK("a condition variable signalled only when its key says it has waiters loses no wake-up", pp_lost == 0);
    }
    {
        Handle ew; Thread cw;
        svcCreateEvent(&ew, &cs_ev_r);
        threadCreate(&cw, cs_waiter, NULL);
        svcSleepThread(30000000);
        CHECK("CancelSynchronization ends a thread's unbounded WaitSynchronization", svcCancelSynchronization(cw.h) == 0 && (threadJoin(&cw), cs_res == KERR_CANCELLED) && cs_idx == 0xFFFFFFFFu);
        int i;
        CHECK("... and a cancel made before the wait ends the next wait at once", svcCancelSynchronization(CUR_THREAD_HANDLE) == 0 && svcWaitSync(&i, &cs_ev_r, 1, -1) == KERR_CANCELLED);
        CHECK("... once: the wait after that times out normally", svcWaitSync(&i, &cs_ev_r, 1, 1000000) == KERR_TIMED_OUT);
    }
    threadCreate(&late, late_fn, NULL);
    int idx;
    CHECK("wait on a running thread times out", svcWaitSync(&idx, &late.h, 1, 1000000) == KERR_TIMED_OUT && !late_ran);
    CHECK("wait until it exits", svcWaitSync(&idx, &late.h, 1, -1) == 0 && late_ran);
    return 0;
}
