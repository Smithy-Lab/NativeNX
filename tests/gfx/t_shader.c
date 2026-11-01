// SPDX-License-Identifier: GPL-3.0-or-later
// The shader translation (gfx/nxl_shader.cpp) on hand-assembled NVN code areas: a vertex + fragment program and its
// descriptors; the SPIR-V is written to the folder given (for spirv-val), and the counts are checked
#include "../../gfx/nxl_shader.h"
#include "../maxwell_asm.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(what, cond) do { int ok_ = !!(cond); printf("%s - %s\n", ok_ ? "ok" : "not ok", what); fails += !ok_; } while (0)

int main(int argc, char **argv) {
    static uint8_t vs[4096], fs[4096], vctl[0x880], fctl[0x880];
    float red[4] = {1, 0, 0, 1};
    uint32_t nv = mx_vs_passthrough(vs), nf = mx_fs_color(fs, 1, red);
    memcpy(vctl + 0x704, &nv, 4); memcpy(fctl + 0x704, &nf, 4);
    nxl_sh_src src[NXL_SH_NSTAGE] = {0};
    src[NXL_SH_VS] = (nxl_sh_src){vs, nv, vctl};
    src[NXL_SH_FS] = (nxl_sh_src){fs, nf, fctl};
    nxl_sh_state st = {0};
    st.topology = 3;
    nxl_sh_result r;
    int rc = nxl_sh_translate(src, 0, &st, NULL, &r);
    if (rc) printf("error: %s\n", r.err);
    CHECK("a vertex + fragment program translates", rc == 0 && r.stages == (1u << NXL_SH_VS | 1u << NXL_SH_FS));
    CHECK("the vertex stage reads attributes 0 and 1", r.vs_reads_attr[0] && r.vs_reads_attr[1] && !r.vs_reads_attr[2]);
    CHECK("the fragment stage writes target 0", r.fs_writes[0] && !r.fs_writes[1]);
    CHECK("no descriptors", r.ndescs == 0);
    if (argc > 1 && rc == 0)
        for (int s = 0; s < NXL_SH_NSTAGE; s++) {
            if (!r.spirv[s]) continue;
            char p[512]; snprintf(p, sizeof p, "%s/stage%d.spv", argv[1], s);
            FILE *f = fopen(p, "wb"); fwrite(r.spirv[s], 4, r.words[s], f); fclose(f);
        }
    nxl_sh_free(&r);
    // the constant-colour fragment shader alone with the vertex shader
    nf = mx_fs_color(fs, 0, red); memcpy(fctl + 0x704, &nf, 4); src[NXL_SH_FS].code_len = nf;
    rc = nxl_sh_translate(src, 0, &st, NULL, &r);
    if (rc) printf("error: %s\n", r.err);
    CHECK("a constant-colour fragment shader translates", rc == 0);
    nxl_sh_free(&r);
    printf("nxl-tests: %s\n", fails ? "failed" : "passed");
    return fails != 0;
}
