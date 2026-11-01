// SPDX-License-Identifier: GPL-3.0-or-later
// elf2nso: an AArch64 ELF shared object (as tests/guest/guest.ld lays it out: text from address 0, then ro, then data
// and bss, each starting on a page) to an NSO, the Switch's executable format, with LZ4-compressed segments.
// usage: elf2nso [-u] input.elf output.nso     (-u: leave the segments uncompressed)
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lz4.h"

static void fail(const char *m) { fprintf(stderr, "elf2nso: %s\n", m); exit(1); }

// elf2nso: turns an ELF (the guest tests) into an NSO module, its segments LZ4-compressed unless -u.
int main(int argc, char **argv) {
    int compress = 1, a = 1;
    if (argc > 1 && !strcmp(argv[1], "-u")) compress = 0, a++;
    if (argc - a != 2) fail("usage: elf2nso [-u] input.elf output.nso");
    FILE *f = fopen(argv[a], "rb");
    if (!f) fail("cannot open the input");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *e = malloc(n);
    if (fread(e, 1, n, f) != (size_t)n) fail("short read");
    fclose(f);
    Elf64_Ehdr *eh = (Elf64_Ehdr *)e;
    if (memcmp(eh->e_ident, ELFMAG, 4) || eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_machine != EM_AARCH64)
        fail("not a 64-bit AArch64 ELF");
    Elf64_Phdr *ph = (Elf64_Phdr *)(e + eh->e_phoff), *seg[3] = {0};
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        int k = (ph[i].p_flags & PF_X) ? 0 : (ph[i].p_flags & PF_W) ? 2 : 1;
        if (seg[k]) fail("more than one load segment of the same kind");
        seg[k] = &ph[i];
    }
    if (!seg[0] || seg[0]->p_vaddr) fail("the text segment must start at address 0");
    uint8_t hdr[0x100] = {0};
    memcpy(hdr, "NSO0", 4);
    uint32_t *w = (uint32_t *)hdr;
    uint8_t *out[3];
    int osz[3];
    uint32_t off = 0x100;
    for (int k = 0; k < 3; k++) {
        uint32_t size = seg[k] ? (uint32_t)seg[k]->p_filesz : 0;
        const uint8_t *src = seg[k] ? e + seg[k]->p_offset : NULL;
        if (compress && size) {
            out[k] = malloc(LZ4_compressBound(size));
            osz[k] = LZ4_compress_default((const char *)src, (char *)out[k], size, LZ4_compressBound(size));
            if (osz[k] <= 0) fail("LZ4 compression failed");
            w[3] |= 1u << k;
        } else {
            out[k] = (uint8_t *)src;
            osz[k] = size;
        }
        w[4 + k * 4] = off;                                       // file offset
        w[5 + k * 4] = seg[k] ? (uint32_t)seg[k]->p_vaddr : 0;    // memory offset
        w[6 + k * 4] = size;                                      // size in memory (bytes from the file)
        w[0x60 / 4 + k] = osz[k];                                 // size in the file
        off += osz[k];
    }
    if (seg[2]) w[0x3C / 4] = (uint32_t)(seg[2]->p_memsz - seg[2]->p_filesz);   // bss
    // a build ID from the contents (64-bit FNV-1a, four ways), so two different test programs never share one
    for (int q = 0; q < 4; q++) {
        uint64_t h = 1469598103934665603ull ^ q;
        for (long i = 0; i < n; i++) h = (h ^ e[i]) * 1099511628211ull;
        memcpy(hdr + 0x40 + 8 * q, &h, 8);
    }
    FILE *o = fopen(argv[a + 1], "wb");
    if (!o) fail("cannot write the output");
    fwrite(hdr, 1, sizeof hdr, o);
    for (int k = 0; k < 3; k++) fwrite(out[k], 1, osz[k], o);
    fclose(o);
    return 0;
}
