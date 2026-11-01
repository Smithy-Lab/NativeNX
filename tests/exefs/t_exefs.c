// SPDX-License-Identifier: GPL-3.0-or-later
// Mods' code patches (src/exefs_patch.c) on a made-up module image: an IPSwitch text patch (the layout of the BotW 1.9.0
// widescreen mod: @nsobid, @flag offset_shift 0x100, @enabled ... @stop), one for another build (left out), a @disabled
// section (left out), an IPS file and an IPS32 file named by the build ID. Prints "nxl-tests: passed" when all hold
#include "platform.h"
#include "nxl.h"
#include <stdarg.h>
#include <sys/stat.h>
int nxl_verbose = 1;
void nxl_log(const char *tag, const char *fmt, ...) { va_list a; va_start(a, fmt); fputs(tag, stdout); vprintf(fmt, a); putchar('\n'); va_end(a); }
void exefs_patches_apply(module_t *m);
static int fails;
static void check(const char *what, int ok) { printf("%s - %s\n", ok ? "ok" : "not ok", what); fails += !ok; }
static void wr(const char *path, const void *d, size_t n) { FILE *f = fopen(path, "wb"); fwrite(d, 1, n, f); fclose(f); }
int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "t_exefs";
    mkdir(dir, 0755);
    char p[1024], q[1024];
    snprintf(p, sizeof p, "%s/a", dir), mkdir(p, 0755);
    snprintf(q, sizeof q, "%s/b", dir), mkdir(q, 0755);
    static uint8_t img[0x10000];
    module_t m = {0};
    snprintf(m.name, sizeof m.name, "main");
    m.base = img, m.size = sizeof img;
    const uint8_t bid[16] = {0xCD, 0x57, 0xB2, 0x3F, 0xA4, 0xBB, 0xAD, 0x65, 0x80, 0x3D, 0x97, 0x88, 0xC0, 0x18, 0x21, 0xEE};
    memcpy(m.build_id, bid, 16);
    char f[1200];
    snprintf(f, sizeof f, "%s/1.9.0.pchtxt", p);
    const char *txt =
        "@nsobid-CD57B23FA4BBAD65803D9788C01821EE\n\n# a widescreen patch\n\n@flag print_values\n@flag offset_shift 0x100\n\n// 21:9\n\n"
        "@enabled\n00001000 CF719C52 // mov    w15, #0xE38E\n00001004 0F03A872 // movk   w15, #0x4018, LSL #16\n00001008 E001271E\n"
        "@disabled\n00002000 FFFFFFFF\n@enabled\n00003000 \"hi\"\n@stop\n00004000 FFFFFFFF\n";
    wr(f, txt, strlen(txt));
    snprintf(f, sizeof f, "%s/other.pchtxt", p);
    const char *other = "@nsobid-0123456789ABCDEF0123456789ABCDEF\n@flag offset_shift 0x100\n@enabled\n00005000 FFFFFFFF\n@stop\n";
    wr(f, other, strlen(other));
    // IPS: header PATCH, record at 0x100 + 0x6000 of 2 bytes, an RLE record of 3 x 0xAB at 0x100 + 0x6010, EOF
    snprintf(f, sizeof f, "%s/CD57B23FA4BBAD65803D9788C01821EE.ips", q);
    const uint8_t ips[] = {'P', 'A', 'T', 'C', 'H', 0x00, 0x61, 0x00, 0x00, 0x02, 0x12, 0x34, 0x00, 0x61, 0x10, 0x00, 0x00, 0x00, 0x03, 0xAB, 'E', 'O', 'F'};
    wr(f, ips, sizeof ips);
    snprintf(f, sizeof f, "%s/CD57B23FA4BBAD65.ips", q);   // IPS32, the build ID shortened (trailing zeros trimmed: does not match this one)
    const uint8_t ips32[] = {'I', 'P', 'S', '3', '2', 0x00, 0x00, 0x71, 0x00, 0x00, 0x01, 0x77, 'E', 'E', 'O', 'F'};
    wr(f, ips32, sizeof ips32);
    snprintf(f, sizeof f, "%s" NXL_LIST_SEP "%s", p, q);
    setenv("NXL_EXEFS_PATCHES", f, 1);
    exefs_patches_apply(&m);
    uint32_t w0, w1, w2;
    memcpy(&w0, img + 0x1000, 4), memcpy(&w1, img + 0x1004, 4), memcpy(&w2, img + 0x1008, 4);
    printf("words at 0x1000: %08x %08x %08x\n", w0, w1, w2);
    check("a text patch's bytes land at its offset (mov w15 / movk / fmov s0)", w0 == 0x529C71CF && w1 == 0x72A8030F && w2 == 0x1E2701E0);
    check("... its @disabled section and what follows @stop are left out", img[0x2000] == 0 && img[0x4000] == 0);
    check("... a string value is written as its bytes", img[0x3000] == 'h' && img[0x3001] == 'i');
    check("a text patch for another build is left out", img[0x5000] == 0);
    check("an IPS file named by the build ID: its record and its run", img[0x6000] == 0x12 && img[0x6001] == 0x34 && img[0x6010] == 0xAB && img[0x6012] == 0xAB && img[0x6013] == 0);
    check("an IPS file whose shortened name stands for another build is left out", img[0x7000] == 0);
    printf("nxl-tests: %s\n", fails ? "failed" : "passed");
    return fails != 0;
}
