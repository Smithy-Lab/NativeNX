# SPDX-License-Identifier: GPL-3.0-or-later
# The changes nxnative's dm/gfx/nvn/hle_nvn.c needs to build into nxlinux (tools/gen_thunks.py --patch). Each must
# match the source exactly once. They touch only how NVN entry points are registered and handed to the game; the NVN
# semantics are nxnative's, unchanged.
#
# On the Mac, NVN(name, ...) puts {name, h_name} into a Mach-O section, and nvnBootstrapLoader returns ARM thunks
# (nvn_thunks.S) that jump to the implementation. Here:
#   1. NVN(...) registers into an ELF section and leaves a scan marker, so gen_thunks.py writes a thunk for every
#      implementation, registered under "@nvn:<name>" (a name no import can bind to);
#   2. the slot table is filled from that ELF section;
#   3. nvnBootstrapLoader / nvnDeviceGetProcAddress return guest-callable addresses: an implementation's thunk, a
#      quiet stub returning 0 for nxnative's deliberately empty ones, and for the rest a stub that reports the name once
#      and returns 0 (as nxnative's nvn_unimpl_entry does).

REPLACE = [
    ('''typedef struct { const char *name; void *fn; } nvn_impl;
#define NVN(name, ret, ...)                                                                       \\
    static ret h_##name(__VA_ARGS__);                                                             \\
    static const NVNT_##name chk_##name __attribute__((unused)) = h_##name;                       \\
    __attribute__((used, section("__DATA,swnvn"))) static const nvn_impl e_##name = {#name, (void *)h_##name}; \\
    static ret h_##name(__VA_ARGS__)
// Newer interfaces without prototypes: no signature check
#define NVN_NOPROTO(name, ret, ...)                                                               \\
    static ret h_##name(__VA_ARGS__);                                                             \\
    __attribute__((used, section("__DATA,swnvn"))) static const nvn_impl e_##name = {#name, (void *)h_##name}; \\
    static ret h_##name(__VA_ARGS__)
''',
     '''typedef struct { const char *name; void *fn; } nvn_impl;
// nxlinux: a section of its own (NXL_SECTION, src/hle_abi.h), and a marker for tools/gen_thunks.py (the thunk is registered as "@nvn:<name>")
#ifdef NXL_GEN_SCAN
#define NXL_NVN_MARK(name, ret, ...) __nxl_hle(0, "@nvn:" #name, NULL, ret, h_##name, __VA_ARGS__)
#else
#define NXL_NVN_MARK(name, ret, ...)
#endif
#define NVN(name, ret, ...)                                                                       \\
    NXL_NVN_MARK(name, ret, __VA_ARGS__)                                                          \\
    static ret h_##name(__VA_ARGS__);                                                             \\
    static const NVNT_##name chk_##name __attribute__((unused)) = h_##name;                       \\
    __attribute__((used, retain, section(NXL_SECTION("nxl_nvn")))) static const nvn_impl e_##name = {#name, (void *)h_##name}; \\
    static ret h_##name(__VA_ARGS__)
// Newer interfaces without prototypes: no signature check
#define NVN_NOPROTO(name, ret, ...)                                                               \\
    NXL_NVN_MARK(name, ret, __VA_ARGS__)                                                          \\
    static ret h_##name(__VA_ARGS__);                                                             \\
    __attribute__((used, retain, section(NXL_SECTION("nxl_nvn")))) static const nvn_impl e_##name = {#name, (void *)h_##name}; \\
    static ret h_##name(__VA_ARGS__)
'''),

    ('''    const nvn_impl *im = (const nvn_impl *)getsectiondata(&_mh_execute_header, "__DATA", "swnvn", &sz);
''',
     '''    extern const nvn_impl __start_nxl_nvn[], __stop_nxl_nvn[];   // nxlinux: the section's bounds (NXL_SECTION)
    const nvn_impl *im = __start_nxl_nvn;
    sz = (unsigned long)((const char *)__stop_nxl_nvn - (const char *)__start_nxl_nvn);
'''),

    ('''static void *nvn_lookup(const char *name) {
    nvn_init_slots();
    for (unsigned i = 0; i < NVN_NFUNC; i++)
        if (!strcmp(nvn_names[i], name)) return nvn_thunk[i];''',
     '''void *nxl_nvn_entry(unsigned i);   // nxlinux: the guest-callable address of entry i (after the empty implementations)
static void *nvn_lookup(const char *name) {
    nvn_init_slots();
    for (unsigned i = 0; i < NVN_NFUNC; i++)
        if (!strcmp(nvn_names[i], name)) return nxl_nvn_entry(i);'''),

    ('''// ---------- Window / present ----------
''',
     '''// nxlinux: what the game gets for entry i. Its thunk (generated, "@nvn:<name>") when nxnative implements it; a quiet
// stub returning 0 for the empty implementations; else a stub that reports the name once and returns 0.
uint64_t hle_lookup(const char *name, int force_only);
uint64_t hle_soft_stub(const char *name, int quiet);
void *nxl_nvn_entry(unsigned i) {
    static void *_Atomic cache[NVN_NFUNC];
    void *a = atomic_load(&cache[i]);
    if (a) return a;
    void *fn = nvn_slot[i].fn;
    if (fn == (void *)nvn_unimpl_entry) a = (void *)hle_soft_stub(nvn_names[i], 0);
    else if (fn == (void *)nvn_nop) a = (void *)hle_soft_stub(nvn_names[i], 1);
    else {
        char n[200];
        snprintf(n, sizeof n, "@nvn:%s", nvn_names[i]);
        if (!(a = (void *)hle_lookup(n, 0))) die("NVN %s is implemented but has no thunk", nvn_names[i]);
    }
    atomic_store(&cache[i], a);
    return a;
}

// ---------- Window / present ----------
'''),

    # 5. nvnProgramSetShaders tells the Vulkan backend (gfx/vk_draw.c), which keeps a copy of the control sections: the
    #    game may free them once the call returns, and the shaders are translated later, at the first draw
    ('''    OLOG("ProgramSetShaders %p n=%d data0=%#llx", (void *)p, n, n ? (unsigned long long)sd[0].data : 0ull);
    return 1;
''',
     '''    OLOG("ProgramSetShaders %p n=%d data0=%#llx", (void *)p, n, n ? (unsigned long long)sd[0].data : 0ull);
    { void mtl_prog_set(prog_t *); mtl_prog_set(o); }   // nxlinux: the Vulkan backend copies the control sections
    return 1;
'''),
]
