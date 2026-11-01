// SPDX-License-Identifier: GPL-3.0-or-later
// The Vulkan backend's core: the device, the game's memory pools and the submissions (see vk.h).
//
// Memory pools: NVN puts buffers, textures' storage, shader code and command data in memory pools carved from the game's
// own memory, and GPU addresses are the CPU addresses. Each pool is imported as Vulkan memory (VK_EXT_external_memory_host)
// with one buffer over all of it, so any GPU address is (pool buffer, offset) and what the game writes is what the GPU
// reads, as on the Switch's unified memory. Textures are the exception: they live in Vulkan images (vk_tex.c).
//
// Submissions: a queue submission (mtl_sub_begin .. mtl_sub_commit) is one command buffer. It is submitted and waited
// for at commit, so the GPU has finished when the call returns and the CPU-visible effects of completion (event
// values, counter reports) are written in order right after, as nxnative's Metal completion handlers do. That makes
// fences trivially complete; overlapping the CPU and the GPU is left for later.
#define _GNU_SOURCE
#include "vk.h"
#include "platform.h"
#include "nvn_timestamp.h"
#include <time.h>
#include <stdatomic.h>
#include <sys/stat.h>
static void pcache_open(void);
static void pcache_maybe_save(void);
enum { OCC_MAX = 8192 };   // occlusion queries per submission

vk_ctx g_vk;
int g_vk_log;
static int g_vk_state;   // 0 not tried, 1 up, -1 failed

uint64_t guest_ns(void);
static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint32_t vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_vk.mem.memoryTypeCount; i++)
        if ((bits >> i & 1) && (g_vk.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

const char *const *vk_present_instance_exts(uint32_t *n);   // vk_present.c: what the window needs
static PFN_vkGetMemoryHostPointerPropertiesEXT p_host_props;

static int vk_init(void) {   // (the submission's command buffer comes from g_vk.cpool, used only under g_sub_mu)
    const char *lg = getenv("NXL_VK_LOG");
    g_vk_log = lg ? atoi(lg) : 0;
    uint32_t nwin = 0;
    const char *const *winx = vk_present_instance_exts(&nwin);
    const char *iexts[16];
    uint32_t nie = 0;
    for (uint32_t i = 0; i < nwin && nie < 14; i++) iexts[nie++] = winx[i];
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "nxlinux", 1, "nxlinux", 1, VK_API_VERSION_1_3};
    const char *layers[1];
    uint32_t nl = 0;
    if (getenv("NXL_VK_VALIDATE")) layers[nl++] = "VK_LAYER_KHRONOS_validation";
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, nl, layers, nie, iexts};
    VkResult r = vkCreateInstance(&ici, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && nl) { ici.enabledLayerCount = 0; r = vkCreateInstance(&ici, NULL, &g_vk.inst); }
    if (r != VK_SUCCESS) { VRED("no Vulkan instance (%d): install a Vulkan driver (on Fedora: vulkan-loader and your GPU's driver)", r); return -1; }
    uint32_t nd = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &nd, NULL);
    if (!nd) { VRED("no Vulkan device"); return -1; }
    VkPhysicalDevice *pd = calloc(nd, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &nd, pd);
    // prefer a discrete GPU, then an integrated one, then anything (lavapipe in tests); NXL_VK_DEVICE picks by index
    int best = -1, best_score = -1;
    const char *want = getenv("NXL_VK_DEVICE");
    for (uint32_t i = 0; i < nd; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        int s = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (p.apiVersion < VK_API_VERSION_1_3) s = 0;
        if (want && (uint32_t)atoi(want) == i) s = 10;
        if (s > best_score) best_score = s, best = (int)i;
    }
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &g_vk.mem);
    vkGetPhysicalDeviceFeatures(g_vk.phys, &g_vk.feat);
    g_vk.api = g_vk.props.apiVersion;
    if (g_vk.api < VK_API_VERSION_1_3) { VRED("%s offers Vulkan %u.%u; 1.3 is needed", g_vk.props.deviceName, VK_API_VERSION_MAJOR(g_vk.api), VK_API_VERSION_MINOR(g_vk.api)); return -1; }
    g_vk.nvidia = g_vk.props.vendorID == 0x10DE;

    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ex = calloc(ne ? ne : 1, sizeof *ex);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ex);
    const char *dexts[16];
    uint32_t nde = 0;
    g_vk.has_host_import = has_ext(ex, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    if (g_vk.has_host_import) dexts[nde++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    else { VRED("%s has no VK_EXT_external_memory_host: the game's memory cannot be shared with the GPU", g_vk.props.deviceName); free(ex); return -1; }
    g_vk.has_custom_border = has_ext(ex, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.has_custom_border) dexts[nde++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.has_swapchain = nwin && has_ext(ex, ne, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (g_vk.has_swapchain) dexts[nde++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    g_vk.has_depth_clip = has_ext(ex, ne, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
    if (g_vk.has_depth_clip) dexts[nde++] = VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME;
    g_vk.has_vertex_div = has_ext(ex, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) || has_ext(ex, ne, "VK_KHR_vertex_attribute_divisor");
    if (has_ext(ex, ne, "VK_KHR_vertex_attribute_divisor")) dexts[nde++] = "VK_KHR_vertex_attribute_divisor";
    else if (has_ext(ex, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME)) dexts[nde++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    free(ex);

    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hp};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    g_vk.host_ptr_align = hp.minImportedHostPointerAlignment ? hp.minImportedHostPointerAlignment : 4096;

    // features: what the translated shaders and the replay use, as far as the device has them
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT fcb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, &f11};
    VkPhysicalDeviceDepthClipEnableFeaturesEXT fdc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, &fcb};
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT fvd = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, &fdc};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &fvd};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &f2);
    if (!g_vk.has_custom_border) fdc.pNext = &f11;
    if (!g_vk.has_depth_clip) fvd.pNext = g_vk.has_custom_border ? (void *)&fcb : (void *)&f11;
    if (!g_vk.has_vertex_div) f2.pNext = g_vk.has_depth_clip ? (void *)&fdc : g_vk.has_custom_border ? (void *)&fcb : (void *)&f11;
    if (!f13.dynamicRendering || !f13.synchronization2) { VRED("%s lacks dynamic rendering / synchronization2", g_vk.props.deviceName); return -1; }
    // keep only what is used (some drivers reject features that cost them)
    f2.features.robustBufferAccess = VK_FALSE;
    f2.features.sparseBinding = f2.features.sparseResidencyBuffer = f2.features.sparseResidencyImage2D = f2.features.sparseResidencyImage3D = VK_FALSE;
    f2.features.sparseResidencyAliased = f2.features.sparseResidency2Samples = f2.features.sparseResidency4Samples = VK_FALSE;
    f2.features.sparseResidency8Samples = f2.features.sparseResidency16Samples = VK_FALSE;
    f2.features.inheritedQueries = VK_FALSE;
    f12.bufferDeviceAddressCaptureReplay = f12.bufferDeviceAddressMultiDevice = VK_FALSE;
    f13.robustImageAccess = VK_FALSE;
    g_vk.has_shader_int8 = f12.shaderInt8;
    g_vk.has_storage8 = f12.storageBuffer8BitAccess;
    g_vk.has_storage16 = f11.storageBuffer16BitAccess;
    g_vk.has_float16 = f12.shaderFloat16;
    // vertex / tessellation shaders that pick the layer or viewport (layered passes without a geometry shader); without
    // these the translation drops the write and every layer's draws land on layer 0
    g_vk.has_output_layer = f12.shaderOutputLayer && f12.shaderOutputViewportIndex;
    g_vk.has_astc = f2.features.textureCompressionASTC_LDR;
    g_vk.feat = f2.features;

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++)
        if ((qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { g_vk.qfam = i; break; }
    free(qp);
    if (g_vk.qfam == UINT32_MAX) { VRED("no graphics + compute queue"); return -1; }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, g_vk.qfam, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci, 0, NULL, nde, dexts, NULL};
    r = vkCreateDevice(g_vk.phys, &dci, NULL, &g_vk.dev);
    if (r != VK_SUCCESS) { VRED("vkCreateDevice failed (%d)", r); return -1; }
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    p_host_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &cpi, NULL, &g_vk.cpool));
    pcache_open();
    VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_OCCLUSION, OCC_MAX};
    if (vkCreateQueryPool(g_vk.dev, &qpi, NULL, &g_vk.occ_pool) != VK_SUCCESS) g_vk.occ_pool = VK_NULL_HANDLE;
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VK_CHECK(vkCreateSampler(g_vk.dev, &sci, NULL, &g_vk.nearest));
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    g_vk.has_d24s8 = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    fprintf(stderr, "[nxl] vulkan: %s (Vulkan %u.%u.%u, driver %#x)%s\n", g_vk.props.deviceName, VK_API_VERSION_MAJOR(g_vk.api),
            VK_API_VERSION_MINOR(g_vk.api), VK_API_VERSION_PATCH(g_vk.api), g_vk.props.driverVersion, g_vk.has_astc ? "" : ", ASTC decoded on the CPU");
    vk_draw_init();
    vk_present_init();
    return 0;
}
// ---------- the pipeline cache on disk ----------
// Pipelines a game needs are made when a draw first needs them (a stutter: Breath of the Wild made about 4,500 in a
// 34-minute session, 12.7 s in all, most in the frames where a new effect first showed). The Vulkan pipeline cache is
// kept in $XDG_CACHE_HOME/nxlinux/pipelines-<GPU's cache UUID>.bin (default ~/.cache/nxlinux), loaded at start and saved
// when new pipelines were made (at most every 30 s, on a thread of its own) and at exit, so the next run takes them from
// there. The driver checks the data's header and ignores data from another GPU or driver. NXL_PIPELINE_CACHE=0: none
static char g_pc_path[1024];
static _Atomic int g_pc_saving;
static uint64_t g_pc_saved_n, g_pc_saved_at;
// Where the pipeline cache file is (~/.cache/nxlinux/pipelines-<GPU and driver hash>.bin), unless NXL_PIPELINE_CACHE=0.
static void pcache_path(void) {
    const char *e = getenv("NXL_PIPELINE_CACHE");
    if (e && *e == '0') return;
    char dir[900];
    nxl_user_dir(NXL_DIR_CACHE, dir, sizeof dir);
    char cmd[1000];
    snprintf(cmd, sizeof cmd, "%s", dir);
    for (char *q = cmd + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(cmd, 0755); *q = '/'; }
    mkdir(cmd, 0755);
    char uuid[33];
    for (int i = 0; i < 16; i++) snprintf(uuid + 2 * i, 3, "%02x", g_vk.props.pipelineCacheUUID[i]);
    snprintf(g_pc_path, sizeof g_pc_path, "%s/pipelines-%s.bin", dir, uuid);
}
// Loads the pipeline cache file into a Vulkan pipeline cache (Vulkan checks it fits this GPU and driver).
static void pcache_open(void) {
    pcache_path();
    void *data = NULL;
    size_t n = 0;
    FILE *f = g_pc_path[0] ? fopen(g_pc_path, "rb") : NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && (data = malloc((size_t)sz)) && fread(data, 1, (size_t)sz, f) == (size_t)sz) n = (size_t)sz;
        fclose(f);
    }
    VkPipelineCacheCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, n, data};
    if (vkCreatePipelineCache(g_vk.dev, &pci, NULL, &g_vk.pcache) != VK_SUCCESS) {   // unusable data: start empty
        pci.initialDataSize = 0, pci.pInitialData = NULL, n = 0;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pci, NULL, &g_vk.pcache));
    }
    free(data);
    if (g_pc_path[0]) fprintf(stderr, "[nxl] pipeline cache: %s (%zu KB loaded)\n", g_pc_path, n / 1024);
}
// Writes the pipeline cache to its file (through a temporary file, on a thread of its own).
static void *pcache_write(void *arg) {
    (void)arg;
    size_t n = 0;
    void *data = NULL;
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &n, NULL) == VK_SUCCESS && n && (data = malloc(n)) &&
        vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &n, data) == VK_SUCCESS) {
        char tmp[1100];
        snprintf(tmp, sizeof tmp, "%s.tmp", g_pc_path);
        FILE *f = fopen(tmp, "wb");
        if (f) {   // whole or not at all (closed once, whichever way it went)
            int ok = fwrite(data, 1, n, f) == n;
            ok = fclose(f) == 0 && ok;
            if (ok) rename(tmp, g_pc_path); else unlink(tmp);
        }
    }
    free(data);
    atomic_store(&g_pc_saving, 0);
    return NULL;
}
// after a submission: save when pipelines were made since the last save, at most every 30 s
static void pcache_maybe_save(void) {
    uint64_t passes, draws, pipes, tr, fails;
    if (!g_pc_path[0] || !g_vk.pcache) return;
    vk_draw_stats(&passes, &draws, &pipes, &tr, &fails);
    uint64_t now = nxl_perf_now();
    if (pipes == g_pc_saved_n || now - g_pc_saved_at < 30000000000ull || atomic_exchange(&g_pc_saving, 1)) return;
    g_pc_saved_n = pipes, g_pc_saved_at = now;
    pthread_t t;
    if (pthread_create(&t, NULL, pcache_write, NULL)) atomic_store(&g_pc_saving, 0);
    else pthread_detach(t);
}
void vk_pcache_save_now(void) {   // at exit
    if (!g_pc_path[0] || !g_vk.pcache) return;
    while (atomic_exchange(&g_pc_saving, 1)) { struct timespec ts = {0, 5000000}; nanosleep(&ts, NULL); }
    pcache_write(NULL);
}
// Whether Vulkan is up, starting it the first time it is asked.
int vk_ready(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    if (!g_vk_state) {
        static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&mu);
        if (!g_vk_state) g_vk_state = vk_init() ? -1 : 1;
        pthread_mutex_unlock(&mu);
    }
    (void)once;
    return g_vk_state > 0;
}
void mtl_init(void) { vk_ready(); }

// ---------- memory pools ----------
typedef struct {
    uint64_t lo, hi;          // the imported range (aligned outward)
    uint64_t mem_lo;          // the pool's own start (the key mtl_pool_del gets)
    VkDeviceMemory m;
    VkBuffer b;
    int refs;
} vk_pool;
static vk_pool *g_pools;
static int g_npools, g_cpools;
static pthread_rwlock_t g_pools_mu = PTHREAD_RWLOCK_INITIALIZER;

// Imports a memory pool's host memory as a Vulkan buffer, so the GPU reads the game's memory where it is.
static int pool_import(vk_pool *p) {
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r = p_host_props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)(uintptr_t)p->lo, &hp);
    if (r != VK_SUCCESS) { VRED("host pointer %#llx not importable (%d)", (unsigned long long)p->lo, r); return -1; }
    uint32_t mt = vk_mem_type(hp.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) mt = vk_mem_type(hp.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (mt == UINT32_MAX) { VRED("no memory type for imported host memory"); return -1; }
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)(uintptr_t)p->lo};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, p->hi - p->lo, mt};
    r = vkAllocateMemory(g_vk.dev, &ai, NULL, &p->m);
    if (r != VK_SUCCESS) { VRED("importing the memory pool %#llx..%#llx failed (%d)", (unsigned long long)p->lo, (unsigned long long)p->hi, r); return -1; }
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, p->hi - p->lo,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                                 VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                             VK_SHARING_MODE_EXCLUSIVE};
    VK_CHECK(vkCreateBuffer(g_vk.dev, &bi, NULL, &p->b));
    VK_CHECK(vkBindBufferMemory(g_vk.dev, p->b, p->m, 0));
    return 0;
}

static pthread_mutex_t g_sub_mu, g_queue_mu;
static pthread_t g_sub_thread;
static int g_sub_active;
// the queue idle: every queue use (submit, present, wait) holds g_queue_mu, as Vulkan requires of vkQueueWaitIdle and of
// vkDeviceWaitIdle (vkDeviceWaitIdle from one thread while another submitted is the likely cause of a hang in Super Mario
// Bros. Wonder entering a level: a memory pool finalized while the render thread submitted)
void vk_idle(void) {
    pthread_mutex_lock(&g_queue_mu);
    vkQueueWaitIdle(g_vk.queue);
    pthread_mutex_unlock(&g_queue_mu);
}
// The game made a memory pool: imported (or shared with a pool already covering it).
void mtl_pool_add(const void *mem, uint64_t size) {
    if (!vk_ready() || !mem || !size) return;
    uint64_t a = g_vk.host_ptr_align, lo = (uint64_t)(uintptr_t)mem & ~(a - 1), hi = ((uint64_t)(uintptr_t)mem + size + a - 1) & ~(a - 1);
    pthread_rwlock_wrlock(&g_pools_mu);
    for (int i = 0; i < g_npools; i++)   // already covered (a pool inside another): share it
        if (g_pools[i].lo <= lo && hi <= g_pools[i].hi) {
            if (g_pools[i].mem_lo == (uint64_t)(uintptr_t)mem) g_pools[i].refs++;
            else VLOG(1, "pool %p+%#llx inside the pool at %#llx", mem, (unsigned long long)size, (unsigned long long)g_pools[i].lo);
            pthread_rwlock_unlock(&g_pools_mu);
            return;
        }
    vk_pool p = {lo, hi, (uint64_t)(uintptr_t)mem, VK_NULL_HANDLE, VK_NULL_HANDLE, 1};
    if (pool_import(&p) == 0) {
        if (g_npools == g_cpools) g_pools = realloc(g_pools, (g_cpools = g_cpools ? 2 * g_cpools : 64) * sizeof *g_pools);
        int i = g_npools++;
        while (i > 0 && g_pools[i - 1].lo > lo) { g_pools[i] = g_pools[i - 1]; i--; }   // kept sorted by start
        g_pools[i] = p;
        VLOG(1, "pool %#llx..%#llx imported", (unsigned long long)lo, (unsigned long long)hi);
    }
    pthread_rwlock_unlock(&g_pools_mu);
}
// a pool finalized: its buffer goes once no submission can use it (the one being recorded on another thread finishes
// first; the game itself waits for the GPU's work on a pool before finalizing it)
void mtl_pool_del(const void *mem) {
    if (!vk_ready()) return;
    int own = g_sub_active && pthread_equal(pthread_self(), g_sub_thread);
    if (!own) pthread_mutex_lock(&g_sub_mu);
    pthread_rwlock_wrlock(&g_pools_mu);
    for (int i = 0; i < g_npools; i++)
        if (g_pools[i].mem_lo == (uint64_t)(uintptr_t)mem && --g_pools[i].refs == 0) {
            vk_idle();
            vkDestroyBuffer(g_vk.dev, g_pools[i].b, NULL);
            vkFreeMemory(g_vk.dev, g_pools[i].m, NULL);
            memmove(&g_pools[i], &g_pools[i + 1], (size_t)(g_npools - i - 1) * sizeof *g_pools);
            g_npools--;
            break;
        }
    pthread_rwlock_unlock(&g_pools_mu);
    if (!own) pthread_mutex_unlock(&g_sub_mu);
}
// The imported pool that holds an address range (a binary search, then a few back for overlapping pools).
static int pool_find(uint64_t addr, uint64_t len) {
    int lo = 0, hi = g_npools - 1, best = -1;
    while (lo <= hi) {   // the last pool starting at or below addr
        int mid = (lo + hi) / 2;
        if (g_pools[mid].lo <= addr) best = mid, lo = mid + 1; else hi = mid - 1;
    }
    for (int i = best; i >= 0 && i >= best - 4; i--)   // overlapping imports are rare; look back a little
        if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) return i;
    return -1;
}
// The Vulkan buffer and offset of a guest address range, when it is in an imported pool.
int vk_buf_at(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    pthread_rwlock_rdlock(&g_pools_mu);
    int i = pool_find(addr, len ? len : 1);
    if (i >= 0) *buf = g_pools[i].b, *off = addr - g_pools[i].lo;
    pthread_rwlock_unlock(&g_pools_mu);
    return i >= 0;
}
// the pool holding addr, and how many bytes of it follow addr
int vk_buf_room(uint64_t addr, VkBuffer *buf, VkDeviceSize *off, uint64_t *room) {
    pthread_rwlock_rdlock(&g_pools_mu);
    int i = pool_find(addr, 1);
    if (i >= 0) *buf = g_pools[i].b, *off = addr - g_pools[i].lo, *room = g_pools[i].hi - addr;
    pthread_rwlock_unlock(&g_pools_mu);
    return i >= 0;
}
// The same as vk_buf_at, for nxnative's NVN code.
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    VkBuffer b;
    VkDeviceSize o;
    if (!vk_ready() || !vk_buf_at(addr, len, &b, &o)) return 0;
    *buf = (void *)(uintptr_t)b;
    *off = o;
    return 1;
}
void mtl_alias_drop_pool(const void *pool) { (void)pool; }

// ---------- scratch memory (per submission) ----------
#define SCRATCH_CHUNK (8u << 20)
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *p; VkDeviceSize size, used; } scratch_chunk;
static scratch_chunk *g_scr;
static int g_nscr, g_cscr, g_scr_cur;
// A new host-visible scratch buffer, for copies of data that is not in a pool.
static scratch_chunk scratch_new(VkDeviceSize n) {
    scratch_chunk c = {0};
    c.size = n;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
                             VK_SHARING_MODE_EXCLUSIVE};
    VK_CHECK(vkCreateBuffer(g_vk.dev, &bi, NULL, &c.b));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_vk.dev, c.b, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                               vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, &c.m));
    VK_CHECK(vkBindBufferMemory(g_vk.dev, c.b, c.m, 0));
    VK_CHECK(vkMapMemory(g_vk.dev, c.m, 0, VK_WHOLE_SIZE, 0, (void **)&c.p));
    return c;
}
// Room in the scratch buffers (reset each submission): a pointer to write and its buffer and offset.
void *vk_scratch(VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off) {
    if (align < 16) align = 16;
    for (;;) {
        if (g_scr_cur < g_nscr) {
            scratch_chunk *c = &g_scr[g_scr_cur];
            VkDeviceSize o = (c->used + align - 1) & ~(align - 1);
            if (o + n <= c->size) { c->used = o + n; *buf = c->b; *off = o; return c->p + o; }
            g_scr_cur++;
            continue;
        }
        if (g_nscr == g_cscr) g_scr = realloc(g_scr, (g_cscr = g_cscr ? 2 * g_cscr : 8) * sizeof *g_scr);
        g_scr[g_nscr++] = scratch_new(n > SCRATCH_CHUNK ? (n + 0xFFFF) & ~0xFFFFull : SCRATCH_CHUNK);
    }
}
// Makes the scratch buffers free again (a chunk made for one large request is freed).
static void scratch_reset(void) {
    for (int i = 0; i < g_nscr; i++) g_scr[i].used = 0;
    g_scr_cur = 0;
    // a chunk made for one large request is not kept
    for (int i = 0; i < g_nscr;)
        if (g_scr[i].size > SCRATCH_CHUNK) {
            vkDestroyBuffer(g_vk.dev, g_scr[i].b, NULL);
            vkFreeMemory(g_vk.dev, g_scr[i].m, NULL);
            g_scr[i] = g_scr[--g_nscr];
        } else i++;
}

// ---------- descriptor pools (per submission) ----------
static VkDescriptorPool *g_dpools;
static int g_ndp, g_cdp, g_dp_cur;
// A new descriptor pool.
static VkDescriptorPool dpool_new(void) {
    VkDescriptorPoolSize sz[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024},
    };
    VkDescriptorPoolCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2048, 6, sz};
    VkDescriptorPool p;
    VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &p));
    return p;
}
// A descriptor set from the pools (a new pool when the current one is full).
VkDescriptorSet vk_desc_alloc(VkDescriptorSetLayout l) {
    for (;;) {
        if (g_dp_cur == g_ndp) {
            if (g_ndp == g_cdp) g_dpools = realloc(g_dpools, (g_cdp = g_cdp ? 2 * g_cdp : 4) * sizeof *g_dpools);
            g_dpools[g_ndp++] = dpool_new();
        }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_dpools[g_dp_cur], 1, &l};
        VkDescriptorSet s;
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, &s);
        if (r == VK_SUCCESS) return s;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) { VRED("vkAllocateDescriptorSets: %d", r); return VK_NULL_HANDLE; }
        g_dp_cur++;
    }
}
static void dpools_reset(void) {
    for (int i = 0; i < g_ndp; i++) vkResetDescriptorPool(g_vk.dev, g_dpools[i], 0);
    g_dp_cur = 0;
}

// ---------- deferred destruction (objects the submission in flight may still use) ----------
typedef struct { VkImage im; VkImageView views[16]; int nviews; VkDeviceMemory m; } dead_image;
static dead_image *g_dead;
static int g_ndead, g_cdead;
static pthread_mutex_t g_dead_mu = PTHREAD_MUTEX_INITIALIZER;
// Queues an image (and its views and memory) to be destroyed once the GPU is done with it.
void vk_defer_destroy_image(VkImage im, VkImageView *views, int nviews, VkDeviceMemory m) {
    pthread_mutex_lock(&g_dead_mu);
    if (g_ndead == g_cdead) g_dead = realloc(g_dead, (g_cdead = g_cdead ? 2 * g_cdead : 64) * sizeof *g_dead);
    dead_image d = {im, {0}, nviews > 16 ? 16 : nviews, m};
    memcpy(d.views, views, (size_t)d.nviews * sizeof *views);
    g_dead[g_ndead++] = d;
    pthread_mutex_unlock(&g_dead_mu);
}
// Destroys the queued images (after waiting for the GPU).
static void dead_flush(void) {
    pthread_mutex_lock(&g_dead_mu);
    if (g_ndead) vk_idle();   // a frame being shown (vk_present.c) may still read one of them
    for (int i = 0; i < g_ndead; i++) {
        for (int k = 0; k < g_dead[i].nviews; k++) vkDestroyImageView(g_vk.dev, g_dead[i].views[k], NULL);
        vkDestroyImage(g_vk.dev, g_dead[i].im, NULL);
        vkFreeMemory(g_vk.dev, g_dead[i].m, NULL);
    }
    g_ndead = 0;
    pthread_mutex_unlock(&g_dead_mu);
}

// ---------- submissions ----------
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; uint64_t base; uint32_t *qi, nq; } post_op;   // 1 event, 2 counter, 3 occlusion count
static post_op *g_post;
static int g_npost, g_cpost;
static uint64_t g_fence;
static pthread_mutex_t g_sub_mu = PTHREAD_MUTEX_INITIALIZER;   // held from mtl_sub_begin to mtl_sub_commit, and by the one-off work
static pthread_mutex_t g_done_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER;
static VkCommandBuffer g_cb;   // the segment being recorded (g_cbs[g_seg])
// A submission is handed to the GPU in segments (vk_segment_cut, at a render pass's end every NXL_SEGMENT draws, as
// nxnative's sub_cut): the GPU works on the first while the rest is translated, and the submission's wait at its end
// covers only the last. Breath of the Wild translates a whole frame (about 3,400 draws) in one submission
enum { MAXSEG = 128 };
static VkCommandBuffer g_cbs[MAXSEG];
static VkFence g_fences[MAXSEG];
static int g_seg;
static uint64_t g_stat_segs;
// vkQueueSubmit from two threads at once is not allowed: the submission and the one-off work take this lock
static pthread_mutex_t g_queue_mu = PTHREAD_MUTEX_INITIALIZER;
// Submits a command buffer (the queue is shared by threads: one at a time).
static VkResult queue_submit(VkCommandBuffer cb, VkFence f) {
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
    pthread_mutex_lock(&g_queue_mu);
    VkResult r = vkQueueSubmit(g_vk.queue, 1, &si, f);
    pthread_mutex_unlock(&g_queue_mu);
    return r;
}
void vk_queue_lock(void) { pthread_mutex_lock(&g_queue_mu); }
void vk_queue_unlock(void) { pthread_mutex_unlock(&g_queue_mu); }
static uint64_t g_stat_subs;

static void post(post_op o) {
    if (g_npost == g_cpost) g_post = realloc(g_post, (g_cpost = g_cpost ? 2 * g_cpost : 64) * sizeof *g_post);
    g_post[g_npost++] = o;
}
void vk_post_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0}); }
void vk_post_counter(uint64_t addr, uint32_t type) { post((post_op){2, NULL, 0, 0, addr, type}); }
// NVN's SAMPLES_PASSED counter: occlusion queries, one per render pass segment while it counts (vk_draw.c). They used to
// report 0, so whatever the game culls by them (lights, flares, effects) was taken as hidden
static uint32_t g_occ_used;
int vk_occ_alloc(void) { return g_vk.occ_pool && g_occ_used < OCC_MAX ? (int)g_occ_used++ : -1; }
void vk_post_occlusion(uint64_t addr, uint64_t base, const uint32_t *idx, uint32_t n) {
    uint32_t *c = n ? malloc(n * sizeof *c) : NULL;
    if (n) memcpy(c, idx, n * sizeof *c);
    post((post_op){3, NULL, 0, 0, addr, 1, base, c, n});
}
static uint64_t g_occ_res[OCC_MAX];   // this submission's results, read in one call when it completed
static void occ_read(void) {
    if (g_occ_used && vkGetQueryPoolResults(g_vk.dev, g_vk.occ_pool, 0, g_occ_used, g_occ_used * sizeof *g_occ_res, g_occ_res, sizeof *g_occ_res,
                                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
        memset(g_occ_res, 0, g_occ_used * sizeof *g_occ_res);
}
static uint8_t g_occ_scaled[OCC_MAX];   // the query counted a pass drawn at the resolution scale: its samples / scale²
void vk_occ_mark_scaled(uint32_t q, int on) { if (q < OCC_MAX) g_occ_scaled[q] = (uint8_t)(on != 0); }
// An occlusion query's sample count, in the game's own resolution when the pass was drawn scaled.
uint64_t vk_occ_result(uint32_t q) {
    if (q >= g_occ_used) return 0;
    if (!g_occ_scaled[q]) return g_occ_res[q];
    uint32_t up, shift;
    vk_res_params(&up, &shift);
    return g_occ_res[q] * (1ull << (2 * shift)) / ((uint64_t)up * up);   // the game's own pixel count
}
uint64_t vk_next_fence(void) { return g_fence + 1; }

VkCommandBuffer vk_cmd(void) { return g_sub_active && pthread_equal(pthread_self(), g_sub_thread) ? g_cb : VK_NULL_HANDLE; }
void vk_barrier(void) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb};
    vkCmdPipelineBarrier2(g_cb, &di);
}
void vk_barrier_on(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

unsigned vk_cb_gen; // bumped whenever a command buffer begins: the dynamic state cache keys on it
// Starts command buffer segment i of a submission (made the first time).
static void seg_begin(int i) {
    if (!g_cbs[i]) {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_vk.cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, &g_cbs[i]));
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(g_vk.dev, &fi, NULL, &g_fences[i]));
    }
    g_seg = i, g_cb = g_cbs[i];
    VK_CHECK(vkResetCommandBuffer(g_cb, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(g_cb, &bi));
    vk_cb_gen++;
}
static void cb_begin(void) {
    seg_begin(0);
    if (g_vk.occ_pool) vkCmdResetQueryPool(g_cb, g_vk.occ_pool, 0, OCC_MAX), g_occ_used = 0;
    vk_barrier();   // what the CPU wrote since the last submission (host-coherent memory: a host write is visible at submit)
}
// hand what is recorded so far to the GPU and go on in a new segment (outside a render pass; the caller re-binds
// everything, as every draw does); the barrier at its start orders it after the earlier segments
int vk_segment_cut(void) {
    if (!g_sub_active || g_seg + 1 >= MAXSEG) return 0;
    VK_CHECK(vkEndCommandBuffer(g_cb));
    VK_CHECK(queue_submit(g_cb, g_fences[g_seg]));
    seg_begin(g_seg + 1);
    vk_barrier();
    g_stat_segs++;
    nxl_perf_add(NXL_PERF_SEGMENT, nxl_perf_now());
    return 1;
}
uint64_t vk_stat_segments(void) { return g_stat_segs; }
// Ends and submits the submission's segments and waits for them (reporting a GPU hang after 10 s).
static void cb_submit_wait(void) {
    uint64_t t0 = nxl_perf_now();
    VK_CHECK(vkEndCommandBuffer(g_cb));
    VK_CHECK(queue_submit(g_cb, g_fences[g_seg]));
    VkResult r = vkWaitForFences(g_vk.dev, (uint32_t)g_seg + 1, g_fences, VK_TRUE, 10ull * 1000000000ull);
    if (r == VK_TIMEOUT) { VRED("a submission did not complete in 10 s (GPU hang?)"); vkWaitForFences(g_vk.dev, (uint32_t)g_seg + 1, g_fences, VK_TRUE, UINT64_MAX); }
    else if (r != VK_SUCCESS) VRED("vkWaitForFences: %d (device lost?)", r);
    vkResetFences(g_vk.dev, (uint32_t)g_seg + 1, g_fences);
    scratch_reset();
    dpools_reset();
    dead_flush();
    vk_tex_reap();
    vk_trace_dump_flush();
    nxl_perf_add(NXL_PERF_SUBMIT, t0);
    pcache_maybe_save();
}

static uint64_t g_sub_t0;
// The game's command submission starts: a new command buffer, pending uploads first.
void mtl_sub_begin(void) {
    pthread_mutex_lock(&g_sub_mu);
    g_sub_t0 = nxl_perf_now();
    g_npost = 0;
    if (!vk_ready()) return;
    cb_begin();
    g_sub_thread = pthread_self(), g_sub_active = 1;
    vk_tex_flush_uploads();
    vk_draw_reset();
}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }
// with no Vulkan device: what the game can observe without pixels, as the null backend does (fences, events, counters,
// buffer copies, uniform updates and buffer clears), so a machine without a driver still runs the game headless
static void cpu_exec_list(const uint8_t *p, int depth) {
    if (depth > 16) return;
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        const uint8_t *a = p + 8;
        switch (h.op) {
        case OP_END: return;
        case OP_JUMP: memcpy(&p, a, 8); continue;
        case OP_CALL: { uint64_t t; memcpy(&t, a, 8); cpu_exec_list((const uint8_t *)(uintptr_t)t, depth + 1); break; }
        case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t mode, v; memcpy(&val, a, 8), memcpy(&mode, a + 8, 4), memcpy(&v, a + 12, 4); vk_post_event(val, mode, v); break; }
        case OP_REPORT_COUNTER: { uint64_t t, addr; memcpy(&t, a, 8), memcpy(&addr, a + 8, 8); vk_post_counter(addr, (uint32_t)t); break; }
        case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = vk_next_fence(); s->fenced = 1; break; }
        case OP_COPY_B2B: { uint64_t src, dst, n; memcpy(&src, a, 8), memcpy(&dst, a + 8, 8), memcpy(&n, a + 16, 8); memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n); break; }
        case OP_UPDATE_UB: { uint64_t dst, n; memcpy(&dst, a, 8), memcpy(&n, a + 8, 8); memcpy((void *)(uintptr_t)dst, a + 16, n); break; }
        case OP_CLEAR_BUF: {
            const p_clear_buf *c = (const p_clear_buf *)a;
            uint8_t *d = (uint8_t *)(uintptr_t)c->addr;
            for (uint64_t i = 0; i + 4 <= c->size; i += 4) memcpy(d + i, &c->value, 4);
            break;
        }
        default: break;
        }
        if (h.size < 8) return;
        p += h.size;
    }
}
void mtl_exec(const uint8_t *handle) {
    if (vk_ready()) vk_exec_list(handle, 0);
    else cpu_exec_list(handle, 0);
}
// The submission ends: submitted and waited for, then its fence value is the game's to wait on.
uint64_t mtl_sub_commit(void) {
    if (vk_ready()) {
        vk_draw_end();
        g_sub_active = 0;
        nxl_perf_add(NXL_PERF_REPLAY, g_sub_t0);
        cb_submit_wait();
    }
    uint64_t now = guest_ns();
    uint64_t ticks = nvn_timestamp_from_ns(now ? now : mono_ns());
    if (vk_ready() && g_vk.occ_pool) occ_read();
    for (int i = 0; i < g_npost; i++) {   // the submission is complete: its event and counter writes land, in order
        post_op *o = &g_post[i];
        if (o->kind == 1) { if (o->mode == 1) *o->val += o->v; else *o->val = o->v; }
        else if (o->kind == 3) {
            uint64_t n = o->base;
            for (uint32_t k = 0; k < o->nq; k++) n += vk_occ_result(o->qi[k]);
            uint64_t rec[2] = {n, ticks};
            memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec);
            free(o->qi);
        }
        else { uint64_t rec[2] = {o->type == NVN_COUNTER_TYPE_TIMESTAMP || o->type == 0x10 ? ticks : 0, ticks}; memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec); }
    }
    g_npost = 0;
    if (vk_ready()) vk_occ_fold();
    g_stat_subs++;
    pthread_mutex_lock(&g_done_mu);
    uint64_t f = __atomic_add_fetch(&g_fence, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_done_cv);
    pthread_mutex_unlock(&g_done_mu);
    pthread_mutex_unlock(&g_sub_mu);
    return f;
}
uint64_t vk_stat_subs(void) { return g_stat_subs; }

// one-off work outside a submission (vk_tex.c uploads when a texture is registered): its own command buffer, waited for
static VkCommandBuffer g_once_cb;
static VkFence g_once_fence;
static VkCommandPool g_once_pool;   // a command pool of its own: pools are used from one thread at a time
static pthread_mutex_t g_once_mu = PTHREAD_MUTEX_INITIALIZER;
// A command buffer for a one-off job outside the game's submissions (one at a time).
VkCommandBuffer vk_once_begin(void) {
    pthread_mutex_lock(&g_once_mu);
    if (!g_once_cb) {
        VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_vk.qfam};
        VK_CHECK(vkCreateCommandPool(g_vk.dev, &cpi, NULL, &g_once_pool));
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_once_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, &g_once_cb));
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(g_vk.dev, &fi, NULL, &g_once_fence));
    }
    VK_CHECK(vkResetCommandBuffer(g_once_cb, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(g_once_cb, &bi));
    return g_once_cb;
}
// Submits the one-off command buffer and waits for it.
void vk_once_end(VkCommandBuffer cb) {
    VK_CHECK(vkEndCommandBuffer(cb));
    VK_CHECK(queue_submit(cb, g_once_fence));
    VK_CHECK(vkWaitForFences(g_vk.dev, 1, &g_once_fence, VK_TRUE, UINT64_MAX));
    vkResetFences(g_vk.dev, 1, &g_once_fence);
    pthread_mutex_unlock(&g_once_mu);
}

uint64_t mtl_last_fence(void) { return __atomic_load_n(&g_fence, __ATOMIC_ACQUIRE); }
// Waits for a fence value to be reached (or the timeout); 1 when it was.
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (fence <= mtl_last_fence()) return 1;
    if (!timeout_ns) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns;
    ts.tv_sec += (time_t)(t / 1000000000ull), ts.tv_nsec += (long)(t % 1000000000ull);
    if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
    pthread_mutex_lock(&g_done_mu);
    int r = 0;
    while (!(r = fence <= g_fence))
        if (pthread_cond_timedwait(&g_done_cv, &g_done_mu, &ts)) { r = fence <= g_fence; break; }
    pthread_mutex_unlock(&g_done_mu);
    return r;
}
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns) { return mtl_wait(fence, timeout_ns); }
void mtl_finish(void) { if (vk_ready()) vk_idle(); }
void mtl_drain(void) { mtl_finish(); }
