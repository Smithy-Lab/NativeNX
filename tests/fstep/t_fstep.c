// SPDX-License-Identifier: GPL-3.0-or-later
// The frame step stubs as the loader moves them (src/fstep.c): for each build in nxl_fstep.c, given its patch area and state
// page offsets on the command line as "build p st", prints the moved stub words and site words (t_fstep.py compares them with
// the stubs assembled at those offsets directly)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t off, len; const uint8_t *orig; } nxl_fstep_site;
typedef struct { uint32_t off, n; const uint32_t *w; const char *why; } nxl_fstep_put;
typedef struct { const char *build; uint32_t p0, s0, nwords; const uint32_t *words; int render_scale;
                 uint32_t nsites; const nxl_fstep_site *sites; uint32_t nputs; const nxl_fstep_put *puts; } nxl_fstep;
extern const nxl_fstep nxl_fsteps[];
extern const size_t nxl_nfsteps;
int nxl_fstep_reloc(uint64_t p0, uint64_t s0, uint64_t p, uint64_t st, uint64_t at0, uint64_t at, uint32_t w, uint32_t *out, uint64_t *branch_to);
int main(int argc, char **argv) {
    for (int a = 1; a + 2 < argc; a += 3) {
        uint64_t p = strtoull(argv[a + 1], 0, 0), st = strtoull(argv[a + 2], 0, 0), to;
        for (size_t i = 0; i < nxl_nfsteps; i++) {
            const nxl_fstep *f = &nxl_fsteps[i];
            if (strcmp(f->build, argv[a])) continue;
            printf("%s stubs", f->build);
            for (uint32_t k = 0; k < f->nwords; k++) {
                uint32_t w;
                if (!nxl_fstep_reloc(f->p0, f->s0, p, st, f->p0 + 4ull * k, p + 4ull * k, f->words[k], &w, &to)) { printf(" FAIL"); break; }
                printf(" %08x", w);
            }
            printf("\n%s puts", f->build);
            for (uint32_t j = 0; j < f->nputs; j++)
                for (uint32_t k = 0; k < f->puts[j].n; k++) {
                    uint32_t w, at = f->puts[j].off + 4 * k;
                    if (!nxl_fstep_reloc(f->p0, f->s0, p, st, at, at, f->puts[j].w[k], &w, &to)) { printf(" FAIL"); break; }
                    printf(" %08x", w);
                }
            printf("\n");
        }
    }
    return 0;
}
