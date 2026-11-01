// SPDX-License-Identifier: GPL-3.0-or-later
// Where the time goes, for the -v status lines (cpu.c prints them every 10 s): counters and nanoseconds, added to by the
// graphics backend and the CPU side, read as differences between two status lines.
#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

enum {
    NXL_PERF_TRANSLATE,     // shader translations (yuzu's recompiler)
    NXL_PERF_PIPELINE,      // Vulkan pipelines made
    NXL_PERF_SUBMIT,        // submissions: the GPU's work, waited for
    NXL_PERF_UPLOAD,        // texture uploads (deswizzle, copy), bytes in NXL_PERF_UPLOAD_BYTES
    NXL_PERF_ASTC,          // ASTC textures decoded on the CPU (part of the uploads)
    NXL_PERF_PRESENT,       // frames shown in the window (acquire, blit, present)
    NXL_PERF_JIT,           // guest code the ahead-of-time pass missed, compiled now
    NXL_PERF_REPLAY,        // NVN commands turned into Vulkan ones (a submission's recording, its GPU wait excluded)
    NXL_PERF_SEGMENT,       // segments handed to the GPU before their submission's end (count only)
    NXL_PERF_D_SHADER,      // a draw's recording, in parts (part of NXL_PERF_REPLAY): finding its translation
    NXL_PERF_D_DESC,        // ... its descriptors (textures looked up and uploaded, buffers, the set)
    NXL_PERF_D_PIPE,        // ... its pipeline looked up, and the render pass begun
    NXL_PERF_D_CMD,         // ... its Vulkan commands (binds, dynamic state, the draw)
    NXL_PERF_N
};
extern _Atomic uint64_t nxl_perf_n[NXL_PERF_N], nxl_perf_ns[NXL_PERF_N], nxl_perf_upload_bytes;
static inline uint64_t nxl_perf_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline void nxl_perf_add(int k, uint64_t t0) {
    atomic_fetch_add_explicit(&nxl_perf_n[k], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&nxl_perf_ns[k], nxl_perf_now() - t0, memory_order_relaxed);
}
