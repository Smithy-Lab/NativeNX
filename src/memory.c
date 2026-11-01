// SPDX-License-Identifier: GPL-3.0-or-later
// The guest address space: Horizon's 39-bit layout, reserved at its own addresses (guest address == host address).
#include "nxl.h"
#include "host.h"
#include <stdatomic.h>

// Horizon starts the ASLR region at 128 MiB, but games find their regions through svcGetInfo and their code is
// position-independent, so nxlinux places everything from 32 GiB up: low addresses are where a non-PIE host
// executable and its brk heap live (the heap grows from just after the executable, at a randomized start).
region_t g_aslr  = {0x0800000000ull, 0x0078000000ull};   // modules: about 2 GiB from 32 GiB
region_t g_alias = {0x1000000000ull, 0x1000000000ull};   // 64 GiB alias region (MapPhysicalMemory, the SDK's VAMM)
region_t g_heap  = {0x2000000000ull, 0x0180000000ull};   // 6 GiB heap region (svcSetHeapSize)
region_t g_stack = {0x2200000000ull, 0x0080000000ull};   // 2 GiB stack region (svcMapMemory of thread stacks)
static const uint64_t ADDR_SPACE_END = 1ull << 39;

#define PAGE 0x1000ull
static pthread_mutex_t g_mem_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_aslr_next, g_stack_next, g_heap_size;

// what QueryMemory reports: the mapped blocks, kept sorted by address
typedef struct { uint64_t addr, size; uint32_t type, perm; } block_t;
static block_t g_blocks[4096];
static int g_nblocks;
enum { MT_UNMAPPED = 0, MT_CODE_STATIC = 3, MT_CODE_MUTABLE = 4, MT_HEAP = 5, MT_MAPPED = 0xB, MT_THREAD_LOCAL = 0xC };

static void block_remove_range(uint64_t a, uint64_t s) {   // caller holds g_mem_mu
    for (int i = 0; i < g_nblocks; i++) {
        block_t *b = &g_blocks[i];
        uint64_t be = b->addr + b->size, e = a + s;
        if (be <= a || b->addr >= e) continue;
        if (b->addr < a && be > e) {   // split in two
            memmove(&g_blocks[i + 2], &g_blocks[i + 1], (g_nblocks - i - 1) * sizeof *b);
            g_blocks[i + 1] = (block_t){e, be - e, b->type, b->perm};
            b->size = a - b->addr;
            g_nblocks++;
            return;
        }
        if (b->addr < a) { b->size = a - b->addr; continue; }
        if (be > e) { b->size = be - e; b->addr = e; continue; }
        memmove(b, b + 1, (g_nblocks - i - 1) * sizeof *b);
        g_nblocks--, i--;
    }
}
static void block_add(uint64_t a, uint64_t s, uint32_t type, uint32_t perm) {   // caller holds g_mem_mu
    block_remove_range(a, s);
    if (g_nblocks == (int)(sizeof g_blocks / sizeof *g_blocks)) die("too many memory blocks");
    int i = 0;
    while (i < g_nblocks && g_blocks[i].addr < a) i++;
    memmove(&g_blocks[i + 1], &g_blocks[i], (g_nblocks - i) * sizeof *g_blocks);
    g_blocks[i] = (block_t){a, s, type, perm};
    g_nblocks++;
}

// Reserves a region of the guest's address space at its fixed address (stops the run when something else is there).
static void reserve(const char *what, region_t r) {
    if (host_vm_reserve(r.base, r.size))
        die("cannot reserve the guest's %s region at %#llx (%#llx bytes): %s. Something else is mapped there.", what,
            (unsigned long long)r.base, (unsigned long long)r.size, host_vm_error());
}

// Reserves the guest's regions (code, heap, alias, stack, map) at the addresses Horizon gives them, sized by the game's
// profile.
void mem_init(void) {
    // the alias and stack regions take the sizes the program's profile gives (the SDK sizes its virtual address memory
    // bookkeeping from them), within the reservations above
    if (g_profile.mem_alias && g_profile.mem_alias <= g_alias.size) g_alias.size = g_profile.mem_alias;
    if (g_profile.mem_stack && g_profile.mem_stack <= g_stack.size) g_stack.size = g_profile.mem_stack;
    reserve("ASLR", g_aslr);
    reserve("alias", g_alias);
    reserve("heap", g_heap);
    reserve("stack", g_stack);
    reserve("HLE stub", (region_t){NXL_HLE_BASE, NXL_HLE_SIZE});
    reserve("host-callable stub", (region_t){NXL_TRAMP_BASE, NXL_TRAMP_SIZE});
    g_aslr_next = g_aslr.base;
    g_stack_next = g_stack.base;
}

static void map_rw(uint64_t a, uint64_t s) {   // fresh zeroed pages in a reservation
    if (host_vm_commit(a, s)) die("cannot map %#llx+%#llx: %s", (unsigned long long)a, (unsigned long long)s, host_vm_error());
}
static void unmap_to_reserved(uint64_t a, uint64_t s) { host_vm_decommit(a, s); }   // back to an inaccessible reservation

// Room for a module in the guest's code region (ASLR region); its address.
uint64_t mem_map_module(uint64_t size) {
    pthread_mutex_lock(&g_mem_mu);
    size = (size + PAGE - 1) & ~(PAGE - 1);
    uint64_t a = g_aslr_next;
    if (a + size > g_aslr.base + g_aslr.size) die("ASLR region full");
    g_aslr_next = (a + size + 0x200000 - 1) & ~0x1FFFFFull;   // modules start on 2 MiB boundaries, as Horizon's loader places them
    map_rw(a, size);
    block_add(a, size, MT_CODE_MUTABLE, 3);
    pthread_mutex_unlock(&g_mem_mu);
    return a;
}

// called by the loader once a module is relocated: text and ro become read-only (the guest's code is never executed in
// place, only read: the recompiled copy runs, so no part of the guest image is ever executable on the host)
void mem_protect(uint64_t a, uint64_t s, int writable, uint32_t type, uint32_t perm);
// Sets a range writable or read-only and notes its memory type and permission (for QueryMemory).
void mem_protect(uint64_t a, uint64_t s, int writable, uint32_t type, uint32_t perm) {
    if (!s) return;
    if (host_vm_protect(a, s, writable ? HOST_VM_RW : HOST_VM_R)) die("cannot change the protection of %#llx: %s", (unsigned long long)a, host_vm_error());
    pthread_mutex_lock(&g_mem_mu);
    block_add(a, s, type, perm);
    pthread_mutex_unlock(&g_mem_mu);
}

// SVC SetHeapSize: grows or shrinks the heap (2 MiB steps); its address.
Result mem_set_heap(uint64_t size, uint64_t *out) {
    if (size & 0x1FFFFF) return KERR_INVALID_SIZE;
    if (size > g_heap.size) return KERR_OUT_OF_MEMORY;
    pthread_mutex_lock(&g_mem_mu);
    if (size > g_heap_size) {
        if (host_vm_commit(g_heap.base + g_heap_size, size - g_heap_size)) {   // (zeroed: it was given back when it shrank)
            pthread_mutex_unlock(&g_mem_mu);
            return KERR_OUT_OF_MEMORY;
        }
    } else if (size < g_heap_size) {
        unmap_to_reserved(g_heap.base + size, g_heap_size - size);   // the freed part reads as zero when it grows again
        block_remove_range(g_heap.base + size, g_heap_size - size);
    }
    g_heap_size = size;
    if (size) block_add(g_heap.base, size, MT_HEAP, 3);
    pthread_mutex_unlock(&g_mem_mu);
    *out = g_heap.base;
    return RES_OK;
}
uint64_t mem_heap_used(void) { return g_heap_size; }

// A guest stack of a size, with a guard page below it; its top.
uint64_t mem_stack_alloc(uint64_t size) {
    pthread_mutex_lock(&g_mem_mu);
    size = (size + PAGE - 1) & ~(PAGE - 1);
    uint64_t a = g_stack_next + PAGE;   // a guard page below each stack
    if (a + size > g_stack.base + g_stack.size) die("stack region full");
    g_stack_next = a + size;
    map_rw(a, size);
    block_add(a, size, MT_MAPPED, 3);
    pthread_mutex_unlock(&g_mem_mu);
    return a + size;
}

static int in_region(region_t r, uint64_t a, uint64_t s) { return a >= r.base && s <= r.size && a - r.base <= r.size - s; }

// MapMemory's destination: the stack region; the alias region; or the 1.0 kernel's map region below 2 GiB. The launch
// year's SDKs (Breath of the Wild 1.6.0's nnSdk 0.12.14) ask no kernel where to put a thread's stack alias: their
// nn::os AddressSpaceAllocator picks random addresses from 128 MiB to 2 GiB, as the 1.0 kernel's 36-bit layout had free,
// checks them with QueryMemory and maps there. nxlinux's own regions are all above 32 GiB, so that range belongs to the
// host process: a page the host holds is "in use" (InvalidCurrentMemoryState), and the SDK tries another address.
static const region_t g_map_legacy = {0x08000000ull, 0x78000000ull};
static int map_dest_ok(uint64_t dst, uint64_t size) {
    return in_region(g_stack, dst, size) || in_region(g_alias, dst, size) || in_region(g_map_legacy, dst, size);
}
// SVC MapMemory: maps memory at src to dst (the stack region or the old map region), src made inaccessible.
Result mem_map(uint64_t dst, uint64_t src, uint64_t size) {
    if ((dst | src | size) & (PAGE - 1) || !size) return KERR_INVALID_SIZE;
    if (!map_dest_ok(dst, size)) return KERR_INVALID_ADDR;
    pthread_mutex_lock(&g_mem_mu);
    if (in_region(g_map_legacy, dst, size)) {
        if (host_vm_map_new(dst, size)) {   // (something of the host's is there: the SDK tries elsewhere)
            pthread_mutex_unlock(&g_mem_mu);
            return KERR_INVALID_MEM_STATE;
        }
    } else map_rw(dst, size);
    memcpy((void *)dst, (void *)src, size);
    host_vm_protect(src, size, HOST_VM_NONE);   // the source is inaccessible while mapped, as on Horizon
    block_add(dst, size, MT_MAPPED, 3);
    pthread_mutex_unlock(&g_mem_mu);
    return RES_OK;
}
// SVC UnmapMemory: undoes a MapMemory.
Result mem_unmap(uint64_t dst, uint64_t src, uint64_t size) {
    if ((dst | src | size) & (PAGE - 1) || !size) return KERR_INVALID_SIZE;
    if (!map_dest_ok(dst, size)) return KERR_INVALID_ADDR;
    pthread_mutex_lock(&g_mem_mu);
    host_vm_protect(src, size, HOST_VM_RW);
    memcpy((void *)src, (void *)dst, size);
    if (in_region(g_map_legacy, dst, size)) host_vm_release(dst, size);   // back to the host
    else unmap_to_reserved(dst, size);
    block_remove_range(dst, size);
    pthread_mutex_unlock(&g_mem_mu);
    return RES_OK;
}

// SVC QueryMemory: the block of memory an address is in, its type and permission.
void mem_query(uint64_t addr, MemoryInfo *out) {
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&g_mem_mu);
    uint64_t lo = 0, hi = ADDR_SPACE_END;
    for (int i = 0; i < g_nblocks; i++) {
        block_t *b = &g_blocks[i];
        if (addr >= b->addr && addr - b->addr < b->size) {
            out->addr = b->addr, out->size = b->size, out->type = b->type, out->perm = b->perm;
            pthread_mutex_unlock(&g_mem_mu);
            return;
        }
        if (b->addr + b->size <= addr) lo = b->addr + b->size;
        else if (b->addr > addr) { hi = b->addr; break; }
    }
    pthread_mutex_unlock(&g_mem_mu);
    if (addr >= ADDR_SPACE_END) lo = ADDR_SPACE_END, hi = 0;   // the inaccessible rest of the 64-bit space
    out->addr = lo, out->size = hi - lo, out->type = MT_UNMAPPED, out->perm = 0;
}

// svcMapPhysicalMemory / svcUnmapPhysicalMemory: commit or release pages of the alias region (the SDK's virtual address
// memory manager). Released pages read as zero when committed again, as on Horizon
static _Atomic uint64_t g_alias_mapped;
static uint8_t *g_alias_bits;   // one bit per 4 KiB page of the alias region: mapped
static int alias_on(uint64_t a) { uint64_t p = (a - g_alias.base) / PAGE; return g_alias_bits[p >> 3] >> (p & 7) & 1; }
static int64_t alias_mark(uint64_t a, uint64_t s, int on) {   // pages whose state changed; caller holds g_mem_mu
    if (!g_alias_bits) g_alias_bits = calloc(g_alias.size / PAGE / 8, 1);
    int64_t n = 0;
    for (uint64_t p = (a - g_alias.base) / PAGE, e = p + s / PAGE; p < e; p++) {
        uint8_t m = (uint8_t)(1u << (p & 7)), was = g_alias_bits[p >> 3] & m;
        if (on && !was) g_alias_bits[p >> 3] |= m, n++;
        else if (!on && was) g_alias_bits[p >> 3] &= (uint8_t)~m, n++;
    }
    return n;
}
// SVC MapPhysicalMemory / UnmapPhysicalMemory: backs (or frees) a range of the alias region.
Result mem_map_physical(uint64_t a, uint64_t s, int map) {
    if (!s || ((a | s) & (PAGE - 1))) return KERR_INVALID_SIZE;
    if (!in_region(g_alias, a, s)) return 0xD401;   // invalid memory region
    pthread_mutex_lock(&g_mem_mu);
    if (map) {   // the pages not mapped yet are committed (zeroed); the mapped ones keep what they hold
        if (!g_alias_bits) g_alias_bits = calloc(g_alias.size / PAGE / 8, 1);
        for (uint64_t p = a; p < a + s;) {
            if (alias_on(p)) { p += PAGE; continue; }
            uint64_t q = p;
            while (q < a + s && !alias_on(q)) q += PAGE;
            if (host_vm_commit(p, q - p)) { pthread_mutex_unlock(&g_mem_mu); return KERR_OUT_OF_MEMORY; }
            p = q;
        }
    }
    int64_t n = alias_mark(a, s, map);
    if (map) {
        block_add(a, s, MT_HEAP, 3);   // what QueryMemory reports for physical memory mapped in the alias region
    } else {
        unmap_to_reserved(a, s);
        block_remove_range(a, s);
    }
    atomic_fetch_add(&g_alias_mapped, (map ? n : -n) * (int64_t)PAGE);
    pthread_mutex_unlock(&g_mem_mu);
    return RES_OK;
}
uint64_t mem_alias_mapped(void) { return atomic_load(&g_alias_mapped); }
