// SPDX-License-Identifier: GPL-3.0-or-later
// Drawing through NVN as a game does, rendered by the Vulkan backend (lavapipe in the test environment): a program
// of hand-assembled Maxwell shaders (tests/maxwell_asm.h), a vertex buffer, a 64x64 render target cleared and drawn
// on, read back with nvnTextureReadTexels. The triangle covers the lower-left half of clip space, so with NVN's default
// GL origin pixel (8, 8) is drawn and (56, 56) keeps the clear colour.
#include "nx.h"
#include "nvn.h"
#include "../maxwell_asm.h"

PFNNVNGENERICFUNCPTRPROC nvnBootstrapLoader(const char *name);
static PFNNVNDEVICEGETPROCADDRESSPROC get_proc;
#define P(T, name) ((T)get_proc(NULL, name))

static NVNdevice dev;
static NVNmemoryPool pool, code_pool, tex_pool;
static NVNqueue queue;
static NVNcommandBuffer cmd;
static NVNprogram prog;
static NVNtexture rt;
static u8 pool_mem[256 * 1024] __attribute__((aligned(4096)));
static u8 code_mem[64 * 1024] __attribute__((aligned(4096)));
static u8 tex_mem[512 * 1024] __attribute__((aligned(65536)));
static u8 control[16 * 1024] __attribute__((aligned(8)));
static u8 vs_ctl[0x880] __attribute__((aligned(8))), fs_ctl[0x880] __attribute__((aligned(8)));
static u32 texels[64 * 64];

static NVNtexture smp_tex;
static NVNtexturePool tpool;
static NVNsamplerPool spool;
static NVNsampler sampler;
static NVNtextureHandle tex_handle;

// mode 0: a constant colour, 1: the vertices' colour through a varying, 2: the texture at the vertices' (u, v)
static NVNtexture *g_target = &rt;
static int g_occ_test;
static const float *g_ub_then;   // mode 3: after the draw, update the uniform buffer to this colour and draw again
static u64 g_vbuf_size = 96;   // the vertex buffer's size as bound   // count the triangle's samples (SAMPLES_PASSED) into pool_mem + 0x1F000
static const NVNtextureView *g_target_view;
static int g_vp_w = 64, g_vp_h = 64;   // the viewport and scissor (and so the clear)
static int g_full;                     // the triangle covers the whole viewport (else its lower-left half)
static int draw_and_read(int mode, const float vcolor[4]) {
    static const float clear[4] = {0, 0, 1, 1};
    // shaders in the code pool: vertex at 0, fragment at 0x1000
    uint32_t nv = mx_vs_passthrough(code_mem), nf = mode == 2 ? mx_fs_texture(code_mem + 0x1000) : mode == 3 ? mx_fs_uniform(code_mem + 0x1000)
                  : mode == 4 ? mx_fs_fetch(code_mem + 0x1000) : mode == 5 ? mx_fs_size(code_mem + 0x1000) : mx_fs_color(code_mem + 0x1000, mode, vcolor);
    __builtin_memset(vs_ctl, 0, sizeof vs_ctl), __builtin_memset(fs_ctl, 0, sizeof fs_ctl);
    __builtin_memcpy(vs_ctl + 0x704, &nv, 4), __builtin_memcpy(fs_ctl + 0x704, &nf, 4);
    NVNbufferAddress code = P(PFNNVNMEMORYPOOLGETBUFFERADDRESSPROC, "nvnMemoryPoolGetBufferAddress")(&code_pool);
    NVNshaderData sd[2] = {{code, vs_ctl}, {code + 0x1000, fs_ctl}};
    if (!P(PFNNVNPROGRAMSETSHADERSPROC, "nvnProgramSetShaders")(&prog, 2, sd)) return 0;

    // three vertices: position xyzw, colour rgba
    float *vb = (float *)(pool_mem + 0x10000);
    const float pos[3][4] = {{-1, -1, 0, 1}, {1, -1, 0, 1}, {-1, 1, 0, 1}}, full[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
    for (int i = 0; i < 3; i++) for (int k = 0; k < 4; k++) vb[8 * i + k] = g_full ? full[i][k] : pos[i][k], vb[8 * i + 4 + k] = vcolor[k];
    NVNbufferAddress base = P(PFNNVNMEMORYPOOLGETBUFFERADDRESSPROC, "nvnMemoryPoolGetBufferAddress")(&pool);

    NVNvertexAttribState va[2];
    NVNvertexStreamState vs;
    for (int i = 0; i < 2; i++) {
        P(PFNNVNVERTEXATTRIBSTATESETDEFAULTSPROC, "nvnVertexAttribStateSetDefaults")(&va[i]);
        P(PFNNVNVERTEXATTRIBSTATESETFORMATPROC, "nvnVertexAttribStateSetFormat")(&va[i], NVN_FORMAT_RGBA32F, 16 * i);
        P(PFNNVNVERTEXATTRIBSTATESETSTREAMINDEXPROC, "nvnVertexAttribStateSetStreamIndex")(&va[i], 0);
    }
    P(PFNNVNVERTEXSTREAMSTATESETDEFAULTSPROC, "nvnVertexStreamStateSetDefaults")(&vs);
    P(PFNNVNVERTEXSTREAMSTATESETSTRIDEPROC, "nvnVertexStreamStateSetStride")(&vs, 32);

    P(PFNNVNCOMMANDBUFFERBEGINRECORDINGPROC, "nvnCommandBufferBeginRecording")(&cmd);
    const NVNtexture *rts[1] = {g_target};
    const NVNtextureView *rvs[1] = {g_target_view};
    P(PFNNVNCOMMANDBUFFERSETRENDERTARGETSPROC, "nvnCommandBufferSetRenderTargets")(&cmd, 1, rts, g_target_view ? rvs : NULL, NULL, NULL);
    P(PFNNVNCOMMANDBUFFERSETVIEWPORTPROC, "nvnCommandBufferSetViewport")(&cmd, 0, 0, g_vp_w, g_vp_h);
    P(PFNNVNCOMMANDBUFFERSETSCISSORPROC, "nvnCommandBufferSetScissor")(&cmd, 0, 0, g_vp_w, g_vp_h);
    P(PFNNVNCOMMANDBUFFERCLEARCOLORPROC, "nvnCommandBufferClearColor")(&cmd, 0, clear, NVN_CLEAR_COLOR_MASK_RGBA);
    P(PFNNVNCOMMANDBUFFERBINDPROGRAMPROC, "nvnCommandBufferBindProgram")(&cmd, &prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    P(PFNNVNCOMMANDBUFFERBINDVERTEXATTRIBSTATEPROC, "nvnCommandBufferBindVertexAttribState")(&cmd, 2, va);
    P(PFNNVNCOMMANDBUFFERBINDVERTEXSTREAMSTATEPROC, "nvnCommandBufferBindVertexStreamState")(&cmd, 1, &vs);
    P(PFNNVNCOMMANDBUFFERBINDVERTEXBUFFERPROC, "nvnCommandBufferBindVertexBuffer")(&cmd, 0, base + 0x10000, g_vbuf_size);
    if (mode == 3) {   // uniform buffer 0 of the fragment stage: the colour
        float *ub = (float *)(pool_mem + 0x18000);
        for (int k = 0; k < 4; k++) ub[k] = vcolor[k];
        P(PFNNVNCOMMANDBUFFERBINDUNIFORMBUFFERPROC, "nvnCommandBufferBindUniformBuffer")(&cmd, NVN_SHADER_STAGE_FRAGMENT, 0, base + 0x18000, 256);
    }
    if (mode == 2 || mode == 4 || mode == 5) {
        P(PFNNVNCOMMANDBUFFERSETTEXTUREPOOLPROC, "nvnCommandBufferSetTexturePool")(&cmd, &tpool);
        P(PFNNVNCOMMANDBUFFERSETSAMPLERPOOLPROC, "nvnCommandBufferSetSamplerPool")(&cmd, &spool);
        P(PFNNVNCOMMANDBUFFERBINDTEXTUREPROC, "nvnCommandBufferBindTexture")(&cmd, NVN_SHADER_STAGE_FRAGMENT, 0, tex_handle);
    }
    if (g_occ_test) P(PFNNVNCOMMANDBUFFERRESETCOUNTERPROC, "nvnCommandBufferResetCounter")(&cmd, NVN_COUNTER_TYPE_SAMPLES_PASSED);
    P(PFNNVNCOMMANDBUFFERDRAWARRAYSPROC, "nvnCommandBufferDrawArrays")(&cmd, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    if (mode == 3 && g_ub_then) {
        P(PFNNVNCOMMANDBUFFERUPDATEUNIFORMBUFFERPROC, "nvnCommandBufferUpdateUniformBuffer")(&cmd, base + 0x18000, 256, 0, 16, g_ub_then);
        P(PFNNVNCOMMANDBUFFERDRAWARRAYSPROC, "nvnCommandBufferDrawArrays")(&cmd, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    }
    if (g_occ_test) P(PFNNVNCOMMANDBUFFERREPORTCOUNTERPROC, "nvnCommandBufferReportCounter")(&cmd, NVN_COUNTER_TYPE_SAMPLES_PASSED, base + 0x1F000);
    NVNcommandHandle h = P(PFNNVNCOMMANDBUFFERENDRECORDINGPROC, "nvnCommandBufferEndRecording")(&cmd);
    P(PFNNVNQUEUESUBMITCOMMANDSPROC, "nvnQueueSubmitCommands")(&queue, 1, &h);
    P(PFNNVNQUEUEFINISHPROC, "nvnQueueFinish")(&queue);

    NVNcopyRegion r = {0, 0, 0, 64, 64, 1};
    if (g_target == &rt) P(PFNNVNTEXTUREREADTEXELSPROC, "nvnTextureReadTexels")(&rt, NULL, &r, texels);
    return 1;
}

// pools made and finalized on another thread while this one submits (a game entering a level frees pools as its render
// thread draws: finalizing waited for the GPU without the queue lock, and Super Mario Bros. Wonder hung there)
static u8 churn_mem[4][64 * 1024] __attribute__((aligned(4096)));
static u8 churn_stack[64 * 1024] __attribute__((aligned(16)));
static volatile int churn_done, churn_count;
static void churn(void *arg) {
    (void)arg;
    for (int n = 0; n < 200; n++) {
        NVNmemoryPool cp;
        NVNmemoryPoolBuilder pb;
        P(PFNNVNMEMORYPOOLBUILDERSETDEFAULTSPROC, "nvnMemoryPoolBuilderSetDefaults")(&pb);
        P(PFNNVNMEMORYPOOLBUILDERSETDEVICEPROC, "nvnMemoryPoolBuilderSetDevice")(&pb, &dev);
        P(PFNNVNMEMORYPOOLBUILDERSETSTORAGEPROC, "nvnMemoryPoolBuilderSetStorage")(&pb, churn_mem[n & 3], sizeof churn_mem[0]);
        P(PFNNVNMEMORYPOOLBUILDERSETFLAGSPROC, "nvnMemoryPoolBuilderSetFlags")(&pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
        if (P(PFNNVNMEMORYPOOLINITIALIZEPROC, "nvnMemoryPoolInitialize")(&cp, &pb)) churn_count++;
        P(PFNNVNMEMORYPOOLFINALIZEPROC, "nvnMemoryPoolFinalize")(&cp);
    }
    churn_done = 1;
    svcExitThread();
}

// the sampled texture finalized and made again on another thread while this one draws with it (a game frees and makes
// textures while its render thread draws: the backend freed its record at once, and a draw read freed memory)
static NVNtextureBuilder tex_tb;
static int tex_tres;
static volatile int tex_churn_done;
static u8 tex_churn_stack[64 * 1024] __attribute__((aligned(16)));
static void tex_churn(void *arg) {
    (void)arg;
    for (int n = 0; n < 300; n++) {
        P(PFNNVNTEXTUREFINALIZEPROC, "nvnTextureFinalize")(&smp_tex);
        P(PFNNVNTEXTUREINITIALIZEPROC, "nvnTextureInitialize")(&smp_tex, &tex_tb);
        P(PFNNVNTEXTUREPOOLREGISTERTEXTUREPROC, "nvnTexturePoolRegisterTexture")(&tpool, tex_tres + 1, &smp_tex, NULL);
        svcSleepThread(1000000);   // 1 ms: the draws overlap the whole run
    }
    tex_churn_done = 1;
    svcExitThread();
}

static u32 rgba8(const float c[4]) {
    u32 v = 0;
    for (int k = 0; k < 4; k++) v |= (u32)(c[k] * 255.0f + 0.5f) << (8 * k);
    return v;
}

int test_main(void) {
    get_proc = (PFNNVNDEVICEGETPROCADDRESSPROC)nvnBootstrapLoader("nvnDeviceGetProcAddress");
    NVNdeviceBuilder db;
    P(PFNNVNDEVICEBUILDERSETDEFAULTSPROC, "nvnDeviceBuilderSetDefaults")(&db);
    CHECK("nvnDeviceInitialize", P(PFNNVNDEVICEINITIALIZEPROC, "nvnDeviceInitialize")(&dev, &db));

    struct { NVNmemoryPool *p; u8 *mem; size_t n; int flags; } pools[3] = {
        {&pool, pool_mem, sizeof pool_mem, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED},
        {&code_pool, code_mem, sizeof code_mem, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED | NVN_MEMORY_POOL_FLAGS_SHADER_CODE},
        {&tex_pool, tex_mem, sizeof tex_mem, NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED},
    };
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        NVNmemoryPoolBuilder pb;
        P(PFNNVNMEMORYPOOLBUILDERSETDEFAULTSPROC, "nvnMemoryPoolBuilderSetDefaults")(&pb);
        P(PFNNVNMEMORYPOOLBUILDERSETDEVICEPROC, "nvnMemoryPoolBuilderSetDevice")(&pb, &dev);
        P(PFNNVNMEMORYPOOLBUILDERSETSTORAGEPROC, "nvnMemoryPoolBuilderSetStorage")(&pb, pools[i].mem, pools[i].n);
        P(PFNNVNMEMORYPOOLBUILDERSETFLAGSPROC, "nvnMemoryPoolBuilderSetFlags")(&pb, pools[i].flags);
        ok &= P(PFNNVNMEMORYPOOLINITIALIZEPROC, "nvnMemoryPoolInitialize")(pools[i].p, &pb);
    }
    CHECK("memory pools (data, shader code, a texture)", ok);
    NVNqueueBuilder qb;
    P(PFNNVNQUEUEBUILDERSETDEFAULTSPROC, "nvnQueueBuilderSetDefaults")(&qb);
    P(PFNNVNQUEUEBUILDERSETDEVICEPROC, "nvnQueueBuilderSetDevice")(&qb, &dev);
    CHECK("nvnQueueInitialize", P(PFNNVNQUEUEINITIALIZEPROC, "nvnQueueInitialize")(&queue, &qb));
    CHECK("nvnCommandBufferInitialize", P(PFNNVNCOMMANDBUFFERINITIALIZEPROC, "nvnCommandBufferInitialize")(&cmd, &dev));
    P(PFNNVNCOMMANDBUFFERADDCOMMANDMEMORYPROC, "nvnCommandBufferAddCommandMemory")(&cmd, &pool, 0x20000, 0x10000);
    P(PFNNVNCOMMANDBUFFERADDCONTROLMEMORYPROC, "nvnCommandBufferAddControlMemory")(&cmd, control, sizeof control);
    CHECK("nvnProgramInitialize", P(PFNNVNPROGRAMINITIALIZEPROC, "nvnProgramInitialize")(&prog, &dev));

    NVNtextureBuilder tb;
    P(PFNNVNTEXTUREBUILDERSETDEFAULTSPROC, "nvnTextureBuilderSetDefaults")(&tb);
    P(PFNNVNTEXTUREBUILDERSETDEVICEPROC, "nvnTextureBuilderSetDevice")(&tb, &dev);
    P(PFNNVNTEXTUREBUILDERSETTARGETPROC, "nvnTextureBuilderSetTarget")(&tb, NVN_TEXTURE_TARGET_2D);
    P(PFNNVNTEXTUREBUILDERSETFORMATPROC, "nvnTextureBuilderSetFormat")(&tb, NVN_FORMAT_RGBA8);
    P(PFNNVNTEXTUREBUILDERSETSIZE2DPROC, "nvnTextureBuilderSetSize2D")(&tb, 64, 64);
    P(PFNNVNTEXTUREBUILDERSETSTORAGEPROC, "nvnTextureBuilderSetStorage")(&tb, &tex_pool, 0);
    CHECK("a 64x64 render target", P(PFNNVNTEXTUREINITIALIZEPROC, "nvnTextureInitialize")(&rt, &tb));

    const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1};
    CHECK("draw a triangle with a constant-colour fragment shader", draw_and_read(0, red));
    print("pixel (8, 8) %x, (56, 56) %x\n", texels[8 * 64 + 8], texels[56 * 64 + 56]);
    CHECK("... inside the triangle: the fragment shader's colour", texels[8 * 64 + 8] == rgba8(red));
    CHECK("... outside it: the clear colour", texels[56 * 64 + 56] == rgba8(blue));
    CHECK("draw again with the vertex colour interpolated", draw_and_read(1, green));
    print("pixel (8, 8) %x, (56, 56) %x\n", texels[8 * 64 + 8], texels[56 * 64 + 56]);
    CHECK("... inside: the vertices' colour, through a varying", texels[8 * 64 + 8] == rgba8(green));
    CHECK("... outside: cleared again", texels[56 * 64 + 56] == rgba8(blue));

    const float yellow[4] = {1, 1, 0, 1};
    CHECK("draw with the colour from a uniform buffer", draw_and_read(3, yellow));
    CHECK("... inside: the uniform buffer's colour", texels[8 * 64 + 8] == rgba8(yellow));

    // a copy into a texture of another format converts (NVN's 2D engine): red RGBA8 becomes red BGRA8, bytes B G R A. Copied
    // as bytes it would read as blue (Breath of the Wild's blurred scene behind its menus). Then the same copy mirrored in Y.
    {
        static NVNtexture bgra;
        NVNtextureBuilder cb;
        P(PFNNVNTEXTUREBUILDERSETDEFAULTSPROC, "nvnTextureBuilderSetDefaults")(&cb);
        P(PFNNVNTEXTUREBUILDERSETDEVICEPROC, "nvnTextureBuilderSetDevice")(&cb, &dev);
        P(PFNNVNTEXTUREBUILDERSETTARGETPROC, "nvnTextureBuilderSetTarget")(&cb, NVN_TEXTURE_TARGET_2D);
        P(PFNNVNTEXTUREBUILDERSETFORMATPROC, "nvnTextureBuilderSetFormat")(&cb, NVN_FORMAT_BGRA8);
        P(PFNNVNTEXTUREBUILDERSETSIZE2DPROC, "nvnTextureBuilderSetSize2D")(&cb, 64, 64);
        P(PFNNVNTEXTUREBUILDERSETSTORAGEPROC, "nvnTextureBuilderSetStorage")(&cb, &tex_pool, 0x10000);
        CHECK("a 64x64 BGRA8 texture", P(PFNNVNTEXTUREINITIALIZEPROC, "nvnTextureInitialize")(&bgra, &cb));
        draw_and_read(0, red);
        for (int flags = 0; flags <= NVN_COPY_FLAGS_MIRROR_Y_BIT; flags += NVN_COPY_FLAGS_MIRROR_Y_BIT) {
            NVNcopyRegion r = {0, 0, 0, 64, 64, 1};
            P(PFNNVNCOMMANDBUFFERBEGINRECORDINGPROC, "nvnCommandBufferBeginRecording")(&cmd);
            P(PFNNVNCOMMANDBUFFERCOPYTEXTURETOTEXTUREPROC, "nvnCommandBufferCopyTextureToTexture")(&cmd, &rt, NULL, &r, &bgra, NULL, &r, flags);
            NVNcommandHandle h = P(PFNNVNCOMMANDBUFFERENDRECORDINGPROC, "nvnCommandBufferEndRecording")(&cmd);
            P(PFNNVNQUEUESUBMITCOMMANDSPROC, "nvnQueueSubmitCommands")(&queue, 1, &h);
            P(PFNNVNQUEUEFINISHPROC, "nvnQueueFinish")(&queue);
            P(PFNNVNTEXTUREREADTEXELSPROC, "nvnTextureReadTexels")(&bgra, NULL, &r, texels);
            print("BGRA8 copy (flags %d): pixel (8, 8) %x, (8, 56) %x, (56, 56) %x\n", flags, texels[8 * 64 + 8], texels[56 * 64 + 8], texels[56 * 64 + 56]);
            u32 want_red = 0xffff0000, want_blue = 0xff0000ff;   // as BGRA8 bytes
            if (!flags) CHECK("RGBA8 -> BGRA8 copy keeps the colours (red stays red, blue stays blue)", texels[8 * 64 + 8] == want_red && texels[56 * 64 + 56] == want_blue);
            else CHECK("... mirrored in Y: rows swap", texels[56 * 64 + 8] == want_red && texels[8 * 64 + 56] == want_blue);
        }
    }

    // a texture over storage another format already uses (the image there is BGRA8, from the test above): a copy into
    // it and a clear of it are in its own format, RGB10A2. Breath of the Wild's layout captures are RGB10A2 over RGBA8
    // storage; converted to the image's format instead, the blurred scene behind its menus came out blue
    {
        static NVNtexture a10;
        NVNtextureBuilder ab;
        P(PFNNVNTEXTUREBUILDERSETDEFAULTSPROC, "nvnTextureBuilderSetDefaults")(&ab);
        P(PFNNVNTEXTUREBUILDERSETDEVICEPROC, "nvnTextureBuilderSetDevice")(&ab, &dev);
        P(PFNNVNTEXTUREBUILDERSETTARGETPROC, "nvnTextureBuilderSetTarget")(&ab, NVN_TEXTURE_TARGET_2D);
        P(PFNNVNTEXTUREBUILDERSETFORMATPROC, "nvnTextureBuilderSetFormat")(&ab, NVN_FORMAT_RGB10A2);
        P(PFNNVNTEXTUREBUILDERSETSIZE2DPROC, "nvnTextureBuilderSetSize2D")(&ab, 64, 64);
        P(PFNNVNTEXTUREBUILDERSETSTORAGEPROC, "nvnTextureBuilderSetStorage")(&ab, &tex_pool, 0x10000);
        CHECK("an RGB10A2 texture over the BGRA8 one's storage", P(PFNNVNTEXTUREINITIALIZEPROC, "nvnTextureInitialize")(&a10, &ab));
        draw_and_read(0, red);
        NVNcopyRegion r = {0, 0, 0, 64, 64, 1};
        for (int step = 0; step < 2; step++) {
            P(PFNNVNCOMMANDBUFFERBEGINRECORDINGPROC, "nvnCommandBufferBeginRecording")(&cmd);
            if (!step) P(PFNNVNCOMMANDBUFFERCOPYTEXTURETOTEXTUREPROC, "nvnCommandBufferCopyTextureToTexture")(&cmd, &rt, NULL, &r, &a10, NULL, &r, 0);
            else P(PFNNVNCOMMANDBUFFERCLEARTEXTUREPROC, "nvnCommandBufferClearTexture")(&cmd, &a10, NULL, &r, green, NVN_CLEAR_COLOR_MASK_RGBA);
            NVNcommandHandle h = P(PFNNVNCOMMANDBUFFERENDRECORDINGPROC, "nvnCommandBufferEndRecording")(&cmd);
            P(PFNNVNQUEUESUBMITCOMMANDSPROC, "nvnQueueSubmitCommands")(&queue, 1, &h);
            P(PFNNVNQUEUEFINISHPROC, "nvnQueueFinish")(&queue);
            P(PFNNVNTEXTUREREADTEXELSPROC, "nvnTextureReadTexels")(&a10, NULL, &r, texels);
            print("RGB10A2 %s: pixel (8, 8) %x, (56, 56) %x\n", step ? "clear" : "copy", texels[8 * 64 + 8], texels[56 * 64 + 56]);
            if (!step) CHECK("a copy into it converts to RGB10A2 (red 0xc00003ff, blue 0xfff00000)", texels[8 * 64 + 8] == 0xc00003ff && texels[56 * 64 + 56] == 0xfff00000);
            else CHECK("a clear of it is in RGB10A2 (green 0xc00ffc00)", texels[8 * 64 + 8] == 0xc00ffc00);
        }
        // a clear of part of it clears that part only
        NVNcopyRegion part = {16, 16, 0, 8, 8, 1};
        P(PFNNVNCOMMANDBUFFERBEGINRECORDINGPROC, "nvnCommandBufferBeginRecording")(&cmd);
        P(PFNNVNCOMMANDBUFFERCLEARTEXTUREPROC, "nvnCommandBufferClearTexture")(&cmd, &a10, NULL, &part, red, NVN_CLEAR_COLOR_MASK_RGBA);
        NVNcommandHandle h = P(PFNNVNCOMMANDBUFFERENDRECORDINGPROC, "nvnCommandBufferEndRecording")(&cmd);
        P(PFNNVNQUEUESUBMITCOMMANDSPROC, "nvnQueueSubmitCommands")(&queue, 1, &h);
        P(PFNNVNQUEUEFINISHPROC, "nvnQueueFinish")(&queue);
        P(PFNNVNTEXTUREREADTEXELSPROC, "nvnTextureReadTexels")(&a10, NULL, &r, texels);
        print("RGB10A2 part clear: pixel (20, 20) %x, (8, 8) %x\n", texels[20 * 64 + 20], texels[8 * 64 + 8]);
        CHECK("a clear of an 8x8 region changes that region only", texels[20 * 64 + 20] == 0xc00003ff && texels[8 * 64 + 8] == 0xc00ffc00);
    }

    // drawing into one slice of a 3D texture, named by the target's view (Breath of the Wild builds its colour-grading
    // table this way, eight targets on eight slices): the draw lands on that slice only
    {
        static NVNtexture vol;
        NVNtextureBuilder vb;
        P(PFNNVNTEXTUREBUILDERSETDEFAULTSPROC, "nvnTextureBuilderSetDefaults")(&vb);
        P(PFNNVNTEXTUREBUILDERSETDEVICEPROC, "nvnTextureBuilderSetDevice")(&vb, &dev);
        P(PFNNVNTEXTUREBUILDERSETTARGETPROC, "nvnTextureBuilderSetTarget")(&vb, NVN_TEXTURE_TARGET_3D);
        P(PFNNVNTEXTUREBUILDERSETFORMATPROC, "nvnTextureBuilderSetFormat")(&vb, NVN_FORMAT_RGBA8);
        P(PFNNVNTEXTUREBUILDERSETSIZE3DPROC, "nvnTextureBuilderSetSize3D")(&vb, 64, 64, 2);
        P(PFNNVNTEXTUREBUILDERSETSTORAGEPROC, "nvnTextureBuilderSetStorage")(&vb, &tex_pool, 0x30000);
        CHECK("a 64x64x2 3D texture", P(PFNNVNTEXTUREINITIALIZEPROC, "nvnTextureInitialize")(&vol, &vb));
        static NVNtextureView sv;
        P(PFNNVNTEXTUREVIEWSETDEFAULTSPROC, "nvnTextureViewSetDefaults")(&sv);
        P(PFNNVNTEXTUREVIEWSETLAYERSPROC, "nvnTextureViewSetLayers")(&sv, 1, 1);
        g_target = &vol, g_target_view = &sv;
        draw_and_read(0, red);
        g_target = &rt, g_target_view = NULL;
        static u32 slab[64 * 64 * 2];
        NVNcopyRegion r = {0, 0, 0, 64, 64, 2};
        P(PFNNVNTEXTUREREADTEXELSPROC, "nvnTextureReadTexels")(&vol, NULL, &r, slab);
        print("3D slices: slice 0 (8, 8) %x, slice 1 (8, 8) %x (56, 56) %x\n", slab[8 * 64 + 8], slab[4096 + 8 * 64 + 8], slab[4096 + 56 * 64 + 56]);
        CHECK("a draw into slice 1 of a 3D texture lands there, and not on slice 0", slab[4096 + 8 * 64 + 8] == rgba8(red) && slab[4096 + 56 * 64 + 56] == rgba8(blue) && slab[8 * 64 + 8] == 0);
    }

    // a vertex buffer bound with a size past the end of its memory pool (games give a buffer's whole size from an offset
    // into it): its vertices are read, not zeros
    g_vbuf_size = 1 << 20;
    draw_and_read(0, red);
    g_vbuf_size = 96;
    CHECK("a vertex buffer whose size runs past its pool still draws", texels[8 * 64 + 8] == rgba8(red));

    // a linear render target in a CPU-visible pool: what the GPU draws reaches its memory, where the CPU reads it
    // (Breath of the Wild's terrain system reads GPU-drawn maps this way)
    {
        static NVNtexture lin;
        NVNtextureBuilder lb;
        P(PFNNVNTEXTUREBUILDERSETDEFAULTSPROC, "nvnTextureBuilderSetDefaults")(&lb);
        P(PFNNVNTEXTUREBUILDERSETDEVICEPROC, "nvnTextureBuilderSetDevice")(&lb, &dev);
        P(PFNNVNTEXTUREBUILDERSETTARGETPROC, "nvnTextureBuilderSetTarget")(&lb, NVN_TEXTURE_TARGET_2D);
        P(PFNNVNTEXTUREBUILDERSETFORMATPROC, "nvnTextureBuilderSetFormat")(&lb, NVN_FORMAT_RGBA8);
        P(PFNNVNTEXTUREBUILDERSETSIZE2DPROC, "nvnTextureBuilderSetSize2D")(&lb, 64, 64);
        P(PFNNVNTEXTUREBUILDERSETFLAGSPROC, "nvnTextureBuilderSetFlags")(&lb, NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET);
        P(PFNNVNTEXTUREBUILDERSETSTRIDEPROC, "nvnTextureBuilderSetStride")(&lb, 256);
        P(PFNNVNTEXTUREBUILDERSETSTORAGEPROC, "nvnTextureBuilderSetStorage")(&lb, &pool, 0x38000);
        CHECK("a 64x64 linear render target in a CPU-visible pool", P(PFNNVNTEXTUREINITIALIZEPROC, "nvnTextureInitialize")(&lin, &lb));
        u32 *mem = (u32 *)(pool_mem + 0x38000);
        mem[8 * 64 + 8] = 0x12345678;
        g_target = &lin;
        draw_and_read(0, red);
        g_target = &rt;
        print("linear target memory: (8, 8) %x, (56, 56) %x\n", mem[8 * 64 + 8], mem[56 * 64 + 56]);
        CHECK("the GPU's drawing reaches a linear render target's memory", mem[8 * 64 + 8] == rgba8(red) && mem[56 * 64 + 56] == rgba8(blue));
    }

    // a uniform buffer updated between two draws of one pass: the second draw reads the new colour, and the buffer holds
    // it afterwards (an update inside a pass is held and copied at the pass's end)
    {
        const float teal[4] = {0, 1, 1, 1};
        g_ub_then = teal;
        draw_and_read(3, yellow);
        g_ub_then = NULL;
        float *ub = (float *)(pool_mem + 0x18000);
        print("after an update between draws: pixel %x, buffer %d %d %d\n", texels[8 * 64 + 8], (int)ub[0], (int)ub[1], (int)ub[2]);
        CHECK("a uniform update between two draws of a pass reaches the second draw", texels[8 * 64 + 8] == rgba8(teal));
        CHECK("... and the buffer holds it afterwards", ub[0] == 0 && ub[1] == 1 && ub[2] == 1);
    }

    // occlusion counting: the samples the triangle covers (about half of 64x64), not 0 (0 told the game everything it
    // tested was hidden)
    {
        u64 *rep = (u64 *)(pool_mem + 0x1F000);
        rep[0] = 12345;
        g_occ_test = 1;
        draw_and_read(0, red);
        g_occ_test = 0;
        print("samples passed: %d\n", (int)rep[0]);
        CHECK("SAMPLES_PASSED counts the triangle's samples", rep[0] > 1500 && rep[0] < 2600);
    }

    // a 2x2 texture written by the CPU (WriteTexels: block-linear storage, deswizzled on upload), sampled nearest
    P(PFNNVNTEXTUREBUILDERSETSIZE2DPROC, "nvnTextureBuilderSetSize2D")(&tb, 2, 2);
    P(PFNNVNTEXTUREBUILDERSETSTORAGEPROC, "nvnTextureBuilderSetStorage")(&tb, &tex_pool, 0x20000);
    CHECK("a 2x2 texture", P(PFNNVNTEXTUREINITIALIZEPROC, "nvnTextureInitialize")(&smp_tex, &tb));
    const u32 quad[4] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff};   // (0,0) red, (1,0) green, (0,1) blue, (1,1) white
    NVNcopyRegion all = {0, 0, 0, 2, 2, 1};
    P(PFNNVNTEXTUREWRITETEXELSPROC, "nvnTextureWriteTexels")(&smp_tex, NULL, &all, quad);
    int tdesc = 0, sdesc = 0, tres = 0, sres = 0;
    PFNNVNDEVICEGETINTEGERPROC geti = P(PFNNVNDEVICEGETINTEGERPROC, "nvnDeviceGetInteger");
    geti(&dev, NVN_DEVICE_INFO_TEXTURE_DESCRIPTOR_SIZE, &tdesc), geti(&dev, NVN_DEVICE_INFO_SAMPLER_DESCRIPTOR_SIZE, &sdesc);
    geti(&dev, NVN_DEVICE_INFO_RESERVED_TEXTURE_DESCRIPTORS, &tres), geti(&dev, NVN_DEVICE_INFO_RESERVED_SAMPLER_DESCRIPTORS, &sres);
    int ntd = tres + 16, nsd = sres + 16;
    CHECK("texture and sampler pools", P(PFNNVNTEXTUREPOOLINITIALIZEPROC, "nvnTexturePoolInitialize")(&tpool, &pool, 0x30000, ntd)
          && P(PFNNVNSAMPLERPOOLINITIALIZEPROC, "nvnSamplerPoolInitialize")(&spool, &pool, 0x30000 + (ntd * tdesc + 0xFFF) / 0x1000 * 0x1000, nsd));
    NVNsamplerBuilder sb;
    P(PFNNVNSAMPLERBUILDERSETDEFAULTSPROC, "nvnSamplerBuilderSetDefaults")(&sb);
    P(PFNNVNSAMPLERBUILDERSETDEVICEPROC, "nvnSamplerBuilderSetDevice")(&sb, &dev);
    P(PFNNVNSAMPLERBUILDERSETMINMAGFILTERPROC, "nvnSamplerBuilderSetMinMagFilter")(&sb, NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST);
    CHECK("a nearest sampler", P(PFNNVNSAMPLERINITIALIZEPROC, "nvnSamplerInitialize")(&sampler, &sb));
    P(PFNNVNTEXTUREPOOLREGISTERTEXTUREPROC, "nvnTexturePoolRegisterTexture")(&tpool, tres + 1, &smp_tex, NULL);
    P(PFNNVNSAMPLERPOOLREGISTERSAMPLERPROC, "nvnSamplerPoolRegisterSampler")(&spool, sres + 1, &sampler);
    tex_handle = P(PFNNVNDEVICEGETTEXTUREHANDLEPROC, "nvnDeviceGetTextureHandle")(&dev, tres + 1, sres + 1);
    const float uv_green[4] = {0.75f, 0.25f, 0, 1}, uv_white[4] = {0.75f, 0.75f, 0, 1};
    CHECK("draw sampling the texture", draw_and_read(2, uv_green));
    print("pixel (8, 8) %x, (56, 56) %x\n", texels[8 * 64 + 8], texels[56 * 64 + 56]);
    CHECK("... at (0.75, 0.25): texel (1, 0), green", texels[8 * 64 + 8] == quad[1]);
    CHECK("... outside: cleared", texels[56 * 64 + 56] == rgba8(blue));
    draw_and_read(2, uv_white);
    CHECK("... at (0.75, 0.75): texel (1, 1), white", texels[8 * 64 + 8] == quad[3]);

    // a view's channel mapping replaces the texture's (it does not apply on top of it): the same storage built with the
    // mapping (0, 0, 0, 1), registered with a view mapping (R, G, B, A), samples the texture's own colours
    {
        static NVNtexture swz_tex;
        NVNtextureBuilder sb2 = tb;
        P(PFNNVNTEXTUREBUILDERSETSWIZZLEPROC, "nvnTextureBuilderSetSwizzle")(&sb2, NVN_TEXTURE_SWIZZLE_ZERO, NVN_TEXTURE_SWIZZLE_ZERO, NVN_TEXTURE_SWIZZLE_ZERO, NVN_TEXTURE_SWIZZLE_ONE);
        P(PFNNVNTEXTUREINITIALIZEPROC, "nvnTextureInitialize")(&swz_tex, &sb2);
        NVNtextureView v;
        P(PFNNVNTEXTUREVIEWSETDEFAULTSPROC, "nvnTextureViewSetDefaults")(&v);
        P(PFNNVNTEXTUREVIEWSETSWIZZLEPROC, "nvnTextureViewSetSwizzle")(&v, NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_A);
        P(PFNNVNTEXTUREPOOLREGISTERTEXTUREPROC, "nvnTexturePoolRegisterTexture")(&tpool, tres + 2, &swz_tex, &v);
        NVNtextureHandle keep = tex_handle;
        tex_handle = P(PFNNVNDEVICEGETTEXTUREHANDLEPROC, "nvnDeviceGetTextureHandle")(&dev, tres + 2, sres + 1);
        draw_and_read(2, uv_green);
        print("texture mapping (0 0 0 1), view mapping (R G B A): pixel (8, 8) %x\n", texels[8 * 64 + 8]);
        CHECK("a view's channel mapping replaces the texture's", texels[8 * 64 + 8] == quad[1]);
        tex_handle = keep;
    }

    // 16-bit packed formats, read and drawn: NVN names the channels from the low bits (RGB565: red in bits 0-4, BGR5A1:
    // blue in bits 0-4, A1BGR5: alpha in bit 0), so a red texel reads red and a red draw writes red's bits
    {
        static const struct { int fmt; u16 red; const char *name; } pk[] = {
            {NVN_FORMAT_RGB565, 0x001F, "RGB565"}, {NVN_FORMAT_BGR565, 0xF800, "BGR565"}, {NVN_FORMAT_RGB5A1, 0x801F, "RGB5A1"},
            {NVN_FORMAT_BGR5A1, 0xFC00, "BGR5A1"}, {NVN_FORMAT_A1BGR5, 0xF801, "A1BGR5"},
        };
        for (unsigned i = 0; i < sizeof pk / sizeof *pk; i++) {
            static NVNtexture pt[5], pr[5];
            NVNtextureBuilder b2 = tb;
            P(PFNNVNTEXTUREBUILDERSETFORMATPROC, "nvnTextureBuilderSetFormat")(&b2, pk[i].fmt);
            P(PFNNVNTEXTUREBUILDERSETSIZE2DPROC, "nvnTextureBuilderSetSize2D")(&b2, 2, 2);
            P(PFNNVNTEXTUREBUILDERSETSTORAGEPROC, "nvnTextureBuilderSetStorage")(&b2, &tex_pool, 0x24000 + 0x1000 * i);
            P(PFNNVNTEXTUREINITIALIZEPROC, "nvnTextureInitialize")(&pt[i], &b2);
            u16 q[4] = {pk[i].red, pk[i].red, pk[i].red, pk[i].red};
            P(PFNNVNTEXTUREWRITETEXELSPROC, "nvnTextureWriteTexels")(&pt[i], NULL, &all, q);
            P(PFNNVNTEXTUREPOOLREGISTERTEXTUREPROC, "nvnTexturePoolRegisterTexture")(&tpool, tres + 3 + i, &pt[i], NULL);
            NVNtextureHandle keep = tex_handle;
            tex_handle = P(PFNNVNDEVICEGETTEXTUREHANDLEPROC, "nvnDeviceGetTextureHandle")(&dev, tres + 3 + i, sres + 1);
            draw_and_read(2, uv_green);
            tex_handle = keep;
            print("%s red texel %x sampled as %x\n", pk[i].name, pk[i].red, texels[8 * 64 + 8]);
            CHECK("a red texel sampled from a packed 16-bit texture reads red", texels[8 * 64 + 8] == rgba8(red));
            // drawn into: a 64x64 target of the same format, red written, the bits read back
            NVNtextureBuilder b3 = tb;
            P(PFNNVNTEXTUREBUILDERSETFORMATPROC, "nvnTextureBuilderSetFormat")(&b3, pk[i].fmt);
            P(PFNNVNTEXTUREBUILDERSETSIZE2DPROC, "nvnTextureBuilderSetSize2D")(&b3, 64, 64);
            P(PFNNVNTEXTUREBUILDERSETSTORAGEPROC, "nvnTextureBuilderSetStorage")(&b3, &tex_pool, 0x8000);
            P(PFNNVNTEXTUREINITIALIZEPROC, "nvnTextureInitialize")(&pr[i], &b3);
            g_target = &pr[i];
            draw_and_read(0, red);
            g_target = &rt;
            static u16 px[64 * 64];
            NVNcopyRegion r64 = {0, 0, 0, 64, 64, 1};
            P(PFNNVNTEXTUREREADTEXELSPROC, "nvnTextureReadTexels")(&pr[i], NULL, &r64, px);
            print("%s: a red draw wrote %x\n", pk[i].name, px[8 * 64 + 8]);
            CHECK("a red draw into a packed 16-bit target writes red's bits", px[8 * 64 + 8] == pk[i].red);
        }
    }

    // Drawn into, then read by texel position and size (the resolution scale, NXL_RES_SCALE: the target drawn into
    // is made larger, and the shaders reading it must still see the game's pixels and size). A 256x128 target: its left
    // half drawn red (the right half stays 0); then fetched at 8x the fragment position of a 64x64 target, and its size
    // asked. Scaled 2x and read without the rescaling pass, texel 164 would be texel 82: red instead of 0
    {
        static NVNtexture big;
        NVNtextureBuilder bb = tb;
        P(PFNNVNTEXTUREBUILDERSETFORMATPROC, "nvnTextureBuilderSetFormat")(&bb, NVN_FORMAT_RGBA8);
        P(PFNNVNTEXTUREBUILDERSETSIZE2DPROC, "nvnTextureBuilderSetSize2D")(&bb, 256, 128);
        P(PFNNVNTEXTUREBUILDERSETSTORAGEPROC, "nvnTextureBuilderSetStorage")(&bb, &tex_pool, 0x40000);
        CHECK("a 256x128 target", P(PFNNVNTEXTUREINITIALIZEPROC, "nvnTextureInitialize")(&big, &bb));
        g_target = &big, g_vp_w = 128, g_vp_h = 128, g_full = 1;
        draw_and_read(0, red);   // x < 128 red, the rest untouched (0)
        g_target = &rt, g_vp_w = 64, g_vp_h = 64;
        P(PFNNVNTEXTUREPOOLREGISTERTEXTUREPROC, "nvnTexturePoolRegisterTexture")(&tpool, tres + 9, &big, NULL);
        NVNtextureHandle keep = tex_handle;
        tex_handle = P(PFNNVNDEVICEGETTEXTUREHANDLEPROC, "nvnDeviceGetTextureHandle")(&dev, tres + 9, sres + 1);
        draw_and_read(4, red);
        print("texel fetch at 8x the position: pixel (8, 4) %x (texel 68), (20, 4) %x (texel 164)\n", texels[4 * 64 + 8], texels[4 * 64 + 20]);
        CHECK("a texel fetch reads the drawn texels by the game's coordinates", texels[4 * 64 + 8] == rgba8(red) && texels[4 * 64 + 20] == 0);
        draw_and_read(5, red);
        u32 sz = texels[4 * 64 + 8];
        print("size query: pixel %x (256 / 512 and 128 / 512)\n", sz);
        CHECK("a size query gives the game's size (256x128)", (sz & 0xff) >= 127 && (sz & 0xff) <= 128 && (sz >> 8 & 0xff) >= 63 && (sz >> 8 & 0xff) <= 64);
        // read by the CPU, the target is at the game's size
        static u32 bigpx[256 * 2];
        NVNcopyRegion row2 = {0, 60, 0, 256, 2, 1};
        P(PFNNVNTEXTUREREADTEXELSPROC, "nvnTextureReadTexels")(&big, NULL, &row2, bigpx);
        print("read by the CPU: (100, 60) %x, (200, 60) %x\n", bigpx[100], bigpx[200]);
        CHECK("read by the CPU at the game's size", bigpx[100] == rgba8(red) && bigpx[200] == 0);
        tex_handle = keep, g_full = 0;
    }

    tex_tb = tb, tex_tres = tres;
    Handle tt;
    CHECK("a thread finalizing and remaking the sampled texture", !svcCreateThread(&tt, (void *)tex_churn, NULL, tex_churn_stack + sizeof tex_churn_stack, 44, 1) && !svcStartThread(tt));
    int tdraws = 0;
    while (!tex_churn_done || tdraws < 10) { draw_and_read(2, uv_green); tdraws++; }
    P(PFNNVNTEXTUREWRITETEXELSPROC, "nvnTextureWriteTexels")(&smp_tex, NULL, &all, quad);
    draw_and_read(2, uv_green);
    print("%d draws while the texture was remade 300 times\n", tdraws);
    CHECK("... no crash, and afterwards it samples right", texels[8 * 64 + 8] == quad[1]);

    Handle th;
    CHECK("a thread finalizing pools", !svcCreateThread(&th, (void *)churn, NULL, churn_stack + sizeof churn_stack, 44, 1) && !svcStartThread(th));
    int draws = 0;
    while (!churn_done || draws < 20) { draw_and_read(0, red); draws++; }
    print("%d draws while %d pools were made and finalized\n", draws, churn_count);
    CHECK("... while this one draws: no hang, and the last draw is right", churn_count == 200 && texels[8 * 64 + 8] == rgba8(red));
    return 0;
}
