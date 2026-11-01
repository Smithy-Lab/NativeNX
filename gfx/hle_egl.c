// SPDX-License-Identifier: GPL-3.0-or-later
// OpenGL games: EGL and GL on the PC's own driver. A Switch game that draws with OpenGL (or OpenGL ES) calls EGL and GL
// in NVIDIA's driver for the Switch (a module of the game's, subsdk*). Here those calls go to the PC's driver instead,
// through libEGL (loaded when the game first asks for EGL; NXL_EGL_LIBRARY names another): GL's functions take the same
// values on both, so each is a plain bridge (tools/gen_gfxapi.py writes them from gfx/api/gl.sigs and egl.sigs, and the
// game's own driver is not run: src/main.c). What differs is the window, done here:
//   · the display is one of the PC's (a GPU device first, EGL_EXT_platform_device; else Mesa's surfaceless platform;
//     else the default one), whatever native display the game names;
//   · the game's window surface (its vi layer's native window) is a pbuffer of the layer's size (1920x1080 when the game
//     does not size its layer); configurations are chosen with the pbuffer bit in place of the window bit, and say they
//     have both;
//   · eglSwapBuffers on it reads the frame back (into a pixel buffer, shown at the next swap: the GPU is not waited for)
//     and hands it to the Vulkan present (gfx/vk_present.c), which shows it as an NVN game's frame (FSR, the frame rate
//     counter, NativeNX's pop-ups and menu, screenshots); the swap interval paces swaps by the clock (60 Hz);
//   · callbacks the game gives (debug messages, blob caches) are guest code the PC's driver must not call: not passed on
//     (NXL_GL_LOG=1 logs the driver's debug messages instead, with eglGetProcAddress's names).
// eglGetProcAddress gives the bridges' addresses, for the names the PC's driver has.
#define _GNU_SOURCE
#include "hle.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext, *EGLDeviceEXT;
typedef int32_t EGLint;
typedef uint32_t EGLBoolean, EGLenum;
typedef intptr_t EGLAttrib;
enum {
    EGL_SUCCESS = 0x3000, EGL_BAD_SURFACE = 0x300D, EGL_NONE = 0x3038, EGL_SURFACE_TYPE = 0x3033, EGL_PBUFFER_BIT = 0x1, EGL_WINDOW_BIT = 0x4,
    EGL_WIDTH = 0x3057, EGL_HEIGHT = 0x3056, EGL_NATIVE_RENDERABLE = 0x302D, EGL_NATIVE_VISUAL_TYPE = 0x302F, EGL_RENDER_BUFFER = 0x3086,
    EGL_GL_COLORSPACE = 0x309D, EGL_EXTENSIONS = 0x3055, EGL_VENDOR = 0x3053, EGL_PLATFORM_DEVICE_EXT = 0x313F,
    EGL_PLATFORM_SURFACELESS_MESA = 0x31DD,
};
enum {
    GL_VERSION = 0x1F02, GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401, GL_PACK_ROW_LENGTH = 0x0D02, GL_PACK_SKIP_ROWS = 0x0D03,
    GL_PACK_SKIP_PIXELS = 0x0D04, GL_PACK_ALIGNMENT = 0x0D05, GL_FRAMEBUFFER = 0x8D40, GL_FRAMEBUFFER_BINDING = 0x8CA6,
    GL_READ_FRAMEBUFFER = 0x8CA8, GL_READ_FRAMEBUFFER_BINDING = 0x8CAA, GL_PIXEL_PACK_BUFFER = 0x88EB, GL_PIXEL_PACK_BUFFER_BINDING = 0x88ED,
    GL_STREAM_READ = 0x88E1, GL_MAP_READ_BIT = 0x1,
};

static int g_log = -1;
static int gl_log(void) { if (g_log < 0) { const char *e = getenv("NXL_GL_LOG"); g_log = e && atoi(e); } return g_log; }
#define GLOG(...) do { fprintf(stderr, "[gl] " __VA_ARGS__); fputc('\n', stderr); } while (0)

// ---------- the PC's libEGL ----------
static void *g_lib;
static void *(*e_GetProcAddress)(const char *);
static pthread_once_t g_lib_once = PTHREAD_ONCE_INIT;
static void lib_load(void) {
    const char *n = getenv("NXL_EGL_LIBRARY");
    g_lib = dlopen(n && *n ? n : "libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!g_lib) { fprintf(stderr, "[gl] [RED] the PC's EGL library (%s) cannot be loaded: %s; an OpenGL game cannot draw (on Fedora: libglvnd-egl)\n", n && *n ? n : "libEGL.so.1", dlerror()); return; }
    e_GetProcAddress = (void *(*)(const char *))dlsym(g_lib, "eglGetProcAddress");
}
// A function of the PC's EGL library (loaded the first time).
static void *egl_sym(const char *name) {
    pthread_once(&g_lib_once, lib_load);
    if (!g_lib) return NULL;
    void *p = dlsym(g_lib, name);
    if (!p && e_GetProcAddress) p = e_GetProcAddress(name);
    return p;
}
static void *gl_sym(const char *name) {
    pthread_once(&g_lib_once, lib_load);
    return e_GetProcAddress ? e_GetProcAddress(name) : NULL;
}
#define HOST(ret, name, ...) static ret (*name)(__VA_ARGS__)
#define BIND(var, sym) (var ? 1 : (*(void **)&var = egl_sym(sym)) != NULL)

// the bridges' host functions (tools/gen_gfxapi.py's thunks call this on their first call)
void *nxl_vk_proc(const char *name, int *final);   // hle_vk.c
static uint64_t gfx_missing(void) { return 0; }
void *nxl_gfx_proc(int api, const char *name, void **slot);
// The PC's function behind an OpenGL, EGL or Vulkan import of the game, looked up the first time it is called.
void *nxl_gfx_proc(int api, const char *name, void **slot) {
    int final = 1;
    void *p = api == 0 ? gl_sym(name) : api == 1 ? egl_sym(name) : nxl_vk_proc(name, &final);
    if (!p) {
        static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
        static int n;
        pthread_mutex_lock(&mu);
        if (n++ < 40) fprintf(stderr, "[%s] the game called %s, which the PC's driver does not have: it does nothing and returns 0\n", api == 2 ? "vk" : "gl", name);
        pthread_mutex_unlock(&mu);
        p = (void *)gfx_missing;
    }
    if (final) *slot = p;
    return p;
}

// ---------- the display ----------
HOST(EGLDisplay, e_GetPlatformDisplay, EGLenum, void *, const EGLint *);
HOST(EGLDisplay, e_GetDisplay, void *);
HOST(EGLBoolean, e_Initialize, EGLDisplay, EGLint *, EGLint *);
HOST(EGLBoolean, e_QueryDevices, EGLint, EGLDeviceEXT *, EGLint *);
HOST(const char *, e_QueryDeviceString, EGLDeviceEXT, EGLint);
HOST(const char *, e_QueryString, EGLDisplay, EGLint);
static EGLDisplay g_dpy;
static pthread_once_t g_dpy_once = PTHREAD_ONCE_INIT;
// Opens an EGL display on the PC's GPU (a device, surfaceless or the default platform: NXL_EGL_PLATFORM picks one).
static void dpy_open(void) {
    const char *want = getenv("NXL_EGL_PLATFORM");   // device, surfaceless or default: only that one
    int any = !want || !*want;
    if (!BIND(e_GetDisplay, "eglGetDisplay") || !BIND(e_Initialize, "eglInitialize")) return;
    (void)BIND(e_QueryString, "eglQueryString");
    const char *cext = e_QueryString ? e_QueryString(NULL, EGL_EXTENSIONS) : NULL;   // the client extensions
    if (!BIND(e_GetPlatformDisplay, "eglGetPlatformDisplayEXT")) (void)BIND(e_GetPlatformDisplay, "eglGetPlatformDisplay");
    if ((any || !strcmp(want, "device")) && cext && strstr(cext, "EGL_EXT_platform_device") && e_GetPlatformDisplay &&
        BIND(e_QueryDevices, "eglQueryDevicesEXT") && BIND(e_QueryDeviceString, "eglQueryDeviceStringEXT")) {
        EGLDeviceEXT dev[16];
        EGLint n = 0;
        if (e_QueryDevices(16, dev, &n)) {
            for (int pass = 0; pass < 2 && !g_dpy; pass++)   // a GPU first, then Mesa's software device
                for (EGLint i = 0; i < n && !g_dpy; i++) {
                    const char *x = e_QueryDeviceString(dev[i], EGL_EXTENSIONS);
                    int soft = x && strstr(x, "EGL_MESA_device_software");
                    if (soft != pass) continue;
                    EGLDisplay d = e_GetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, dev[i], NULL);
                    if (d && e_Initialize(d, NULL, NULL)) g_dpy = d;
                }
            if (g_dpy) { fprintf(stderr, "[gl] the PC's EGL: a GPU device of %d (%s)\n", n, e_QueryString(g_dpy, EGL_VENDOR)); return; }
        }
    }
    if ((any || !strcmp(want, "surfaceless")) && cext && strstr(cext, "EGL_MESA_platform_surfaceless") && e_GetPlatformDisplay) {
        EGLDisplay d = e_GetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
        if (d && e_Initialize(d, NULL, NULL)) { g_dpy = d; fprintf(stderr, "[gl] the PC's EGL: surfaceless (%s)\n", e_QueryString(d, EGL_VENDOR)); return; }
    }
    EGLDisplay d = e_GetDisplay(NULL);
    if (d && e_Initialize(d, NULL, NULL)) { g_dpy = d; fprintf(stderr, "[gl] the PC's EGL: the default display (%s)\n", e_QueryString ? e_QueryString(d, EGL_VENDOR) : "?"); return; }
    fprintf(stderr, "[gl] [RED] no EGL display of the PC's could be opened: the game cannot draw\n");
}
static EGLDisplay host_dpy(void) { pthread_once(&g_dpy_once, dpy_open); return g_dpy; }

HLE_FORCE("eglGetDisplay", "OpenGL: one of the PC's EGL displays", EGLDisplay, h_eglGetDisplay, void *native) { (void)native; return host_dpy(); }
HLE_FORCE("eglGetPlatformDisplay", "OpenGL: one of the PC's EGL displays", EGLDisplay, h_eglGetPlatformDisplay, EGLenum platform, void *native, const EGLAttrib *attr) {
    (void)platform; (void)native; (void)attr; return host_dpy();
}
HLE_FORCE("eglGetPlatformDisplayEXT", "OpenGL: one of the PC's EGL displays", EGLDisplay, h_eglGetPlatformDisplayEXT, EGLenum platform, void *native, const EGLint *attr) {
    (void)platform; (void)native; (void)attr; return host_dpy();
}
HLE_FORCE("eglInitialize", "OpenGL: the PC's display (opened already)", EGLBoolean, h_eglInitialize, EGLDisplay d, EGLint *major, EGLint *minor) {
    if (!host_dpy() || !e_Initialize) return 0;
    return e_Initialize(d ? d : g_dpy, major, minor);
}
// the game may end EGL and start it again (SDL does when it remakes its window): the display stays open
HLE_FORCE("eglTerminate", "OpenGL: the PC's display stays open", EGLBoolean, h_eglTerminate, EGLDisplay d) { (void)d; return 1; }

// ---------- configurations: the pbuffer bit for the window bit ----------
HOST(EGLBoolean, e_ChooseConfig, EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
HOST(EGLBoolean, e_GetConfigAttrib, EGLDisplay, EGLConfig, EGLint, EGLint *);
HLE_FORCE("eglChooseConfig", "OpenGL: the game's window is a pbuffer here", EGLBoolean, h_eglChooseConfig, EGLDisplay d, const EGLint *attr, EGLConfig *cfg, EGLint n, EGLint *num) {
    if (!BIND(e_ChooseConfig, "eglChooseConfig")) return 0;
    EGLint a[128];
    int k = 0, had_type = 0;
    for (int i = 0; attr && attr[i] != EGL_NONE && k < 124; i += 2) {
        if (attr[i] == EGL_NATIVE_RENDERABLE || attr[i] == EGL_NATIVE_VISUAL_TYPE) continue;   // no native windows here
        a[k++] = attr[i];
        EGLint v = attr[i + 1];
        if (attr[i] == EGL_SURFACE_TYPE && v != -1) { had_type = 1; if (v & EGL_WINDOW_BIT) v = (v & ~EGL_WINDOW_BIT) | EGL_PBUFFER_BIT; }
        a[k++] = v;
    }
    if (!had_type) a[k++] = EGL_SURFACE_TYPE, a[k++] = EGL_PBUFFER_BIT;   // EGL's default is the window bit
    a[k] = EGL_NONE;
    return e_ChooseConfig(d, a, cfg, n, num);
}
HLE_FORCE("eglGetConfigAttrib", "OpenGL: a pbuffer configuration says it has windows too", EGLBoolean, h_eglGetConfigAttrib, EGLDisplay d, EGLConfig c, EGLint attr, EGLint *v) {
    if (!BIND(e_GetConfigAttrib, "eglGetConfigAttrib")) return 0;
    EGLBoolean r = e_GetConfigAttrib(d, c, attr, v);
    if (r && v && attr == EGL_SURFACE_TYPE && (*v & EGL_PBUFFER_BIT)) *v |= EGL_WINDOW_BIT;
    return r;
}

// ---------- the window: a pbuffer, read back at each swap ----------
HOST(EGLSurface, e_CreatePbufferSurface, EGLDisplay, EGLConfig, const EGLint *);
HOST(EGLBoolean, e_DestroySurface, EGLDisplay, EGLSurface);
HOST(EGLBoolean, e_SwapBuffers, EGLDisplay, EGLSurface);
HOST(EGLContext, e_GetCurrentContext, void);
void dm_vi_layer_size(int *w, int *h);   // nxnative's hle_vi.c: the size the game gave its layer (0 none)
typedef struct { EGLSurface s; int w, h; } win_t;
static win_t g_win[16];
static pthread_mutex_t g_win_mu = PTHREAD_MUTEX_INITIALIZER;
static win_t *win_of(EGLSurface s) {
    for (int i = 0; s && i < 16; i++) if (g_win[i].s == s) return &g_win[i];
    return NULL;
}
// The game's window surface: a pbuffer of the window's size, read back after each frame and shown in nxlinux's window.
static EGLSurface window_make(EGLDisplay d, EGLConfig cfg, const EGLint *attr) {
    if (!BIND(e_CreatePbufferSurface, "eglCreatePbufferSurface")) return NULL;
    int w = 0, h = 0;
    dm_vi_layer_size(&w, &h);
    if (w <= 0 || h <= 0) w = 1920, h = 1080;   // a docked Switch's screen
    const char *e = getenv("NXL_GL_SIZE");      // NXL_GL_SIZE=WxH: another size (the game must not size its layer)
    if (e && sscanf(e, "%dx%d", &w, &h) != 2) w = 1920, h = 1080;
    EGLint a[32] = {EGL_WIDTH, w, EGL_HEIGHT, h};
    int k = 4;
    for (int i = 0; attr && attr[i] != EGL_NONE && k < 28; i += 2)
        if (attr[i] == EGL_GL_COLORSPACE) a[k++] = attr[i], a[k++] = attr[i + 1];   // the rest is a window's (EGL_RENDER_BUFFER ...)
    a[k] = EGL_NONE;
    EGLSurface s = e_CreatePbufferSurface(d, cfg, a);
    if (!s) { fprintf(stderr, "[gl] [RED] the game's window (a %dx%d pbuffer here) could not be made\n", w, h); return NULL; }
    pthread_mutex_lock(&g_win_mu);
    for (int i = 0; i < 16; i++) if (!g_win[i].s) { g_win[i] = (win_t){s, w, h}; break; }
    pthread_mutex_unlock(&g_win_mu);
    fprintf(stderr, "[gl] the game's window: %dx%d\n", w, h);
    return s;
}
HLE_FORCE("eglCreateWindowSurface", "OpenGL: the game's window is a pbuffer read back at each swap", EGLSurface, h_eglCreateWindowSurface, EGLDisplay d, EGLConfig cfg, void *win, const EGLint *attr) {
    (void)win; return window_make(d, cfg, attr);
}
HLE_FORCE("eglCreatePlatformWindowSurface", "OpenGL: the game's window is a pbuffer read back at each swap", EGLSurface, h_eglCreatePlatformWindowSurface, EGLDisplay d, EGLConfig cfg, void *win, const EGLAttrib *attr) {
    (void)win;
    EGLint a[32];
    int k = 0;
    for (int i = 0; attr && attr[i] != EGL_NONE && k < 30; i += 2) a[k++] = (EGLint)attr[i], a[k++] = (EGLint)attr[i + 1];
    a[k] = EGL_NONE;
    return window_make(d, cfg, a);
}
HLE_FORCE("eglCreatePlatformWindowSurfaceEXT", "OpenGL: the game's window is a pbuffer read back at each swap", EGLSurface, h_eglCreatePlatformWindowSurfaceEXT, EGLDisplay d, EGLConfig cfg, void *win, const EGLint *attr) {
    (void)win; return window_make(d, cfg, attr);
}
HLE_FORCE("eglDestroySurface", "OpenGL: the game's window is a pbuffer here", EGLBoolean, h_eglDestroySurface, EGLDisplay d, EGLSurface s) {
    if (!BIND(e_DestroySurface, "eglDestroySurface")) return 0;
    pthread_mutex_lock(&g_win_mu);
    win_t *w = win_of(s);
    if (w) w->s = NULL;
    pthread_mutex_unlock(&g_win_mu);
    return e_DestroySurface(d, s);
}

// reading a frame back: into one of two pixel buffers, mapped one swap later (when the GPU has long finished with it);
// a context without pixel buffers (OpenGL ES 2), or NXL_GL_SYNC_READ=1, reads straight into the present's memory
HOST(void, g_GetIntegerv, uint32_t, int32_t *);
HOST(const uint8_t *, g_GetString, uint32_t);
HOST(void, g_BindFramebuffer, uint32_t, uint32_t);
HOST(void, g_BindBuffer, uint32_t, uint32_t);
HOST(void, g_PixelStorei, uint32_t, int32_t);
HOST(void, g_ReadPixels, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void *);
HOST(void, g_GenBuffers, int32_t, uint32_t *);
HOST(void, g_BufferData, uint32_t, intptr_t, const void *, uint32_t);
HOST(void *, g_MapBufferRange, uint32_t, intptr_t, intptr_t, uint32_t);
HOST(uint8_t, g_UnmapBuffer, uint32_t);
void *vk_cpu_frame_begin(uint32_t w, uint32_t h);   // gfx/vk_present.c
uint64_t vk_cpu_frame_end(int flip);
typedef struct { EGLContext ctx; int es, ver, pbo_ok; uint32_t pbo[2]; int w[2], h[2], have[2], next; } rb_t;   // a context's read-back state
static rb_t g_rb[8];
// The read-back state of a GL context (its pixel buffer objects).
static rb_t *rb_of(EGLContext c) {
    for (int i = 0; i < 8; i++) if (g_rb[i].ctx == c) return &g_rb[i];
    for (int i = 0; i < 8; i++) if (!g_rb[i].ctx) { memset(&g_rb[i], 0, sizeof g_rb[i]); g_rb[i].ctx = c; g_rb[i].ver = -1; return &g_rb[i]; }
    memset(&g_rb[0], 0, sizeof g_rb[0]);
    g_rb[0].ctx = c, g_rb[0].ver = -1;
    return &g_rb[0];
}
// Looks up the GL functions the read-back uses; 1 when they are all there.
static int gl_bind_all(void) {
#define GB(v, n) (v || (*(void **)&v = gl_sym(n)))
    return GB(g_GetIntegerv, "glGetIntegerv") && GB(g_GetString, "glGetString") && GB(g_BindFramebuffer, "glBindFramebuffer") &&
           GB(g_PixelStorei, "glPixelStorei") && GB(g_ReadPixels, "glReadPixels") && (GB(g_BindBuffer, "glBindBuffer"), GB(g_GenBuffers, "glGenBuffers"),
           GB(g_BufferData, "glBufferData"), GB(g_MapBufferRange, "glMapBufferRange"), GB(g_UnmapBuffer, "glUnmapBuffer"), 1);
#undef GB
}
static void copy_out(const uint8_t *src, int w, int h) {   // one frame's pixels to the present
    uint8_t *dst = vk_cpu_frame_begin((uint32_t)w, (uint32_t)h);
    if (dst && src) memcpy(dst, src, (size_t)w * h * 4);
    vk_cpu_frame_end(1);
}
// Reads the frame the game just drew into the window's picture (through pixel buffer objects, a frame behind).
static void read_back(win_t *win) {
    if (!BIND(e_GetCurrentContext, "eglGetCurrentContext") || !gl_bind_all()) return;
    rb_t *r = rb_of(e_GetCurrentContext());
    if (r->ver < 0) {   // what the context has: OpenGL ES or not, its version
        const char *v = (const char *)g_GetString(GL_VERSION);
        r->es = v && !strncmp(v, "OpenGL ES", 9);
        const char *num = v ? (r->es ? v + 10 : v) : "2";
        while (*num && (*num < '0' || *num > '9')) num++;
        r->ver = atoi(num);
        const char *sync = getenv("NXL_GL_SYNC_READ");
        r->pbo_ok = r->ver >= 3 && g_GenBuffers && g_BindBuffer && g_BufferData && g_MapBufferRange && g_UnmapBuffer && !(sync && atoi(sync));
        fprintf(stderr, "[gl] the game's context: %s (frames read back %s)\n", v ? v : "?", r->pbo_ok ? "into pixel buffers" : "straight away");
    }
    int w = win->w, h = win->h, three = r->ver >= 3;
    int32_t fb = 0, pbo = 0, al = 4, rl = 0, sr = 0, sp = 0;
    g_GetIntegerv(three ? GL_READ_FRAMEBUFFER_BINDING : GL_FRAMEBUFFER_BINDING, &fb);
    g_GetIntegerv(GL_PACK_ALIGNMENT, &al);
    if (three) {
        g_GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
        g_GetIntegerv(GL_PACK_ROW_LENGTH, &rl), g_GetIntegerv(GL_PACK_SKIP_ROWS, &sr), g_GetIntegerv(GL_PACK_SKIP_PIXELS, &sp);
        g_PixelStorei(GL_PACK_ROW_LENGTH, 0), g_PixelStorei(GL_PACK_SKIP_ROWS, 0), g_PixelStorei(GL_PACK_SKIP_PIXELS, 0);
    }
    g_PixelStorei(GL_PACK_ALIGNMENT, 4);
    g_BindFramebuffer(three ? GL_READ_FRAMEBUFFER : GL_FRAMEBUFFER, 0);
    if (r->pbo_ok) {
        int k = r->next;
        if (!r->pbo[0]) g_GenBuffers(2, r->pbo);
        g_BindBuffer(GL_PIXEL_PACK_BUFFER, r->pbo[k]);
        if (r->w[k] != w || r->h[k] != h) { g_BufferData(GL_PIXEL_PACK_BUFFER, (intptr_t)w * h * 4, NULL, GL_STREAM_READ); r->w[k] = w, r->h[k] = h; }
        g_ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, NULL);   // queued: the frame stays on the GPU's side for now
        r->have[k] = 1;
        int o = k ^ 1;   // the previous swap's frame
        if (r->have[o]) {
            g_BindBuffer(GL_PIXEL_PACK_BUFFER, r->pbo[o]);
            const uint8_t *p = g_MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (intptr_t)r->w[o] * r->h[o] * 4, GL_MAP_READ_BIT);
            if (p) { copy_out(p, r->w[o], r->h[o]); g_UnmapBuffer(GL_PIXEL_PACK_BUFFER); }
        }
        r->next = o;
        g_BindBuffer(GL_PIXEL_PACK_BUFFER, (uint32_t)pbo);
    } else {
        uint8_t *dst = vk_cpu_frame_begin((uint32_t)w, (uint32_t)h);
        if (three) g_BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        if (dst) g_ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dst);
        if (three) g_BindBuffer(GL_PIXEL_PACK_BUFFER, (uint32_t)pbo);
        vk_cpu_frame_end(1);
    }
    g_BindFramebuffer(three ? GL_READ_FRAMEBUFFER : GL_FRAMEBUFFER, (uint32_t)fb);
    g_PixelStorei(GL_PACK_ALIGNMENT, al);
    if (three) g_PixelStorei(GL_PACK_ROW_LENGTH, rl), g_PixelStorei(GL_PACK_SKIP_ROWS, sr), g_PixelStorei(GL_PACK_SKIP_PIXELS, sp);
}
static _Atomic int g_interval = 1;
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void pace(void) {   // the swap interval: at most 60 / interval swaps a second
    static uint64_t next;
    int n = atomic_load(&g_interval);
    if (n <= 0) return;
    uint64_t period = 1000000000ull * (uint64_t)(n > 4 ? 4 : n) / 60, t = now_ns();
    if (next > t && next - t <= period) { uint64_t w = next - t; struct timespec ts = {(time_t)(w / 1000000000ull), (long)(w % 1000000000ull)}; nanosleep(&ts, NULL); t = now_ns(); }
    next = next && t < next + period ? next + period : t + period;
}
// eglSwapBuffers for the game's window: the frame read back and shown, paced like a console's frame.
static EGLBoolean swap(EGLDisplay d, EGLSurface s) {
    pthread_mutex_lock(&g_win_mu);
    win_t *w = win_of(s), copy = w ? *w : (win_t){0};
    pthread_mutex_unlock(&g_win_mu);
    if (!w) return BIND(e_SwapBuffers, "eglSwapBuffers") ? e_SwapBuffers(d, s) : 0;
    read_back(&copy);
    pace();
    return 1;
}
HLE_FORCE("eglSwapBuffers", "OpenGL: the frame read back and shown through the Vulkan present", EGLBoolean, h_eglSwapBuffers, EGLDisplay d, EGLSurface s) { return swap(d, s); }
HLE_FORCE("eglSwapBuffersWithDamageKHR", "OpenGL: the frame read back and shown through the Vulkan present", EGLBoolean, h_eglSwapBuffersWithDamageKHR, EGLDisplay d, EGLSurface s, const EGLint *rects, EGLint n) {
    (void)rects; (void)n; return swap(d, s);
}
HLE_FORCE("eglSwapBuffersWithDamageEXT", "OpenGL: the frame read back and shown through the Vulkan present", EGLBoolean, h_eglSwapBuffersWithDamageEXT, EGLDisplay d, EGLSurface s, const EGLint *rects, EGLint n) {
    (void)rects; (void)n; return swap(d, s);
}
HLE_FORCE("eglSwapInterval", "OpenGL: swaps paced by the clock", EGLBoolean, h_eglSwapInterval, EGLDisplay d, EGLint n) {
    (void)d;
    if (atomic_exchange(&g_interval, n) != n) fprintf(stderr, "[gl] swap interval %d\n", n);
    return 1;
}

// ---------- function addresses ----------
HLE_FORCE("eglGetProcAddress", "OpenGL: the bridges to the PC's driver", void *, h_eglGetProcAddress, const char *name) {
    static _Atomic int asked, found;
    if (!name) return NULL;
    uint64_t a = hle_lookup(name, 1);
    if (a && !strncmp(name, "gl", 2) && !gl_sym(name)) a = 0;   // a name the PC's driver lacks: the game sees it is not there
    atomic_fetch_add(&asked, 1);
    if (a) atomic_fetch_add(&found, 1);
    if (gl_log() && !a) GLOG("eglGetProcAddress(%s): not there", name);
    if (gl_log() && atomic_load(&asked) % 500 == 0) GLOG("eglGetProcAddress: %d names asked, %d there", atomic_load(&asked), atomic_load(&found));
    return (void *)(uintptr_t)a;
}

// ---------- callbacks into the game: not passed to the PC's driver ----------
static void dbg_log(uint32_t source, uint32_t type, uint32_t id, uint32_t severity, int32_t length, const char *msg, const void *user) {
    (void)source; (void)id; (void)length; (void)user;
    static _Atomic int n;
    if (atomic_fetch_add(&n, 1) < 200) GLOG("driver: type %#x severity %#x: %s", type, severity, msg);
}
HOST(void, g_DebugMessageCallback, void *, const void *);
static void dbg_cb(const char *which) {
    if (!gl_log()) return;   // the game's callback is guest code: the driver's messages are logged instead, when asked for
    if (!g_DebugMessageCallback) *(void **)&g_DebugMessageCallback = gl_sym(which);
    if (g_DebugMessageCallback) g_DebugMessageCallback((void *)dbg_log, NULL);
}
HLE_FORCE("glDebugMessageCallback", "OpenGL: the game's callback is guest code (NXL_GL_LOG=1 logs the messages)", void, h_glDebugMessageCallback, void *cb, const void *user) { (void)cb; (void)user; dbg_cb("glDebugMessageCallback"); }
HLE_FORCE("glDebugMessageCallbackKHR", "OpenGL: the game's callback is guest code", void, h_glDebugMessageCallbackKHR, void *cb, const void *user) { (void)cb; (void)user; dbg_cb("glDebugMessageCallbackKHR"); }
HLE_FORCE("glDebugMessageCallbackARB", "OpenGL: the game's callback is guest code", void, h_glDebugMessageCallbackARB, void *cb, const void *user) { (void)cb; (void)user; dbg_cb("glDebugMessageCallbackARB"); }
HLE_FORCE("glDebugMessageCallbackAMD", "OpenGL: the game's callback is guest code", void, h_glDebugMessageCallbackAMD, void *cb, void *user) { (void)cb; (void)user; }
HLE_FORCE("glGetVkProcAddrNV", "OpenGL: a host function's address the game cannot call", void *, h_glGetVkProcAddrNV, const char *name) { (void)name; return NULL; }
HLE_FORCE("eglDebugMessageControlKHR", "OpenGL: the game's callback is guest code", EGLint, h_eglDebugMessageControlKHR, void *cb, const EGLAttrib *attr) { (void)cb; (void)attr; return EGL_SUCCESS; }
HLE_FORCE("eglSetBlobCacheFuncsANDROID", "OpenGL: the game's callbacks are guest code", void, h_eglSetBlobCacheFuncsANDROID, EGLDisplay d, void *set, void *get) { (void)d; (void)set; (void)get; }
const int nxl_gfx_bridges = 1;   // src/main.c: the game's GPU driver is not started
