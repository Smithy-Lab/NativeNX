// SPDX-License-Identifier: GPL-3.0-or-later
// nxlinux: load a Switch program's code modules, recompile them for this machine, and run them.
#include "nxl.h"
#include "platform.h"
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

// The shared font (nn::pl) a game draws its text with: nxnative's hle_sdk16.c opens DM_SHARED_FONT, then
// $DM_FONT_DIR/standard.ttf, then a macOS path. On Linux, when neither is given, fontconfig's sans-serif TrueType font
// stands in (the console's own font, dumped from it, can be given with DM_SHARED_FONT)
static int is_ttf(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    unsigned char m[4] = {0};
    size_t n = fread(m, 1, 4, f);
    fclose(f);
    return n == 4 && ((m[0] == 0 && m[1] == 1 && m[2] == 0 && m[3] == 0) || !memcmp(m, "true", 4));   // TrueType outlines
}
// Finds a font for the system's shared font (the button icons beside nxlinux, then Latin and CJK fonts of the PC).
static void find_shared_font(void) {
    const char *dir = getenv("DM_FONT_DIR");
    char p[4096];
    // the button icons: the font the build generates next to nxlinux (cmake/fonts.cmake), or an install's copy
    if (!getenv("DM_SHARED_FONT_EXT") && !(dir && snprintf(p, sizeof p, "%s/nintendo_ext.ttf", dir) > 0 && !access(p, R_OK)))
        if (!nxl_data_file("nxlinux", "nintendo_ext.ttf", p, sizeof p) && is_ttf(p)) setenv("DM_SHARED_FONT_EXT", p, 1);
    if (getenv("DM_SHARED_FONT")) return;
    if (dir) { snprintf(p, sizeof p, "%s/standard.ttf", dir); if (!access(p, R_OK)) return; }
    // static TrueType fonts first (the SDK's font engine predates variable fonts), Latin before CJK-only fallbacks
    const char *fixed[] = {"/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf", "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
                           "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
                           "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        if (is_ttf(fixed[i])) { setenv("DM_SHARED_FONT", fixed[i], 1); LOG("shared font: %s (DM_SHARED_FONT picks another)", fixed[i]); return; }
#ifdef _WIN32   // Windows' own (Arial, then Segoe UI), else the one NativeNX's zip brings (data\fonts)
    const char *windir = getenv("WINDIR");
    static const char *const win[] = {"arial.ttf", "segoeui.ttf"};
    for (int i = 0; i < 2; i++) {
        snprintf(p, sizeof p, "%s/Fonts/%s", windir && *windir ? windir : "C:/Windows", win[i]);
        if (is_ttf(p)) { setenv("DM_SHARED_FONT", p, 1); LOG("shared font: %s (DM_SHARED_FONT picks another)", p); return; }
    }
    if (!nxl_data_file("nativenx", "data/fonts/DejaVuSans.ttf", p, sizeof p) && is_ttf(p)) { setenv("DM_SHARED_FONT", p, 1); LOG("shared font: %s", p); return; }
#else
    const char *pats[] = {"sans-serif:fontformat=TrueType:variable=false", "sans-serif:fontformat=TrueType"};
    for (int i = 0; i < 2; i++) {
        char cmd[256];
        snprintf(cmd, sizeof cmd, "fc-match -f '%%{file}' '%s' 2>/dev/null", pats[i]);
        FILE *f = popen(cmd, "r");
        if (!f) break;
        size_t n = fread(p, 1, sizeof p - 1, f);
        pclose(f);
        p[n] = 0;
        if (n && is_ttf(p)) { setenv("DM_SHARED_FONT", p, 1); LOG("shared font: %s (fontconfig's sans-serif; DM_SHARED_FONT picks another)", p); return; }
    }
#endif
    RED("no TrueType font found for the shared font: install one (sudo dnf install google-noto-sans-fonts) or set DM_SHARED_FONT");
}

#define NXL_CODE_LEVEL 2   // raise when tools/patch_scarletnx.py changes what the recompiler emits
int nxl_verbose;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

// Writes a log line with a tag (one at a time across threads).
void nxl_log(const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_log_mu);
    fputs(tag, stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    pthread_mutex_unlock(&g_log_mu);
    va_end(ap);
}
// Reports a fatal error and stops the run.
_Noreturn void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fflush(stdout);
    fputs("[RED] fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    _exit(70);
}
_Noreturn void nxl_exit(int code) {
    fflush(stdout);
    fflush(stderr);
    _exit(code);   // guest threads may still be running: no atexit handlers or destructors
}

// The SDK boot of a retail title, in the order of nn::init::Start, as nxnative's dm/host/main.c runs it (rtld is not
// loaded: the loader did its relocations). Not yet tried on a game: see docs/ROADMAP.md.
static uint64_t req(const char *n) {
    uint64_t a = guest_sym(n);
    if (!a) die("the program has no %s: is this a retail title's exefs?", n);
    return a;
}
static char g_arg0[] = "game";
static uint64_t g_argv[2] = {(uint64_t)g_arg0, 0};
// Whether the function at fn (in m) computes the address of m's DT_INIT_ARRAY (adrp + add of that address), itself or
// in a function it calls directly (depth levels): then it runs the array
static int init_walks_array(module_t *m, uint64_t fn, int depth) {
    uint64_t lo = (uint64_t)m->base + m->text_off, hi = lo + m->text_size, want = (uint64_t)m->base + m->init_array;
    if (fn < lo || fn >= hi || (fn & 3)) return 0;
    const uint32_t *w = (const uint32_t *)(uintptr_t)fn;
    uint64_t page[32] = {0};
    for (int k = 0; k < 96 && fn + 4 * k < hi; k++) {
        uint32_t x = w[k];
        if ((x & 0x9F000000) == 0x90000000) {   // adrp xd, page
            int64_t imm = (int64_t)((((uint64_t)x >> 29) & 3) | (((uint64_t)x >> 3) & 0x1FFFFC));
            if (imm & 0x100000) imm -= 0x200000;
            page[x & 31] = ((fn + 4 * k) & ~0xFFFull) + (uint64_t)(imm << 12);
        } else if ((x & 0xFF800000) == 0x91000000 && page[(x >> 5) & 31]) {   // add xd, xn, #imm
            if (page[(x >> 5) & 31] + ((x >> 10) & 0xFFF) == want) return 1;
        } else if ((x & 0xFC000000) == 0x94000000 && depth > 0) {   // bl
            int64_t imm = (int64_t)(x & 0x3FFFFFF);
            if (imm & 0x2000000) imm -= 0x4000000;
            if (init_walks_array(m, fn + 4 * k + (uint64_t)(imm * 4), depth - 1)) return 1;
        } else if (x == 0xD65F03C0) break;   // ret
    }
    return 0;
}
int dm_sdk_major(void);   // compat_dm.c: 1 for the pre-1.0 SDKs

// Starts the game's SDK as Horizon's loader does: libc, nnosInitialize, the SDK's module init, then nninitStartup.
static void sdk_boot(void) {
    GUEST_CALL0(req("_init_libc0"));
    LOG(">>> nnosInitialize");
    GUEST_CALL(req("nnosInitialize"), main_thread_handle(), 0);
    GUEST_CALL0(req("_init_libc1"));
    LOG(">>> nninitInitializeSdkModule");
    GUEST_CALL0(req("nninitInitializeSdkModule"));
    GUEST_CALL(req("_ZN2nn2os11SetHostArgcEi"), 1);
    GUEST_CALL(req("_ZN2nn2os11SetHostArgvEPPc"), (uint64_t)g_argv);
    LOG(">>> nninitStartup");
    GUEST_CALL0(req("nninitStartup"));
    GUEST_CALL0(req("_init_libc2"));
    extern const int nxl_gfx_bridges __attribute__((weak));   // gfx/hle_egl.c: OpenGL and Vulkan go to the PC's driver
    for (int i = 0; i < g_nmodules; i++) {   // each module's DT_INIT (its global constructors), as rtld's callback does
        module_t *m = g_modules[i];
        if (&nxl_gfx_bridges && loader_is_gpu_driver(m) && !getenv("NXL_RUN_GPU_DRIVER")) {   // never called: not started either
            LOG("%s: the game's GPU driver: its constructors are not run (NXL_RUN_GPU_DRIVER=1 runs them)", m->name);
            continue;
        }
        uint64_t n = m->init_array ? m->init_arraysz / 8 : 0;
        int walks = m->init && n && init_walks_array(m, (uint64_t)m->base + m->init, 1);
        // the launch year's toolchain: a module's DT_INIT need not run its DT_INIT_ARRAY (its rtld ran it), and a module
        // may have no DT_INIT at all; newer SDKs' DT_INIT runs the array itself, so this changes nothing for them
        // only a module with no DT_INIT: a DT_INIT may run its array through an import this check cannot follow, and
        // running every constructor twice is worse than not at all (Breath of the Wild 1.6.0's crash was not this)
        int run_array = n && !walks && !m->init;
        if (nxl_verbose && (m->init || n))
            LOG("%s: DT_INIT %s, %llu init_array entries%s", m->name, m->init ? "yes" : "no", (unsigned long long)n,
                !n ? "" : walks ? " (DT_INIT runs them)" : run_array ? " (run here: no DT_INIT)" : " (left to DT_INIT)");
        if (m->init) GUEST_CALL0((uint64_t)m->base + m->init);
        for (uint64_t k = 0; run_array && k < n; k++) {
            uint64_t f = ((uint64_t *)((uint8_t *)m->base + m->init_array))[k];
            if (f && f != ~0ull) GUEST_CALL0(f);
        }
    }
    LOG(">>> nnMain");
    GUEST_CALL0(req("nnMain"));
    LOG("nnMain returned");
}

static int is_file(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }
static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }

// Prints how to run nxlinux.
static void usage(void) {
    fprintf(stderr,
            "usage: nxlinux [options] <exefs folder | module.nso ...>\n"
            "  An exefs folder holds a title's code modules (main, subsdk0-9, sdk), extracted from your own game; its\n"
            "  files are read from DM_ROMFS (the extracted RomFS folder), its DLC from DM_AOC_DIR/<index>/.\n"
            "  -v, -vv          more output\n"
            "  --cache DIR      keep recompiled code in DIR (default: ~/.cache/nxlinux)\n"
            "  --no-cache       recompile every time\n"
            "  --boot sdk|entry how to start: the SDK's init sequence then nnMain (retail titles; the default when\n"
            "                   the program defines nnMain), or the main module's entry point (homebrew, tests)\n"
            "  --profile PATH   nxnative's game profile to use (a game.tsv or its folder); by default the exefs's title's\n"
            "                   profile is looked for in $NXL_GAMES_DIR and nxnative's dm/games\n"
            "  --list-hle       list the functions the host implements, and exit\n"
            "  --xref ADDR      after loading, list the code that refers to ADDR (main+0x1234: a global or its GOT slot)\n"
            "  --dump ADDR N    after loading, print N instruction words at ADDR\n");
    exit(2);
}

// nxlinux's start: options, the game's modules (an exefs folder, or files) loaded and linked, then its main thread run.
int main(int argc, char **argv) {
    const char *cache = NULL, *boot = NULL, *profile = NULL, *xref = NULL, *dump = NULL;
    int dump_n = 64;
    int no_cache = 0, list = 0;
    const char *paths[NXL_MAX_MODULES];
    int npaths = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) nxl_verbose = 1;
        else if (!strcmp(argv[i], "-vv")) nxl_verbose = 2;
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc) cache = argv[++i];
        else if (!strcmp(argv[i], "--no-cache")) no_cache = 1;
        else if (!strcmp(argv[i], "--boot") && i + 1 < argc) boot = argv[++i];
        else if (!strcmp(argv[i], "--list-hle")) list = 1;
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile = argv[++i];
        else if (!strcmp(argv[i], "--xref") && i + 1 < argc) xref = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 2 < argc) dump = argv[++i], dump_n = atoi(argv[++i]);
        else if (argv[i][0] == '-') usage();
        else if (npaths < NXL_MAX_MODULES) paths[npaths++] = argv[i];
    }
    if (!list) {   // first, before the host allocates much: the guest's regions must be free
        profile_load(npaths == 1 && is_dir(paths[0]) ? paths[0] : NULL, profile);
        mem_init();
    }
    hle_init();
    if (list) {
        extern const hle_ent __start_nxl_hle[], __stop_nxl_hle[];
        for (const hle_ent *e = __start_nxl_hle; e < __stop_nxl_hle; e++)
            printf("%s%s\n", e->name, e->force ? "  (forced)" : "");
        return 0;
    }
    if (!npaths) usage();
    if (npaths == 1 && is_dir(paths[0])) {   // an exefs: main, then subsdk0-9, then sdk (rtld's work is done here)
        static const char *order[] = {"main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4", "subsdk5",
                                      "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk"};
        char p[4096];
        for (size_t k = 0; k < sizeof order / sizeof *order; k++) {
            snprintf(p, sizeof p, "%s/%s", paths[0], order[k]);
            if (is_file(p)) nso_load(p);
        }
        if (!g_nmodules) die("%s has no main, subsdk or sdk module", paths[0]);
    } else
        for (int i = 0; i < npaths; i++) nso_load(paths[i]);
    loader_link_all();
    if (xref || dump) {   // look into the code, then exit (nothing runs)
        void nxl_xref(const char *spec);
        void nxl_dump(const char *spec, int n);
        if (xref) nxl_xref(xref);
        if (dump) nxl_dump(dump, dump_n > 0 && dump_n <= 4096 ? dump_n : 64);
        return 0;
    }
    void nxl_compat_modules_ready(void);
    nxl_compat_modules_ready();   // nxnative's module table (compat/nxnative/compat_dm.c)
    char dir[4096];
    if (!no_cache && !cache) {
        // a folder per recompiler patch level: code compiled before a change to the recompiler (tools/patch_scarletnx.py)
        // must not be reused, and the cache's own keys do not see such a change. 2: the narrower store-exclusive
        char base[4000];
        nxl_user_dir(NXL_DIR_CACHE, base, sizeof base);
        snprintf(dir, sizeof dir, "%s/%d", base, NXL_CODE_LEVEL);
        cache = dir;
    }
    if (!no_cache) {
        char tmp[4096];
        snprintf(tmp, sizeof tmp, "%s", cache);
        for (char *s = tmp + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(tmp, 0755); *s = '/'; }
        mkdir(tmp, 0755);
    }
    cpu_init(no_cache ? NULL : cache);
    int sdk = boot ? !strcmp(boot, "sdk") : guest_sym("nnMain") != 0;
    if (boot && strcmp(boot, "sdk") && strcmp(boot, "entry")) usage();
    find_shared_font();
    if (nxl_verbose) nxl_status_start();
    module_t *m0 = g_modules[0];
    int code = cpu_run_main((uint64_t)m0->base + m0->text_off, sdk ? sdk_boot : NULL);
    nxl_exit(code);
}
