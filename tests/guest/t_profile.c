// SPDX-License-Identifier: GPL-3.0-or-later
// What svcGetInfo reports comes from the title's NPDM and nxnative's game profile: the test prints it, and the runner
// (CMakeLists.txt t_profile) checks it against the main.npdm and game.tsv it wrote.
#include "nx.h"
int test_main(void) {
    u64 v[6] = {0};
    static const u32 ids[6] = {3, 15, 16, 18, 6, 0};
    for (int i = 0; i < 6; i++) svcGetInfo(&v[i], ids[i], 0xFFFF8001, 0);
    print("alias %lx stack %lx sysres %lx program %lx pool %lx coremask %lx\n", v[0], v[1], v[2], v[3], v[4], v[5]);
    u64 aslr = 0, aslr_size = 0, alias = 0, heap = 0, stack = 0;
    svcGetInfo(&aslr, 12, 0xFFFF8001, 0), svcGetInfo(&aslr_size, 13, 0xFFFF8001, 0);
    svcGetInfo(&alias, 2, 0xFFFF8001, 0), svcGetInfo(&heap, 4, 0xFFFF8001, 0), svcGetInfo(&stack, 14, 0xFFFF8001, 0);
    CHECK("the address space holds every region", alias >= aslr && heap >= aslr && stack + v[1] <= aslr + aslr_size);
    u64 app = 0, free_threads = 0;
    CHECK("IsApplication", svcGetInfo(&app, 23, 0xFFFF8001, 0) == 0 && app == 1);
    CHECK("FreeThreadCount", svcGetInfo(&free_threads, 24, 0xFFFF8001, 0) == 0 && free_threads == 95);
    u64 exc = 0;
    CHECK("a user exception context area", svcGetInfo(&exc, 20, 0xFFFF8001, 0) == 0 && exc && ((volatile u8 *)exc)[0x1c0] == 0);
    return 0;
}
