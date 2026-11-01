// SPDX-License-Identifier: GPL-3.0-or-later
// NVN, the Switch's graphics API, served by nxnative's hle_nvn.c on nxlinux's null backend, driven the way a game does:
// nvnBootstrapLoader, then a device, a memory pool, a queue, an event, a sync and a command buffer; a submission whose
// CPU-visible effects (buffer copy, uniform update, buffer clear, timestamp, event signal) are checked after a fence.
#include "nx.h"
#include "nvn.h"   // nxnative's dm/gfx/nvn/include (MIT, reconstructed by h1k421)

PFNNVNGENERICFUNCPTRPROC nvnBootstrapLoader(const char *name);
static PFNNVNDEVICEGETPROCADDRESSPROC get_proc;
#define P(T, name) ((T)get_proc(NULL, name))

static NVNdevice dev;
static NVNmemoryPool pool;
static NVNqueue queue;
static NVNevent ev;
static NVNsync sync_;
static NVNcommandBuffer cmd;
static u8 pool_mem[512 * 1024] __attribute__((aligned(4096)));
static u8 tex_mem[2][2 * 1024 * 1024] __attribute__((aligned(65536)));   // two 640x360 RGBA8 window textures
static NVNmemoryPool tex_pool[2];
static NVNtexture tex[2];
static NVNwindow win;
static NVNsync acq_sync;
static u8 control[16 * 1024] __attribute__((aligned(8)));

int test_main(void) {
    get_proc = (PFNNVNDEVICEGETPROCADDRESSPROC)nvnBootstrapLoader("nvnDeviceGetProcAddress");
    CHECK("nvnBootstrapLoader hands out nvnDeviceGetProcAddress", get_proc != NULL);

    NVNdeviceBuilder db;
    P(PFNNVNDEVICEBUILDERSETDEFAULTSPROC, "nvnDeviceBuilderSetDefaults")(&db);
    P(PFNNVNDEVICEBUILDERSETFLAGSPROC, "nvnDeviceBuilderSetFlags")(&db, 0);
    CHECK("nvnDeviceInitialize", P(PFNNVNDEVICEINITIALIZEPROC, "nvnDeviceInitialize")(&dev, &db));

    NVNmemoryPoolBuilder pb;
    P(PFNNVNMEMORYPOOLBUILDERSETDEFAULTSPROC, "nvnMemoryPoolBuilderSetDefaults")(&pb);
    P(PFNNVNMEMORYPOOLBUILDERSETDEVICEPROC, "nvnMemoryPoolBuilderSetDevice")(&pb, &dev);
    P(PFNNVNMEMORYPOOLBUILDERSETSTORAGEPROC, "nvnMemoryPoolBuilderSetStorage")(&pb, pool_mem, sizeof pool_mem);
    P(PFNNVNMEMORYPOOLBUILDERSETFLAGSPROC, "nvnMemoryPoolBuilderSetFlags")(&pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    CHECK("nvnMemoryPoolInitialize", P(PFNNVNMEMORYPOOLINITIALIZEPROC, "nvnMemoryPoolInitialize")(&pool, &pb));
    NVNbufferAddress base = P(PFNNVNMEMORYPOOLGETBUFFERADDRESSPROC, "nvnMemoryPoolGetBufferAddress")(&pool);
    CHECK("GPU address = CPU address", base == (NVNbufferAddress)pool_mem);

    NVNqueueBuilder qb;
    P(PFNNVNQUEUEBUILDERSETDEFAULTSPROC, "nvnQueueBuilderSetDefaults")(&qb);
    P(PFNNVNQUEUEBUILDERSETDEVICEPROC, "nvnQueueBuilderSetDevice")(&qb, &dev);
    CHECK("nvnQueueInitialize", P(PFNNVNQUEUEINITIALIZEPROC, "nvnQueueInitialize")(&queue, &qb));

    NVNeventBuilder eb;
    P(PFNNVNEVENTBUILDERSETDEFAULTSPROC, "nvnEventBuilderSetDefaults")(&eb);
    P(PFNNVNEVENTBUILDERSETSTORAGEPROC, "nvnEventBuilderSetStorage")(&eb, &pool, 0x1000);
    CHECK("nvnEventInitialize", P(PFNNVNEVENTINITIALIZEPROC, "nvnEventInitialize")(&ev, &eb));
    PFNNVNEVENTGETVALUEPROC ev_value = P(PFNNVNEVENTGETVALUEPROC, "nvnEventGetValue");
    CHECK("a new event reads 0", ev_value(&ev) == 0);
    CHECK("nvnSyncInitialize", P(PFNNVNSYNCINITIALIZEPROC, "nvnSyncInitialize")(&sync_, &dev));

    CHECK("nvnCommandBufferInitialize", P(PFNNVNCOMMANDBUFFERINITIALIZEPROC, "nvnCommandBufferInitialize")(&cmd, &dev));
    P(PFNNVNCOMMANDBUFFERADDCOMMANDMEMORYPROC, "nvnCommandBufferAddCommandMemory")(&cmd, &pool, 0x40000, 0x40000);
    P(PFNNVNCOMMANDBUFFERADDCONTROLMEMORYPROC, "nvnCommandBufferAddControlMemory")(&cmd, control, sizeof control);

    u8 *src = pool_mem + 0x2000, *dst = pool_mem + 0x3000, *ub = pool_mem + 0x4000, *clr = pool_mem + 0x5000;
    for (int i = 0; i < 256; i++) src[i] = (u8)(i * 7 + 3);
    P(PFNNVNCOMMANDBUFFERBEGINRECORDINGPROC, "nvnCommandBufferBeginRecording")(&cmd);
    P(PFNNVNCOMMANDBUFFERCOPYBUFFERTOBUFFERPROC, "nvnCommandBufferCopyBufferToBuffer")(&cmd, base + 0x2000, base + 0x3000, 256, 0);
    P(PFNNVNCOMMANDBUFFERUPDATEUNIFORMBUFFERPROC, "nvnCommandBufferUpdateUniformBuffer")(&cmd, base + 0x4000, 256, 16, 8, "NXLINUX!");
    P(PFNNVNCOMMANDBUFFERCLEARBUFFERPROC, "nvnCommandBufferClearBuffer")(&cmd, base + 0x5000, 64, 0xdeadbeef);
    P(PFNNVNCOMMANDBUFFERREPORTCOUNTERPROC, "nvnCommandBufferReportCounter")(&cmd, NVN_COUNTER_TYPE_TIMESTAMP, base + 0x6000);
    P(PFNNVNCOMMANDBUFFERSIGNALEVENTPROC, "nvnCommandBufferSignalEvent")(&cmd, &ev, NVN_EVENT_SIGNAL_MODE_WRITE, NVN_EVENT_SIGNAL_LOCATION_TOP, 0, 42);
    ((void (*)(NVNcommandBuffer *, const char *))get_proc(NULL, "nvnCommandBufferInsertDebugMarker"))(&cmd, "an empty implementation");
    NVNcommandHandle h = P(PFNNVNCOMMANDBUFFERENDRECORDINGPROC, "nvnCommandBufferEndRecording")(&cmd);
    CHECK("recording a command buffer", h != 0);
    CHECK("nothing has run before the submission", ev_value(&ev) == 0 && dst[5] == 0);

    P(PFNNVNQUEUESUBMITCOMMANDSPROC, "nvnQueueSubmitCommands")(&queue, 1, &h);
    P(PFNNVNQUEUEFENCESYNCPROC, "nvnQueueFenceSync")(&queue, &sync_, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    P(PFNNVNQUEUEFLUSHPROC, "nvnQueueFlush")(&queue);
    NVNsyncWaitResult w = P(PFNNVNSYNCWAITPROC, "nvnSyncWait")(&sync_, 1000000000ull);
    CHECK("nvnSyncWait: the submission completes", w == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || w == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED);

    int same = 1;
    for (int i = 0; i < 256; i++) if (dst[i] != src[i]) same = 0;
    CHECK("CopyBufferToBuffer copied", same);
    CHECK("UpdateUniformBuffer wrote at its offset", ub[16] == 'N' && ub[23] == '!' && ub[15] == 0);
    CHECK("ClearBuffer filled", ((u32 *)clr)[0] == 0xdeadbeef && ((u32 *)clr)[15] == 0xdeadbeef && ((u32 *)clr)[16] == 0);
    u64 *ts = (u64 *)(pool_mem + 0x6000);
    CHECK("ReportCounter wrote a timestamp", ts[0] != 0 && ts[1] == ts[0]);
    CHECK("SignalEvent wrote the event", ev_value(&ev) == 42);

    PFNNVNGENERICFUNCPTRPROC un = get_proc(NULL, "nvnDeviceIsExternalDebuggerAttached");
    CHECK("an unimplemented NVN function has an address", un != NULL);
    CHECK("... and calling it returns 0 instead of stopping", ((NVNboolean(*)(const NVNdevice *))un)(&dev) == 0);
    CHECK("an unknown name has none", get_proc(NULL, "nvnNotARealFunction") == NULL);
    P(PFNNVNQUEUEFINISHPROC, "nvnQueueFinish")(&queue);

    // the frame loop: a window of two display textures, acquired and presented as a game does every frame
    int texok = 1;
    for (int k = 0; k < 2; k++) {
        NVNmemoryPoolBuilder tpb;
        P(PFNNVNMEMORYPOOLBUILDERSETDEFAULTSPROC, "nvnMemoryPoolBuilderSetDefaults")(&tpb);
        P(PFNNVNMEMORYPOOLBUILDERSETDEVICEPROC, "nvnMemoryPoolBuilderSetDevice")(&tpb, &dev);
        P(PFNNVNMEMORYPOOLBUILDERSETSTORAGEPROC, "nvnMemoryPoolBuilderSetStorage")(&tpb, tex_mem[k], sizeof tex_mem[k]);
        P(PFNNVNMEMORYPOOLBUILDERSETFLAGSPROC, "nvnMemoryPoolBuilderSetFlags")(&tpb, NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
        texok &= P(PFNNVNMEMORYPOOLINITIALIZEPROC, "nvnMemoryPoolInitialize")(&tex_pool[k], &tpb);
        NVNtextureBuilder tb;
        P(PFNNVNTEXTUREBUILDERSETDEFAULTSPROC, "nvnTextureBuilderSetDefaults")(&tb);
        P(PFNNVNTEXTUREBUILDERSETDEVICEPROC, "nvnTextureBuilderSetDevice")(&tb, &dev);
        P(PFNNVNTEXTUREBUILDERSETFLAGSPROC, "nvnTextureBuilderSetFlags")(&tb, NVN_TEXTURE_FLAGS_DISPLAY);
        P(PFNNVNTEXTUREBUILDERSETTARGETPROC, "nvnTextureBuilderSetTarget")(&tb, NVN_TEXTURE_TARGET_2D);
        P(PFNNVNTEXTUREBUILDERSETFORMATPROC, "nvnTextureBuilderSetFormat")(&tb, NVN_FORMAT_RGBA8);
        P(PFNNVNTEXTUREBUILDERSETSIZE2DPROC, "nvnTextureBuilderSetSize2D")(&tb, 640, 360);
        size_t need = P(PFNNVNTEXTUREBUILDERGETSTORAGESIZEPROC, "nvnTextureBuilderGetStorageSize")(&tb);
        texok &= need > 640 * 360 * 4 - 1 && need <= sizeof tex_mem[k];
        P(PFNNVNTEXTUREBUILDERSETSTORAGEPROC, "nvnTextureBuilderSetStorage")(&tb, &tex_pool[k], 0);
        texok &= P(PFNNVNTEXTUREINITIALIZEPROC, "nvnTextureInitialize")(&tex[k], &tb);
    }
    CHECK("two display textures (block-linear storage size from nxnative's layout code)", texok);
    NVNwindowBuilder wb;
    NVNtexture *const ts2[2] = {&tex[0], &tex[1]};
    P(PFNNVNWINDOWBUILDERSETDEFAULTSPROC, "nvnWindowBuilderSetDefaults")(&wb);
    P(PFNNVNWINDOWBUILDERSETDEVICEPROC, "nvnWindowBuilderSetDevice")(&wb, &dev);
    P(PFNNVNWINDOWBUILDERSETTEXTURESPROC, "nvnWindowBuilderSetTextures")(&wb, 2, ts2);
    P(PFNNVNWINDOWBUILDERSETPRESENTINTERVALPROC, "nvnWindowBuilderSetPresentInterval")(&wb, 1);
    CHECK("nvnWindowInitialize", P(PFNNVNWINDOWINITIALIZEPROC, "nvnWindowInitialize")(&win, &wb));
    P(PFNNVNSYNCINITIALIZEPROC, "nvnSyncInitialize")(&acq_sync, &dev);
    PFNNVNWINDOWACQUIRETEXTUREPROC acquire = P(PFNNVNWINDOWACQUIRETEXTUREPROC, "nvnWindowAcquireTexture");
    PFNNVNQUEUEPRESENTTEXTUREPROC present = P(PFNNVNQUEUEPRESENTTEXTUREPROC, "nvnQueuePresentTexture");
    PFNNVNSYNCWAITPROC wait = P(PFNNVNSYNCWAITPROC, "nvnSyncWait");
    int frames_ok = 1;
    u64 t0 = 0;
    const int N = 30;
    for (int f = 0; f < N; f++) {
        int idx = -1;
        frames_ok &= acquire(&win, &acq_sync, &idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS && (idx == 0 || idx == 1);
        frames_ok &= wait(&acq_sync, 1000000000ull) <= NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED;
        present(&queue, &win, idx);
        if (f == 5) t0 = svcGetSystemTick();
    }
    double fps = (N - 6) / ((svcGetSystemTick() - t0) / 19200000.0);
    CHECK("30 frames acquired and presented", frames_ok);
    print("frame rate over 24 frames: %d fps\n", (int)(fps + 0.5));
    CHECK("the NVN layer paces presents near 60 fps", fps > 50 && fps < 70);
    return 0;
}
