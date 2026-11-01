// SPDX-License-Identifier: GPL-3.0-or-later
// The rest of the replay interface hle_nvn.c calls: frame timing and statistics nxnative's Metal backend keeps, and a
// few host pieces from nxnative's Mac front end. Statistics come from the Vulkan backend where it has them.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

uint64_t vk_stat_subs(void);
void vk_draw_stats(uint64_t *passes, uint64_t *draws, uint64_t *pipes, uint64_t *translations, uint64_t *fails);

void mtl_ub_present(double iv_ms) { (void)iv_ms; }
void mtl_set_frame(uint64_t f) { (void)f; }
double mtl_gpu_ms(uint64_t f) { (void)f; return 0; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; *t0 = *t1 = 0; return 0; }
void mtl_ab_gpu(uint64_t f, double gpu_ms, double frame_ms) { (void)f; (void)gpu_ms; (void)frame_ms; }
void mtl_vbuf_stats(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d) { *a = *b = *c = *d = 0; }
void mtl_stats(uint64_t *passes, uint64_t *draws) { uint64_t p, t, f; vk_draw_stats(passes, draws, &p, &t, &f); }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = *skip = 0; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = *reuse = 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = *store = *full = 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { uint64_t a, b, f; vk_draw_stats(&a, &b, pso, lib, &f); }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = *pso_ns = 0; }
void mtl_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none) { *draws = *made = *none = 0; }
void mtl_alloc_stats(uint64_t *a, uint64_t *b, int *c) { *a = *b = 0; *c = 0; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = *reuploads = 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
uint64_t mtl_pkg_edge_skips(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
void mtl_pso_line(char *buf, size_t n) {
    uint64_t passes, draws, pipes, tr, fails;
    vk_draw_stats(&passes, &draws, &pipes, &tr, &fails);
    snprintf(buf, n, "vulkan: %llu pipelines, %llu translations (%llu failed)", (unsigned long long)pipes, (unsigned long long)tr, (unsigned long long)fails);
}

// host pieces the NVN layer calls (nxnative: hle_os.c, front_mac.m, libSystem)
int dm_detclock_enabled(void) { return 0; }
void dm_guest_frame_tick(void) {}
int front_screen_max_fps(void) { return 60; }
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; (void)buf; (void)nbytes; return -1; }
