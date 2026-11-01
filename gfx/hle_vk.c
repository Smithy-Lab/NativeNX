// SPDX-License-Identifier: GPL-3.0-or-later
// Vulkan games: Vulkan on the PC's own driver. A Switch game that draws with Vulkan calls the Vulkan loader and
// NVIDIA's driver for the Switch (modules of the game's, subsdk*); here those calls go to the PC's Vulkan loader, which
// the program links. Vulkan's functions take the same values on both (SPIR-V shaders included), so each is a plain
// bridge (tools/gen_gfxapi.py writes them from gfx/api/vk.sigs) and the game's own loader and driver are not run
// (src/main.c). What differs is done here:
//   · the window: the Switch's surface (vkCreateViSurfaceNN, from VK_NN_vi_surface) is a surface of nxlinux's window
//     (gfx/vk_present.c's window and event thread, so the controller and NativeNX's Home button work), made for the
//     game's instance, to which the game presents itself; without a display, a headless surface where the driver
//     has one (VK_EXT_headless_surface: the tests, with lavapipe);
//   · instance and device extensions: VK_NN_vi_surface is replaced by the PC's surface extensions, and any the PC's
//     driver lacks are left out (logged); no layers (NXL_VK_VALIDATION=1: Khronos' validation layer, if installed);
//     features the PC's GPU lacks are turned off rather than failing the device;
//   · the physical devices are listed with discrete GPUs first (a game takes the first);
//   · allocation callbacks and debug callbacks are guest code the PC's driver must not call: allocations use the
//     driver's own (the bridges pass NULL), debug messages go to nxlinux's log (NXL_VK_LOG=1) instead of the game;
//   · vkGetInstanceProcAddr / vkGetDeviceProcAddr give the bridges' addresses, for the names the PC's driver has.
#define _GNU_SOURCE
#include "hle.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_USE_PLATFORM_VI_NN 1
#include <vulkan/vulkan.h>

#define VKL(...) do { fprintf(stderr, "[vk] " __VA_ARGS__); fputc('\n', stderr); } while (0)
static int vk_log(void) { static int v = -1; if (v < 0) { const char *e = getenv("NXL_VK_LOG"); v = e && atoi(e); } return v; }

static _Atomic(VkInstance) g_inst;   // the game's instance (the last one made)
static int g_headless_surface;        // the instance has VK_EXT_headless_surface for the game's surface

// the bridges' host functions: through the instance (a device's function from it dispatches on the device); before
// there is one only the global functions are found, and a miss is not kept
void *nxl_vk_proc(const char *name, int *final);
// A Vulkan function of the PC's driver for the game (from the game's instance, else the loader); final: no later lookup
// needed.
void *nxl_vk_proc(const char *name, int *final) {
    VkInstance in = atomic_load(&g_inst);
    void *p = (void *)vkGetInstanceProcAddr(in, name);
    if (!p && in) p = (void *)vkGetInstanceProcAddr(NULL, name);
    *final = p || in;
    return p;
}

// is memory the game handed in writable (not in a module's code or read-only data)?
static int writable(const void *p) {
    module_t *m = module_of((uint64_t)(uintptr_t)p);
    return !m || (uint64_t)((const uint8_t *)p - m->base) >= m->data_off;
}

// The game's debug-utils messenger: Vulkan validation messages logged here (the first ones).
static VKAPI_ATTR VkBool32 VKAPI_CALL dbg_utils(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                                const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type; (void)user;
    static _Atomic int n;
    if ((vk_log() || sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) && atomic_fetch_add(&n, 1) < 200) VKL("driver: %s", d && d->pMessage ? d->pMessage : "?");
    return VK_FALSE;
}
// The game's debug-report callback: Vulkan messages logged here (the first ones).
static VKAPI_ATTR VkBool32 VKAPI_CALL dbg_report(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT ot, uint64_t obj, size_t loc, int32_t code,
                                                 const char *layer, const char *msg, void *user) {
    (void)ot; (void)obj; (void)loc; (void)code; (void)layer; (void)user;
    static _Atomic int n;
    if ((vk_log() || (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT)) && atomic_fetch_add(&n, 1) < 200) VKL("driver: %s", msg ? msg : "?");
    return VK_FALSE;
}
// the debug callbacks in a create info's chain: nxlinux's, in place (the chain is the game's memory)
static void chain_callbacks(const void *pnext) {
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)pnext; b; b = b->pNext) {
        if (!writable(b)) { VKL("[RED] a create info's chain is in the game's read-only memory: its debug callbacks cannot be replaced"); return; }
        if (b->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT) ((VkDebugUtilsMessengerCreateInfoEXT *)b)->pfnUserCallback = dbg_utils;
        else if (b->sType == VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT) ((VkDebugReportCallbackCreateInfoEXT *)b)->pfnCallback = dbg_report;
    }
}

// ---------- the instance ----------
static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}
const char *const *vk_present_instance_exts(uint32_t *n);   // vk_present.c: the window's surface extensions
HLE_FORCE("vkEnumerateInstanceExtensionProperties", "Vulkan: the PC's extensions, and the Switch's window surface", VkResult, h_vkEnumerateInstanceExtensionProperties,
          const char *layer, uint32_t *count, VkExtensionProperties *props) {
    if (layer) { *count = 0; return VK_ERROR_LAYER_NOT_PRESENT; }
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n + 1, sizeof *e);
    vkEnumerateInstanceExtensionProperties(NULL, &n, e);
    snprintf(e[n].extensionName, sizeof e[n].extensionName, "%s", VK_NN_VI_SURFACE_EXTENSION_NAME);
    e[n++].specVersion = VK_NN_VI_SURFACE_SPEC_VERSION;
    VkResult r = VK_SUCCESS;
    if (!props) *count = n;
    else {
        if (*count < n) r = VK_INCOMPLETE; else *count = n;
        memcpy(props, e, (size_t)*count * sizeof *e);
    }
    free(e);
    return r;
}
HLE_FORCE("vkEnumerateInstanceLayerProperties", "Vulkan: no layers", VkResult, h_vkEnumerateInstanceLayerProperties, uint32_t *count, VkLayerProperties *props) {
    (void)props; *count = 0; return VK_SUCCESS;
}
HLE_FORCE("vkEnumerateDeviceLayerProperties", "Vulkan: no layers", VkResult, h_vkEnumerateDeviceLayerProperties, VkPhysicalDevice pd, uint32_t *count, VkLayerProperties *props) {
    (void)pd; (void)props; *count = 0; return VK_SUCCESS;
}
HLE_FORCE("vkCreateInstance", "Vulkan: the PC's surface extensions for the Switch's", VkResult, h_vkCreateInstance, const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkInstance *out) {
    (void)alloc;
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    VkExtensionProperties *have = calloc(n ? n : 1, sizeof *have);
    vkEnumerateInstanceExtensionProperties(NULL, &n, have);
    const char *ext[128];
    uint32_t ne = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount && ne < 120; i++) {
        const char *x = ci->ppEnabledExtensionNames[i];
        if (!strcmp(x, VK_NN_VI_SURFACE_EXTENSION_NAME)) continue;
        if (!has_ext(have, n, x)) { VKL("the game asks for the instance extension %s, which the PC's Vulkan lacks: left out", x); continue; }
        ext[ne++] = x;
    }
    uint32_t nw = 0;
    const char *const *win = vk_present_instance_exts(&nw);
    for (uint32_t i = 0; i < nw && ne < 126; i++) {
        int dup = 0;
        for (uint32_t k = 0; k < ne; k++) dup |= !strcmp(ext[k], win[i]);
        if (!dup) ext[ne++] = win[i];
    }
    g_headless_surface = 0;
    if (!nw && has_ext(have, n, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME) && has_ext(have, n, VK_KHR_SURFACE_EXTENSION_NAME)) {   // no display: a headless surface
        int s = 0;
        for (uint32_t k = 0; k < ne; k++) s |= !strcmp(ext[k], VK_KHR_SURFACE_EXTENSION_NAME);
        if (!s) ext[ne++] = VK_KHR_SURFACE_EXTENSION_NAME;
        ext[ne++] = VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME;
        g_headless_surface = 1;
    }
    free(have);
    VkInstanceCreateInfo c = *ci;
    c.enabledExtensionCount = ne, c.ppEnabledExtensionNames = ext;
    const char *val = getenv("NXL_VK_VALIDATION"), *layers[1] = {"VK_LAYER_KHRONOS_validation"};
    c.enabledLayerCount = 0, c.ppEnabledLayerNames = NULL;
    if (val && atoi(val)) c.enabledLayerCount = 1, c.ppEnabledLayerNames = layers;
    chain_callbacks(c.pNext);
    const VkApplicationInfo *ai = ci->pApplicationInfo;
    VkResult r = vkCreateInstance(&c, NULL, out);
    if (r == VK_SUCCESS) atomic_store(&g_inst, *out);
    VKL("the game's instance%s%s (Vulkan %u.%u asked): %s (%d)", ai && ai->pEngineName ? ", engine " : "", ai && ai->pEngineName ? ai->pEngineName : "",
        ai ? VK_API_VERSION_MAJOR(ai->apiVersion) : 1, ai ? VK_API_VERSION_MINOR(ai->apiVersion) : 0, r == VK_SUCCESS ? "made" : "failed", r);
    return r;
}

// discrete GPUs first, then integrated ones, then the rest (a software one last)
static int dev_rank(VkPhysicalDevice p) {
    VkPhysicalDeviceProperties pr;
    vkGetPhysicalDeviceProperties(p, &pr);
    return pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 0 : pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 :
           pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 3 : 2;
}
HLE_FORCE("vkEnumeratePhysicalDevices", "Vulkan: discrete GPUs first", VkResult, h_vkEnumeratePhysicalDevices, VkInstance inst, uint32_t *count, VkPhysicalDevice *out) {
    uint32_t n = 0;
    VkResult r = vkEnumeratePhysicalDevices(inst, &n, NULL);
    if (r != VK_SUCCESS) return r;
    if (!out) { *count = n; return VK_SUCCESS; }
    VkPhysicalDevice *d = calloc(n ? n : 1, sizeof *d);
    vkEnumeratePhysicalDevices(inst, &n, d);
    for (uint32_t i = 1; i < n; i++)   // a stable sort by rank
        for (uint32_t k = i; k > 0 && dev_rank(d[k]) < dev_rank(d[k - 1]); k--) { VkPhysicalDevice t = d[k]; d[k] = d[k - 1]; d[k - 1] = t; }
    static int said;
    if (n && !said++) { VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(d[0], &pr); VKL("the game's GPU: %s", pr.deviceName); }
    r = *count < n ? VK_INCOMPLETE : VK_SUCCESS;
    if (*count > n) *count = n;
    memcpy(out, d, (size_t)*count * sizeof *d);
    free(d);
    return r;
}

// ---------- the device ----------
static void features_mask(VkPhysicalDeviceFeatures *f, const VkPhysicalDeviceFeatures *have) {   // what the GPU lacks: off
    VkBool32 *a = (VkBool32 *)f;
    const VkBool32 *h = (const VkBool32 *)have;
    for (size_t i = 0; i < sizeof *f / sizeof(VkBool32); i++)
        if (a[i] && !h[i]) {
            static int n;
            if (n++ < 8) VKL("the game asks for a device feature (number %zu of VkPhysicalDeviceFeatures) the PC's GPU lacks: off", i);
            a[i] = VK_FALSE;
        }
}
HLE_FORCE("vkCreateDevice", "Vulkan: extensions and features the PC's GPU lacks left out", VkResult, h_vkCreateDevice, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *out) {
    (void)alloc;
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *have = calloc(n ? n : 1, sizeof *have);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, have);
    const char **ext = calloc(ci->enabledExtensionCount + 1, sizeof *ext);
    uint32_t ne = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        const char *x = ci->ppEnabledExtensionNames[i];
        if (has_ext(have, n, x)) ext[ne++] = x;
        else VKL("the game asks for the device extension %s, which the PC's GPU lacks: left out", x);
    }
    free(have);
    VkDeviceCreateInfo c = *ci;
    c.enabledExtensionCount = ne, c.ppEnabledExtensionNames = ext;
    c.enabledLayerCount = 0, c.ppEnabledLayerNames = NULL;
    VkPhysicalDeviceFeatures hf, f;
    vkGetPhysicalDeviceFeatures(pd, &hf);
    if (ci->pEnabledFeatures) { f = *ci->pEnabledFeatures; features_mask(&f, &hf); c.pEnabledFeatures = &f; }
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)c.pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 && writable(b)) features_mask(&((VkPhysicalDeviceFeatures2 *)b)->features, &hf);
    VkResult r = vkCreateDevice(pd, &c, NULL, out);
    free(ext);
    VKL("the game's device: %s (%d)", r == VK_SUCCESS ? "made" : "failed", r);
    return r;
}

// ---------- the window ----------
VkSurfaceKHR vk_game_window(VkInstance inst, uint32_t w, uint32_t h);   // vk_present.c
void dm_vi_layer_size(int *w, int *h);                                    // nxnative's hle_vi.c
HLE_FORCE("vkCreateViSurfaceNN", "Vulkan: the Switch's window surface is one of nxlinux's window", VkResult, h_vkCreateViSurfaceNN, VkInstance inst, const VkViSurfaceCreateInfoNN *ci,
          const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
    (void)ci; (void)alloc;
    int w = 0, h = 0;
    dm_vi_layer_size(&w, &h);
    if (w <= 0 || h <= 0) w = 1280, h = 720;
    if (g_headless_surface) {
        PFN_vkCreateHeadlessSurfaceEXT f = (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(inst, "vkCreateHeadlessSurfaceEXT");
        VkHeadlessSurfaceCreateInfoEXT hi = {VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
        VkResult r = f ? f(inst, &hi, NULL, out) : VK_ERROR_EXTENSION_NOT_PRESENT;
        VKL("the game's window: a headless surface (no display): %d", r);
        return r;
    }
    *out = vk_game_window(inst, (uint32_t)w, (uint32_t)h);
    if (!*out) { VKL("[RED] the game's window could not be made"); return VK_ERROR_INITIALIZATION_FAILED; }
    VKL("the game's window: nxlinux's window");
    return VK_SUCCESS;
}

// ---------- debug callbacks: nxlinux's log instead of the game's ----------
HLE_FORCE("vkCreateDebugUtilsMessengerEXT", "Vulkan: the game's callback is guest code (NXL_VK_LOG=1 logs the messages)", VkResult, h_vkCreateDebugUtilsMessengerEXT, VkInstance inst,
          const VkDebugUtilsMessengerCreateInfoEXT *ci, const VkAllocationCallbacks *alloc, VkDebugUtilsMessengerEXT *out) {
    (void)alloc;
    PFN_vkCreateDebugUtilsMessengerEXT f = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(inst, "vkCreateDebugUtilsMessengerEXT");
    if (!f) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkDebugUtilsMessengerCreateInfoEXT c = *ci;
    c.pNext = NULL, c.pfnUserCallback = dbg_utils, c.pUserData = NULL;
    return f(inst, &c, NULL, out);
}
HLE_FORCE("vkCreateDebugReportCallbackEXT", "Vulkan: the game's callback is guest code", VkResult, h_vkCreateDebugReportCallbackEXT, VkInstance inst,
          const VkDebugReportCallbackCreateInfoEXT *ci, const VkAllocationCallbacks *alloc, VkDebugReportCallbackEXT *out) {
    (void)alloc;
    PFN_vkCreateDebugReportCallbackEXT f = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(inst, "vkCreateDebugReportCallbackEXT");
    if (!f) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkDebugReportCallbackCreateInfoEXT c = *ci;
    c.pNext = NULL, c.pfnCallback = dbg_report, c.pUserData = NULL;
    return f(inst, &c, NULL, out);
}

// ---------- function addresses: the bridges' ----------
static int ours(const char *name) {   // names done here that the PC's driver does not have
    return !strcmp(name, "vkCreateViSurfaceNN");
}
static void *proc_addr(const char *name, void *host) {
    static _Atomic int asked, found;
    if (!name) return NULL;
    uint64_t a = hle_lookup(name, 1);
    if (!host && !ours(name)) a = 0;
    atomic_fetch_add(&asked, 1);
    if (a) atomic_fetch_add(&found, 1);
    else if (vk_log()) VKL("vkGet*ProcAddr(%s): not there", name);
    if (vk_log() && atomic_load(&asked) % 200 == 0) VKL("vkGet*ProcAddr: %d names asked, %d there", atomic_load(&asked), atomic_load(&found));
    return (void *)(uintptr_t)a;
}
HLE_FORCE("vkGetInstanceProcAddr", "Vulkan: the bridges to the PC's driver", void *, h_vkGetInstanceProcAddr, VkInstance inst, const char *name) {
    void *host = name ? (void *)vkGetInstanceProcAddr(inst, name) : NULL;
    if (!host && name && inst) host = (void *)vkGetInstanceProcAddr(NULL, name);
    return proc_addr(name, host);
}
HLE_FORCE("vkGetDeviceProcAddr", "Vulkan: the bridges to the PC's driver", void *, h_vkGetDeviceProcAddr, VkDevice dev, const char *name) {
    return proc_addr(name, name ? (void *)vkGetDeviceProcAddr(dev, name) : NULL);
}
