// SPDX-License-Identifier: GPL-3.0-or-later
// Loading, relocation, host calls with every argument-passing case, callbacks, the C library, heap, memory queries, TLS.
#include "nx.h"

// host functions (src/hle_selftest.c, src/hle_libc.c): imports the loader binds to host stubs
struct pair { long a, b; };
struct big { long v[4]; };
long nxltest_sum9(long, long, long, long, long, long, long, long, long);
double nxltest_mix(int, double, float, char, long, double);
float nxltest_fsum10(float, float, float, float, float, float, float, float, float, float);
unsigned char nxltest_u8(unsigned);
int nxltest_neg(int);
struct pair nxltest_pair(long, long);
long nxltest_pairsum(long, struct pair, long);
struct big nxltest_big(long);
long nxltest_bigsum(struct big, long);
char *nxltest_strdup_upper(char *, const char *);
long nxltest_twice(long (*)(long), long);
long nxltest_native_call1(const char *name, long v);
int nxltest_host_windows(void);
unsigned long nxltest_tid(void);
size_t strlen(const char *);
int strcmp(const char *, const char *);
long strtol(const char *, char **, int);
float sinf(float);
double pow(double, double);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
void *memset(void *, int, size_t);
double nxltest_native_call(const char *);
long nxltest_native_pair(const char *);
long (*nxltest_get_callable(const char *))(long);
long (*nxltest_host_fnptr(void))(long, long, long);
__attribute__((visibility("default"), noinline)) double guest_mix(long a, double b, int c, float d) { return a * b + c + d; }
__attribute__((visibility("default"), noinline)) struct pair guest_pair(long a, long b) { return (struct pair){a + 1, b + 2}; }
__attribute__((visibility("default"), noinline)) long guest_inc(long v) { return v + 1; }
// sets the thread pointer, as the SDK's start-up does in calls the host makes (sdk_boot)
__attribute__((visibility("default"), noinline)) long guest_set_tp(long a) {
    __asm__ volatile("msr tpidr_el0, %0" :: "r"((u64)a << 12));
    return a;
}

// defined here, but the host forces its own (HLE_FORCE): calls must reach the host's, which returns 7
__attribute__((noinline, visibility("default"))) int nxltest_forced(void) { return 1; }

// relocated data: pointers to functions and strings in a table (R_AARCH64_RELATIVE), zeroed bss
static long add1(long v) { return v + 1; }
static long sub1(long v) { return v - 1; }
static long (*const ops[])(long) = {add1, sub1};
static const char *const names[] = {"alpha", "beta", "gamma"};
static long counter = 5;
static long zeroed[64];

static long square_plus_host(long v) { return v * v + nxltest_neg(-1); }   // guest code calling the host from a callback
static int cmp_long(const void *a, const void *b) { long x = *(const long *)a, y = *(const long *)b; return (x > y) - (x < y); }

static int ctor_ran;
__attribute__((constructor)) static void ctor(void) { ctor_ran = 1; }   // not run: test modules have no SDK boot

int test_main(void) {
    // relocations and data
    CHECK("function pointer table", ops[0](10) == 11 && ops[1](10) == 9);
    CHECK("string pointer table", strcmp(names[2], "gamma") == 0 && strlen(names[0]) == 5);
    CHECK("initialized data", counter == 5);
    int z = 1;
    for (int i = 0; i < 64; i++) if (zeroed[i]) z = 0;
    CHECK("bss is zero", z);
    (void)ctor_ran;

    // host calls: each AAPCS64 case
    CHECK("9 integer arguments (one on the stack)", nxltest_sum9(1, 2, 3, 4, 5, 6, 7, 8, 9) == 36 + 9000);
    double m = nxltest_mix(1, 2.5, 0.25f, 3, 4, 0.125);
    CHECK("mixed integer and floating-point arguments", m == 10.875);
    CHECK("10 float arguments (two on the stack)", nxltest_fsum10(1, 1, 1, 1, 1, 1, 1, 1, 2, 3) == 8 + 200 + 3000);
    CHECK("narrow return (unsigned char wraps)", nxltest_u8(255) == 0);
    CHECK("negative int return", nxltest_neg(5) == -5);
    struct pair p = nxltest_pair(4, 5);
    CHECK("16-byte struct return (x0, x1)", p.a == 8 && p.b == 15);
    CHECK("16-byte struct argument between integers", nxltest_pairsum(1, (struct pair){2, 3}, 4) == 1 + 20 + 300 + 4000);
    struct big b = nxltest_big(10);
    CHECK("large struct return (through x8)", b.v[0] == 10 && b.v[3] == 13);
    CHECK("large struct argument (by reference)", nxltest_bigsum((struct big){{1, 2, 3, 4}}, 3) == 30);
    char buf[16];
    CHECK("host writes guest memory", strcmp(nxltest_strdup_upper(buf, "switch"), "SWITCH") == 0);
    CHECK("HLE_FORCE wins over the module's own definition", nxltest_forced() == 7);
    CHECK("guest -> host -> guest -> host", nxltest_twice(square_plus_host, 3) == (9 + 1) + (16 + 1));
    if (nxltest_host_windows()) {   // (src/tramp_win64.S: Windows passes the first four arguments by position)
        SKIP("host calls a guest function through a C pointer (int and float arguments)", "on Windows: mixed integer and floating-point arguments need GUEST_CALL");
        SKIP("host calls a guest function returning a 16-byte struct", "on Windows: such a result comes back through a pointer, which needs GUEST_CALL");
    } else {
        CHECK("host calls a guest function through a C pointer (int and float arguments)", nxltest_native_call("guest_mix") == 5.75);
        CHECK("host calls a guest function returning a 16-byte struct", nxltest_native_pair("guest_pair") == 7 * 100 + 9);
    }
    CHECK("guest calls a host-callable stub address", nxltest_get_callable("guest_inc")(41) == 42);
    CHECK("guest calls a host function pointer the host handed it", nxltest_host_fnptr()(1, 2, 3) == 123);

    // the C library served by the host
    char *end;
    CHECK("strtol with an end pointer", strtol("  -1234xyz", &end, 10) == -1234 && *end == 'x');
    float s = sinf(1.5707964f);
    CHECK("sinf", s > 0.9999f && s <= 1.0f);
    CHECK("pow", pow(2.0, 10.0) == 1024.0);
    long arr[8] = {5, 3, 8, 1, 9, 2, 7, 4};
    qsort(arr, 8, sizeof *arr, cmp_long);   // the host sorts, calling the guest's comparator
    int sorted = 1;
    for (int i = 1; i < 8; i++) if (arr[i - 1] > arr[i]) sorted = 0;
    CHECK("qsort with a guest comparator", sorted && arr[0] == 1 && arr[7] == 9);

    // heap
    u64 heap = 0;
    CHECK("SetHeapSize", svcSetHeapSize(&heap, 0x400000) == 0 && heap);
    u64 *h = (u64 *)heap;
    for (int i = 0; i < 0x80000; i++) h[i] = i * 3;
    u64 sum = 0;
    for (int i = 0; i < 0x80000; i++) sum += h[i];
    CHECK("heap is writable", sum == 3ul * 0x80000 * (0x80000 - 1) / 2);
    u64 region = 0;
    svcGetInfo(&region, 4, 0xFFFF8001, 0);
    CHECK("GetInfo heap region", region == heap);
    MemoryInfo mi;
    CHECK("QueryMemory of the heap", svcQueryMemory(&mi, heap + 0x1000) == 0 && mi.addr == heap && mi.size == 0x400000 && mi.type == 5);
    CHECK("QueryMemory of our code", svcQueryMemory(&mi, (u64)test_main) == 0 && mi.perm == 5);
    CHECK("shrink the heap", svcSetHeapSize(&heap, 0x200000) == 0 && svcQueryMemory(&mi, heap) == 0 && mi.size == 0x200000);

    // MapMemory: the SDK moves a thread stack from the heap into the stack region this way
    u64 stack_region = 0;
    svcGetInfo(&stack_region, 14, 0xFFFF8001, 0);
    u64 dst = stack_region + 0x40000000;
    h[0] = 0xfeedface;
    CHECK("MapMemory", svcMapMemory(dst, heap, 0x10000) == 0 && *(u64 *)dst == 0xfeedface);
    *(u64 *)dst = 0xabad1dea;
    CHECK("UnmapMemory moves the contents back", svcUnmapMemory(dst, heap, 0x10000) == 0 && h[0] == 0xabad1dea);
    u64 extra = 99;
    CHECK("GetInfo 28 (AliasRegionExtraSize, SDK 20 asks): success, 0", svcGetInfo(&extra, 28, 0xFFFF8001, 0) == 0 && extra == 0);
    // into the alias region, as the 1.0 kernel's SDKs map thread stacks (it had no stack region)
    u64 alias_r = 0;
    svcGetInfo(&alias_r, 2, 0xFFFF8001, 0);
    u64 adst = alias_r + 0x800000;
    MemoryInfo ami = {0};
    svcQueryMemory(&ami, adst);
    CHECK("the alias region is free before", ami.type == 0);
    h[0] = 0x51ab51ab;
    CHECK("MapMemory into the alias region (launch-year SDKs' thread stacks)", svcMapMemory(adst, heap, 0x10000) == 0 && *(u64 *)adst == 0x51ab51ab);
    svcQueryMemory(&ami, adst);
    CHECK("... QueryMemory sees it mapped", ami.type != 0 && ami.addr <= adst && ami.addr + ami.size >= adst + 0x10000);
    *(u64 *)adst = 0x0ddba11;
    CHECK("... and UnmapMemory moves the contents back", svcUnmapMemory(adst, heap, 0x10000) == 0 && h[0] == 0x0ddba11);
    // below 2 GiB, where the 1.0 kernel's SDKs map thread stacks without asking where (nn::os AddressSpaceAllocator)
    int mapped_low = 0;
    for (u64 lo = 0x10000000; lo < 0x78000000 && !mapped_low; lo += 0x7000000) {
        h[0] = 0x10e10e;
        Result r = svcMapMemory(lo, heap, 0x10000);
        if (r == 0) {
            mapped_low = *(u64 *)lo == 0x10e10e;
            *(u64 *)lo = 0x10e10f;
            mapped_low &= svcUnmapMemory(lo, heap, 0x10000) == 0 && h[0] == 0x10e10f;
        }
    }
    CHECK("MapMemory below 2 GiB (the 1.0 kernel's map region) maps and moves back", mapped_low);
    CHECK("... and the region's end is still refused", svcMapMemory(0x7FFF0000, heap, 0x20000) == 0xCC01);

    // MapPhysicalMemory: memory committed inside the alias region (the SDK's virtual address memory manager)
    u64 alias = 0, used0 = 0, used1 = 0;
    svcGetInfo(&alias, 2, 0xFFFF8001, 0);
    u64 va = alias + 0x100000;
    svcGetInfo(&used0, 7, 0xFFFF8001, 0);
    CHECK("MapPhysicalMemory", svcMapPhysicalMemory(va, 0x10000) == 0);
    svcGetInfo(&used1, 7, 0xFFFF8001, 0);
    CHECK("... is not counted as used memory without the profile's mem_map_limit (nxnative's rule)", used1 == used0);
    ((u64 *)va)[100] = 0x1234;
    CHECK("... is writable, and QueryMemory sees it", ((u64 *)va)[100] == 0x1234 && svcQueryMemory(&mi, va + 0x800) == 0 && mi.addr == va && mi.size == 0x10000);
    CHECK("UnmapPhysicalMemory", svcUnmapPhysicalMemory(va, 0x10000) == 0);
    CHECK("mapped again it reads zero", svcMapPhysicalMemory(va, 0x10000) == 0 && ((u64 *)va)[100] == 0);
    svcUnmapPhysicalMemory(va, 0x10000);
    CHECK("outside the alias region it is refused", svcMapPhysicalMemory(heap, 0x10000) != 0);

    // TLS, ticks, ids, events
    *(u64 *)(tls() + 0x100) = 0x1234;
    CHECK("TLS holds what was written", *(u64 *)(tls() + 0x100) == 0x1234);
    u64 t0 = svcGetSystemTick();
    svcSleepThread(2000000);
    u64 t1 = svcGetSystemTick();
    CHECK("system tick advances at 19.2 MHz", t1 - t0 >= 19200 * 2 && t1 - t0 < 19200 * 200);
    u64 tid = 0;
    CHECK("GetThreadId", svcGetThreadId(&tid, 0xFFFF8000) == 0 && tid == nxltest_tid());
    Handle w, r;
    int idx = -1;
    CHECK("CreateEvent", svcCreateEvent(&w, &r) == 0);
    CHECK("wait on an unsignaled event times out", svcWaitSync(&idx, &r, 1, 1000000) == KERR_TIMED_OUT);
    svcSignalEvent(w);
    CHECK("wait on a signaled event", svcWaitSync(&idx, &r, 1, -1) == 0 && idx == 0);
    svcClearEvent(w);
    CHECK("cleared event times out again", svcWaitSync(&idx, &r, 1, 0) == KERR_TIMED_OUT);
    CHECK("CloseHandle", svcCloseHandle(w) == 0 && svcCloseHandle(r) == 0);

    // the ELF TLS thread pointer: set, zeroed, writable at a static offset (as Super Mario Bros. Wonder's main uses it)
    u64 tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    CHECK("tpidr_el0 points at a TLS block", tp != 0 && ((u64 *)tp)[0x68 / 8] == 0);
    ((volatile u64 *)tp)[0x68 / 8] = 0x1234;
    CHECK("... which the thread can write", ((volatile u64 *)tp)[0x68 / 8] == 0x1234);
    u64 mine = 0x5000;
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(mine));
    u64 back;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(back));
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(tp));
    CHECK("the guest can set its own thread pointer", back == mine);
    nxltest_native_call1("guest_set_tp", 6);   // sets it to 6 << 12 inside a call from the host
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(back));
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(tp));
    CHECK("a thread pointer set in a call from the host stays set", back == (6 << 12));
    u64 tlr;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tlr));
    u64 obj = *(u64 *)(tlr + 0x1f8);
    CHECK("TLR+0x1f8 points at a zeroed current-thread object", obj && obj != tlr && ((u64 *)obj)[0] == 0 && ((u64 *)obj)[0x3f8 / 8] == 0);
    return 0;
}
