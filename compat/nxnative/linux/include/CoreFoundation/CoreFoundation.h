// SPDX-License-Identifier: GPL-3.0-or-later
// The one CoreFoundation type nxnative's NVN layer uses: a mutable dictionary with pointer keys and values (created
// with NULL callbacks, so nothing is retained). An open-addressing hash table.
#pragma once
#include <stdint.h>
#include <stdlib.h>
typedef const void *CFTypeRef;
typedef long CFIndex;
typedef struct { const void **k, **v; size_t cap, n; } nxl_cfdict;
typedef nxl_cfdict *CFMutableDictionaryRef;
typedef const nxl_cfdict *CFDictionaryRef;
typedef const void *CFAllocatorRef;
static inline size_t nxl_cf_slot(const nxl_cfdict *d, const void *k) {
    size_t i = ((uintptr_t)k >> 3) * 0x9E3779B97F4A7C15ull & (d->cap - 1);
    while (d->k[i] && d->k[i] != k) i = (i + 1) & (d->cap - 1);
    return i;
}
static inline CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex cap, const void *kcb, const void *vcb) {
    (void)a; (void)cap; (void)kcb; (void)vcb;
    nxl_cfdict *d = calloc(1, sizeof *d);
    d->cap = 64;
    d->k = calloc(d->cap, sizeof *d->k), d->v = calloc(d->cap, sizeof *d->v);
    return d;
}
static inline const void *CFDictionaryGetValue(CFDictionaryRef d, const void *k) {
    size_t i = nxl_cf_slot(d, k);
    return d->k[i] ? d->v[i] : NULL;
}
static inline void CFDictionarySetValue(CFMutableDictionaryRef d, const void *k, const void *v) {
    if ((d->n + 1) * 2 > d->cap) {
        nxl_cfdict o = *d;
        d->cap *= 2, d->n = 0;
        d->k = calloc(d->cap, sizeof *d->k), d->v = calloc(d->cap, sizeof *d->v);
        for (size_t i = 0; i < o.cap; i++) if (o.k[i]) { size_t j = nxl_cf_slot(d, o.k[i]); d->k[j] = o.k[i], d->v[j] = o.v[i], d->n++; }
        free(o.k), free(o.v);
    }
    size_t i = nxl_cf_slot(d, k);
    if (!d->k[i]) d->n++;
    d->k[i] = k, d->v[i] = v;
}
static inline void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *k) {
    size_t i = nxl_cf_slot(d, k);
    if (!d->k[i]) return;
    d->k[i] = d->v[i] = NULL, d->n--;
    for (size_t j = (i + 1) & (d->cap - 1); d->k[j]; j = (j + 1) & (d->cap - 1)) {   // re-insert the rest of the cluster
        const void *kk = d->k[j], *vv = d->v[j];
        d->k[j] = d->v[j] = NULL, d->n--;
        size_t s = nxl_cf_slot(d, kk);
        d->k[s] = kk, d->v[s] = vv, d->n++;
    }
}
static inline CFIndex CFDictionaryGetCount(CFDictionaryRef d) { return (CFIndex)d->n; }
