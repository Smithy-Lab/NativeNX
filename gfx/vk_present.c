// SPDX-License-Identifier: GPL-3.0-or-later
// Presentation for the Vulkan backend (see vk.h): the game's swapchain texture is blitted, scaled to fit, into a window
// made with SDL3 (on the presenting thread, which also pumps the window's events); closing the window ends the game.
// Without a display (no DISPLAY / WAYLAND_DISPLAY, or NXL_HEADLESS=1) presents are only counted. NXL_DUMP_FRAMES=<folder>
// writes presented frames as PPM images (every NXL_DUMP_EVERY-th one, default 60), for tests and bug reports.
// NVN's default window origin is GL's lower left, which leaves a frame upside down in memory; the blit flips it.
#define _GNU_SOURCE
#include "vk.h"
#include "platform.h"
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/stat.h>
#ifdef NXL_HAVE_SDL3
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include "nxl_input.h"
#endif

extern int g_origin_lower_left;
void vk_queue_lock(void);
void vk_idle(void);
void vk_queue_unlock(void);

static int g_headless = -1;
static const char *g_dump_dir;
static int g_dump_every = 60;

// the instance extensions a window may need: every surface extension the loader has (SDL then finds what it wants)
const char *const *vk_present_instance_exts(uint32_t *n) {
    static const char *list[8];   // (VK_EXT_headless_surface: SDL's offscreen driver, for tests)
    static uint32_t cnt;
    static int done;
    if (done) { *n = cnt; return list; }
    done = 1;
    const char *dpy = getenv("DISPLAY"), *wl = getenv("WAYLAND_DISPLAY"), *hl = getenv("NXL_HEADLESS");
    g_headless = hl && *hl ? atoi(hl) != 0 : !(dpy && *dpy) && !(wl && *wl);   // NXL_HEADLESS=0 forces a window
#ifndef NXL_HAVE_SDL3
    g_headless = 1;
#endif
    if (g_headless) { *n = 0; return list; }
    uint32_t ne = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL);
    VkExtensionProperties *e = calloc(ne ? ne : 1, sizeof *e);
    vkEnumerateInstanceExtensionProperties(NULL, &ne, e);
    static const char *want[] = {"VK_KHR_surface", "VK_KHR_wayland_surface", "VK_KHR_xlib_surface", "VK_KHR_xcb_surface", "VK_EXT_headless_surface"};
    static char names[5][VK_MAX_EXTENSION_NAME_SIZE];
    for (int w = 0; w < 5; w++)
        for (uint32_t i = 0; i < ne; i++)
            if (!strcmp(e[i].extensionName, want[w])) { snprintf(names[cnt], sizeof names[cnt], "%s", want[w]); list[cnt] = names[cnt]; cnt++; break; }
    free(e);
    if (!cnt) g_headless = 1;
    *n = cnt;
    return list;
}

// Reads the presentation settings once: frames written to a folder (NXL_DUMP_FRAMES, every NXL_DUMP_EVERY), the window
// or none.
void vk_present_init(void) {
    g_dump_dir = getenv("NXL_DUMP_FRAMES");
    const char *ev = getenv("NXL_DUMP_EVERY");
    if (ev && atoi(ev) > 0) g_dump_every = atoi(ev);
    if (g_dump_dir) {
        if (mkdir(g_dump_dir, 0755) && errno != EEXIST) VRED("NXL_DUMP_FRAMES=%s: cannot make the folder (%s): no frames will be written", g_dump_dir, strerror(errno));
        else fprintf(stderr, "[vk] writing the first frame and every %dth to %s (NXL_DUMP_FRAMES)\n", g_dump_every, g_dump_dir);
    }
}

// ---------- counting and timing (hle_nvn.c paces presents by the clock) ----------
#define PRING 256
static double g_pres_s[PRING];
static _Atomic uint64_t g_presents;
static uint64_t mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }

// ---------- dumping a frame ----------
static void dump_frame_to(vk_tex *x, const char *path, int cx, int cy, int cw, int ch, int scale);
static void dump_frame(vk_tex *x, uint64_t n) {
    char path[4096];
    snprintf(path, sizeof path, "%s/frame%06llu.png", g_dump_dir, (unsigned long long)n);
    dump_frame_to(x, path, 0, 0, 0, 0, 1);
}
// a frame (or the rectangle cx, cy, cw x ch of it, scaled down by `scale`) as a picture
static void dump_frame_to(vk_tex *x, const char *path, int cx, int cy, int cw, int ch, int scale) {
    const tex_t *t = vk_tex_desc(x);
    const vk_fmt *f = vk_format(t->d.format);
    if (!f || f->bpb != 4 || f->bw != 1 || f->depth) { VRED_ONCE("frame pictures: the frame's format (NVN %#x) is not a 4-byte colour one: not written", t->d.format); return; }
    uint32_t w, h, d;
    vk_tex_hextent(x, 0, &w, &h, &d);   // the image as it is (at the resolution scale: larger)
    VkDeviceSize sz = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b; VkDeviceMemory m; uint8_t *p;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &b) != VK_SUCCESS) return;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_vk.dev, b, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, &m));
    VK_CHECK(vkBindBufferMemory(g_vk.dev, b, m, 0));
    VkCommandBuffer cb = vk_once_begin();
    VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
    vk_once_end(cb);
    VK_CHECK(vkMapMemory(g_vk.dev, m, 0, VK_WHOLE_SIZE, 0, (void **)&p));
    if (cw <= 0 || ch <= 0) cx = 0, cy = 0, cw = (int)w, ch = (int)h;
    if (cx + cw > (int)w) cw = (int)w - cx;
    if (cy + ch > (int)h) ch = (int)h - cy;
    if (scale < 1) scale = 1;
    int ow = cw / scale, oh = ch / scale;
    int bgr = f->vk == VK_FORMAT_B8G8R8A8_UNORM || f->vk == VK_FORMAT_B8G8R8A8_SRGB;
    uint8_t *img = ow > 0 && oh > 0 ? malloc((size_t)ow * oh * 3) : NULL;
    for (int y = 0; img && y < oh; y++)
        for (int i = 0; i < ow; i++) {
            unsigned acc[3] = {0, 0, 0};
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++) {
                    uint32_t yy = (uint32_t)(cy + y * scale + dy), sy = g_origin_lower_left ? h - 1 - yy : yy;
                    const uint8_t *s = p + ((size_t)sy * w + (size_t)(cx + i * scale + dx)) * 4;
                    acc[0] += s[bgr ? 2 : 0], acc[1] += s[1], acc[2] += s[bgr ? 0 : 2];
                }
            for (int k = 0; k < 3; k++) img[((size_t)y * ow + i) * 3 + k] = (uint8_t)(acc[k] / (unsigned)(scale * scale));
        }
    size_t pl = strlen(path);
    int ok = 0;
    if (img && pl > 4 && !strcmp(path + pl - 4, ".png")) ok = stbi_write_png(path, ow, oh, 3, img, ow * 3) != 0;   // PNG: about a third of the size
    else if (img) {
        FILE *fo = fopen(path, "wb");
        if (fo) { fprintf(fo, "P6\n%d %d\n255\n", ow, oh); ok = fwrite(img, 3, (size_t)ow * oh, fo) == (size_t)ow * oh; ok &= fclose(fo) == 0; }
    }
    if (!ok) VRED_ONCE("frame pictures: cannot write %s (%s)", path, strerror(errno));
    else { static int said; if (!said++) fprintf(stderr, "[vk] first frame picture: %s\n", path); VLOG(1, "frame written to %s", path); }
    free(img);
    vkDestroyBuffer(g_vk.dev, b, NULL);
    vkFreeMemory(g_vk.dev, m, NULL);
}

// ---------- the window ----------
#ifdef NXL_HAVE_SDL3
static SDL_Window *g_win;
static VkSurfaceKHR g_surf;
static VkSwapchainKHR g_sc;
static VkImage g_sc_img[8];
static uint32_t g_sc_n;
static VkExtent2D g_sc_ext;
static VkFormat g_sc_fmt;
static VkSemaphore g_acq[8], g_done[8];
static uint32_t g_frame;
static VkCommandPool g_pool;
static VkCommandBuffer g_cbs[8];
static VkFence g_fences[8];
static int g_win_failed;
static _Atomic int g_shot_req;   // F12: the next frame is saved as a screenshot (shot_save)
void vk_shot_request(void) { atomic_store(&g_shot_req, 1); }   // NativeNX's menu: Take a screenshot
static _Atomic int g_fps_on = -1;   // the frame rate counter (fps_draw): -1 not read from NXL_FPS yet
static int fps_on(void) {
    int v = atomic_load(&g_fps_on);
    if (v < 0) { const char *e = getenv("NXL_FPS"); v = e && atoi(e) ? 1 : 0; atomic_store(&g_fps_on, v); }
    return v;
}

void vk_overlay_swapchain_gone(void);   // vk_overlay.c: the pop-ups over the frame
void vk_overlay_draw(VkCommandBuffer cb, VkImage img, VkFormat fmt, uint32_t w, uint32_t h);
// Destroys the window's swapchain (after the GPU is idle), e.g. when the window changes size.
static void swapchain_destroy(void) {
    if (!g_sc) return;
    vk_idle();
    vk_overlay_swapchain_gone();
    vkDestroySwapchainKHR(g_vk.dev, g_sc, NULL);
    g_sc = VK_NULL_HANDLE;
}
// Makes the window's swapchain at its size in pixels, with the frames' images and the semaphores and fences per frame.
static int swapchain_make(void) {
    VkSurfaceCapabilitiesKHR cap;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_surf, &cap);
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(g_win, &ww, &wh);
    VkExtent2D ext = cap.currentExtent.width != UINT32_MAX ? cap.currentExtent : (VkExtent2D){(uint32_t)ww, (uint32_t)wh};
    if (!ext.width || !ext.height) return -1;   // minimised
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_surf, &nf, NULL);
    VkSurfaceFormatKHR *fs = calloc(nf ? nf : 1, sizeof *fs);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_surf, &nf, fs);
    VkSurfaceFormatKHR sf = fs[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fs[i].format == VK_FORMAT_B8G8R8A8_UNORM || fs[i].format == VK_FORMAT_R8G8B8A8_UNORM) { sf = fs[i]; break; }
    free(fs);
    uint32_t np = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_surf, &np, NULL);
    VkPresentModeKHR pm[8], mode = VK_PRESENT_MODE_FIFO_KHR;
    if (np > 8) np = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_surf, &np, pm);
    for (uint32_t i = 0; i < np; i++) if (pm[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = pm[i];   // hle_nvn.c paces; the present must not block
    uint32_t n = cap.minImageCount + 1;
    if (cap.maxImageCount && n > cap.maxImageCount) n = cap.maxImageCount;
    VkSwapchainKHR old = g_sc;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, g_surf, n, sf.format, sf.colorSpace, ext, 1,
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                                   cap.currentTransform, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, mode, VK_TRUE, old};
    if (!(cap.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) ci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    VkResult r = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_sc);
    if (old) { vk_idle(); vkDestroySwapchainKHR(g_vk.dev, old, NULL); }
    if (r != VK_SUCCESS) { VRED("vkCreateSwapchainKHR failed (%d)", r); g_sc = VK_NULL_HANDLE; return -1; }
    g_sc_n = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_sc, &g_sc_n, g_sc_img);
    g_sc_ext = ext, g_sc_fmt = sf.format;
    return 0;
}
// The window lives on a thread of its own, which handles its events (input, resizes, closing) about every 8 ms whether or
// not the game is showing frames: a window whose events wait for the next frame is "not responding" to the desktop
// during a long load. The presenting thread does the Vulkan side (swapchain, blits) when frames come.
static pthread_mutex_t g_win_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_win_cv = PTHREAD_COND_INITIALIZER;
static int g_win_state;              // 0 not yet, 1 open, -1 failed (under g_win_mu)
static _Atomic int g_win_resized;    // the swapchain is remade at the next frame
static uint32_t g_win_w, g_win_h;
static VkInstance g_win_inst;        // a Vulkan game's own instance, which the window's surface is made for (vk_game_window)
static void win_ready(int st) {
    pthread_mutex_lock(&g_win_mu);
    g_win_state = st;
    pthread_cond_broadcast(&g_win_cv);
    pthread_mutex_unlock(&g_win_mu);
}
// NativeNX's menu over the game (vk_overlay.c draws it): the Home button (Guide on an Xbox pad, PS on a PlayStation
// one) or Shift+Tab asks NativeNX for it, down the pipe NXL_LAUNCHER_FD names ("open W H"); while it is up, the game
// sees nothing pressed and the buttons go to NativeNX instead ("key up", "key south" ...), which draws the menu again
// and sends it. Without NativeNX (no NXL_LAUNCHER_FD) the Home button does nothing
int vk_menu_on(void);
void vk_menu_asked(void);
int vk_overlay_window_cmd(void);
static int g_lfd = -2;
static int launcher_fd(void) {   // NativeNX's end of the pipe (-1: started without NativeNX)
    if (g_lfd == -2) g_lfd = nxl_fd_from_env("NXL_LAUNCHER_FD");   // (src/platform.h)
    return g_lfd;
}
// Sends a line to NativeNX down its pipe (when the game was started from it).
static void launcher_say(const char *fmt, ...) {
    if (launcher_fd() < 0) return;
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    b[n++] = '\n';
    if (write(g_lfd, b, (size_t)n) != n) { close(g_lfd); g_lfd = -1; }   // NativeNX is gone
}
static int g_menu_axis[2];   // the left stick as directions while the menu is up
static int menu_event(const SDL_Event *e) {   // 1: the event was the menu's
    int home = (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) ||
               (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && e->key.scancode == SDL_SCANCODE_TAB && (e->key.mod & SDL_KMOD_SHIFT));
    if (launcher_fd() < 0) return 0;
    if (!vk_menu_on()) {
        if (!home) return 0;
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(g_win, &w, &h);
        void nxl_input_block(int);
        nxl_input_block(1);
        vk_menu_asked();
        launcher_say("open %d %d", w, h);
        fprintf(stderr, "[nxl] NativeNX's menu (the Home button)\n");
        return 1;
    }
    const char *k = NULL;
    if (home) k = "home";
    else if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        static const char *const name[SDL_GAMEPAD_BUTTON_COUNT] = {
            [SDL_GAMEPAD_BUTTON_SOUTH] = "south", [SDL_GAMEPAD_BUTTON_EAST] = "east", [SDL_GAMEPAD_BUTTON_WEST] = "west", [SDL_GAMEPAD_BUTTON_NORTH] = "north",
            [SDL_GAMEPAD_BUTTON_BACK] = "back", [SDL_GAMEPAD_BUTTON_START] = "start", [SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = "lb", [SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = "rb",
            [SDL_GAMEPAD_BUTTON_DPAD_UP] = "up", [SDL_GAMEPAD_BUTTON_DPAD_DOWN] = "down", [SDL_GAMEPAD_BUTTON_DPAD_LEFT] = "left", [SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = "right"};
        if (e->gbutton.button < SDL_GAMEPAD_BUTTON_COUNT) k = name[e->gbutton.button];
        static int told_nintendo;   // once: NativeNX puts A on the right-hand button for these
        if (k && !told_nintendo) { SDL_Gamepad *g = SDL_GetGamepadFromID(e->gbutton.which); if (g && SDL_GetGamepadType(g) >= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO) launcher_say("pad nintendo"), told_nintendo = 1; }
    } else if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
        int ax = e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY, v = e->gaxis.value, d = v > 20000 ? 1 : v < -20000 ? -1 : abs(v) < 12000 ? 0 : g_menu_axis[ax];
        if (d != g_menu_axis[ax]) { g_menu_axis[ax] = d; if (d) k = ax ? (d < 0 ? "up" : "down") : (d < 0 ? "left" : "right"); }
        if (!k) return 1;
    } else if (e->type == SDL_EVENT_KEY_DOWN) {
        switch (e->key.scancode) {
        case SDL_SCANCODE_UP: case SDL_SCANCODE_W: k = "up"; break;
        case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: k = "down"; break;
        case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: k = "left"; break;
        case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: k = "right"; break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: case SDL_SCANCODE_J: k = "enter"; break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: case SDL_SCANCODE_K: k = "esc"; break;
        default: return e->key.scancode != SDL_SCANCODE_F11 && e->key.scancode != SDL_SCANCODE_F12 && e->key.scancode != SDL_SCANCODE_F1;
        }
    } else if (e->type == SDL_EVENT_GAMEPAD_BUTTON_UP || e->type == SDL_EVENT_KEY_UP) return 1;
    else return 0;
    if (k) launcher_say("key %s", k);
    return 1;
}
// The window's thread: SDL starts, the window opens (sized, fullscreen or not), then its events are handled until the
// game ends.
static void *window_main(void *arg) {
    (void)arg;
    if (!SDL_Init(SDL_INIT_VIDEO)) { VRED("SDL_Init: %s", SDL_GetError()); win_ready(-1); return NULL; }
    nxl_input_init();
    const char *title = getenv("NXL_WINDOW_TITLE");
    const char *fs = getenv("NXL_FULLSCREEN");   // the whole screen, at the desktop's resolution (F11 switches)
    g_win = SDL_CreateWindow(title ? title : "nxlinux", (int)(g_win_w ? g_win_w : 1280), (int)(g_win_h ? g_win_h : 720),
                             SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | (fs && atoi(fs) ? SDL_WINDOW_FULLSCREEN : 0));
    if (!g_win) { VRED("SDL_CreateWindow: %s", SDL_GetError()); win_ready(-1); return NULL; }
    if (!SDL_Vulkan_CreateSurface(g_win, g_win_inst ? g_win_inst : g_vk.inst, NULL, &g_surf)) { VRED("SDL_Vulkan_CreateSurface: %s", SDL_GetError()); win_ready(-1); return NULL; }
    win_ready(1);
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 8)) {
            do {
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    fprintf(stderr, "[nxl] window closed: exiting\n");
                    vk_pcache_save_now();
                    fflush(stderr);
                    _exit(0);
                }
                if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) atomic_store(&g_win_resized, 1);
                if (menu_event(&e)) continue;   // NativeNX's menu: the Home button, and the buttons while it is up
                if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F11) {   // full screen on / off
                    SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
                    atomic_store(&g_win_resized, 1);
                }
                if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F12) atomic_store(&g_shot_req, 1);   // a screenshot
                if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F1) {   // the frame rate counter on / off
                    atomic_store(&g_fps_on, !fps_on());
                    fprintf(stderr, "[nxl] frame rate counter %s (F1)\n", atomic_load(&g_fps_on) ? "on" : "off");
                }
                nxl_input_event(&e);
            } while (SDL_PollEvent(&e));
        }
        nxl_input_tick();
        int c = vk_overlay_window_cmd();   // NativeNX: the window out of the way while the game waits at its Home, and back
        if (c == 1) SDL_MinimizeWindow(g_win);
        if (c == 2) { SDL_RestoreWindow(g_win); SDL_RaiseWindow(g_win); }
    }
    return NULL;
}
static int window_thread(uint32_t w, uint32_t h) {   // the window and its thread (once): 1 open, -1 failed
    pthread_mutex_lock(&g_win_mu);
    if (!g_win_state) {
        g_win_w = w, g_win_h = h;
        pthread_t t;
        if (pthread_create(&t, NULL, window_main, NULL)) g_win_state = -1;
        else pthread_detach(t);
        while (!g_win_state) pthread_cond_wait(&g_win_cv, &g_win_mu);
    }
    int st = g_win_state;
    pthread_mutex_unlock(&g_win_mu);
    return st;
}
// A Vulkan game's window (gfx/hle_vk.c, its vkCreateViSurfaceNN): the same window and event thread, its surface made
// for the game's instance, which presents to it itself. 0 when there is no window (no display, or it failed)
VkSurfaceKHR vk_game_window(VkInstance inst, uint32_t w, uint32_t h);
// A Vulkan surface on the game's window, for a game that draws with Vulkan itself (the window opened first when
// needed).
VkSurfaceKHR vk_game_window(VkInstance inst, uint32_t w, uint32_t h) {
    uint32_t n;
    vk_present_instance_exts(&n);   // (decides whether there is a display)
    if (g_headless || g_win_failed) return VK_NULL_HANDLE;
    pthread_mutex_lock(&g_win_mu);
    int started = g_win_state != 0;
    if (!started) g_win_inst = inst;
    pthread_mutex_unlock(&g_win_mu);
    if (started && g_win_inst != inst) { VRED("a second Vulkan instance asked for the window: only the first one's surface is made"); return VK_NULL_HANDLE; }
    return window_thread(w, h) > 0 ? g_surf : VK_NULL_HANDLE;
}
// Opens the window (on its own thread) and checks the GPU can present to it; 0 when there is no window.
static int window_up(uint32_t w, uint32_t h) {
    if (g_win_failed) return 0;
    if (g_pool) return 1;
    int st = window_thread(w, h);
    if (st < 0) { g_win_failed = 1; return 0; }
    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, g_vk.qfam, g_surf, &ok);
    if (!ok || !g_vk.has_swapchain) { VRED("the GPU's queue cannot present to this window"); g_win_failed = 1; return 0; }
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &cpi, NULL, &g_pool));
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 8};
    VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, g_cbs));
    for (int i = 0; i < 8; i++) {
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_acq[i]));
        VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_done[i]));
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
        VK_CHECK(vkCreateFence(g_vk.dev, &fi, NULL, &g_fences[i]));
    }
    fprintf(stderr, "[nxl] window open (%ux%u)\n", w, h);
    return 1;
}
static void pump_events(void) {   // the window thread has the events; a resize remakes the swapchain here
    if (atomic_exchange(&g_win_resized, 0)) swapchain_destroy();
}
// ---------- the frame rate counter (NXL_FPS=1, or F1 in the window) ----------
// Games' frames shown in the last second and their average spacing, drawn by the CPU in a small 5x7 font into a
// buffer that is copied to the window's top left corner (one buffer per frame in flight: the fence of a slot frees it)
enum { FPS_SCALE = 3, FPS_CHARS = 16, FPS_W = (FPS_CHARS * 6 + 2) * FPS_SCALE, FPS_H = 9 * FPS_SCALE };
static VkBuffer g_fps_buf[8];
static VkDeviceMemory g_fps_mem[8];
static uint8_t *g_fps_px[8];
// The 5x7 pixel pattern of a character of the FPS counter (digits, F, P, S, M, '.', space).
static const uint8_t *glyph(char c) {
    static const uint8_t d[][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31}, {31,2,4,2,1,17,14}, {2,6,10,18,31,2,2},
        {31,16,30,1,1,17,14}, {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12}};
    static const uint8_t F[7] = {31,16,16,30,16,16,16}, P[7] = {30,17,17,30,16,16,16}, S[7] = {15,16,16,14,1,1,30},
                         M[7] = {17,27,21,21,17,17,17}, dot[7] = {0,0,0,0,0,12,12}, sp[7] = {0};
    if (c >= '0' && c <= '9') return d[c - '0'];
    return c == 'F' ? F : c == 'P' ? P : c == 'S' ? S : c == 'M' ? M : c == '.' ? dot : sp;
}
// Draws the FPS counter (frames per second and milliseconds per frame) into the corner of the frame being shown.
static void fps_draw(VkCommandBuffer cb, uint32_t fi, VkImage img) {
    if (!fps_on() || g_sc_ext.width < FPS_W + 16 || g_sc_ext.height < FPS_H + 16) return;
    if (!g_fps_buf[fi]) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, FPS_W * FPS_H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE};
        if (vkCreateBuffer(g_vk.dev, &bci, NULL, &g_fps_buf[fi]) != VK_SUCCESS) { g_fps_buf[fi] = VK_NULL_HANDLE; return; }
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(g_vk.dev, g_fps_buf[fi], &mr);
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                    vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        void *p = NULL;
        if (vkAllocateMemory(g_vk.dev, &mai, NULL, &g_fps_mem[fi]) != VK_SUCCESS || vkBindBufferMemory(g_vk.dev, g_fps_buf[fi], g_fps_mem[fi], 0) != VK_SUCCESS ||
            vkMapMemory(g_vk.dev, g_fps_mem[fi], 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
            vkDestroyBuffer(g_vk.dev, g_fps_buf[fi], NULL); g_fps_buf[fi] = VK_NULL_HANDLE; return;
        }
        g_fps_px[fi] = p;
    }
    // the frame rate: the game's presents in the last second (their times: g_pres_s); the last frame's time
    static double shown_fps, shown_ms, last_upd;
    double now = mono_ns() / 1e9;
    if (now - last_upd >= 0.5) {
        uint64_t n = atomic_load(&g_presents);
        int k = 0;   // presents in the last second, and their average spacing
        while (k < PRING - 8 && (uint64_t)k < n && now - g_pres_s[(n - k) % PRING] <= 1.0) k++;
        shown_fps = k;
        shown_ms = k >= 2 ? (g_pres_s[n % PRING] - g_pres_s[(n - k + 1) % PRING]) * 1e3 / (k - 1) : 0;
        last_upd = now;
    }
    char t[FPS_CHARS + 1];
    snprintf(t, sizeof t, "%3.0f FPS %5.1f MS", shown_fps, shown_ms > 999 ? 999 : shown_ms);
    int bgr = g_sc_fmt == VK_FORMAT_B8G8R8A8_UNORM || g_sc_fmt == VK_FORMAT_B8G8R8A8_SRGB;
    uint8_t col[4] = {bgr ? 80 : 255, 255, bgr ? 255 : 80, 255};   // a light yellow on a dark box
    uint8_t *px = g_fps_px[fi];
    for (int i = 0; i < FPS_W * FPS_H; i++) px[4 * i] = px[4 * i + 1] = px[4 * i + 2] = 16, px[4 * i + 3] = 255;
    for (int c = 0; t[c]; c++) {
        const uint8_t *g = glyph(t[c]);
        for (int gy = 0; gy < 7; gy++)
            for (int gx = 0; gx < 5; gx++) {
                if (!(g[gy] >> (4 - gx) & 1)) continue;
                for (int yy = 0; yy < FPS_SCALE; yy++)
                    for (int xx = 0; xx < FPS_SCALE; xx++) {
                        int X = (1 + c * 6 + gx) * FPS_SCALE + xx, Y = (1 + gy) * FPS_SCALE + yy;
                        memcpy(px + 4 * (Y * FPS_W + X), col, 4);
                    }
            }
    }
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {12, 12, 0}, {FPS_W, FPS_H, 1}};
    vkCmdCopyBufferToImage(cb, g_fps_buf[fi], img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
}

int vk_upscale_mode(void);   // vk_upscale.c
int vk_upscale(VkCommandBuffer cb, uint32_t slot, vk_tex *x, int sx, int sy, int sw, int sh, int flip, uint32_t dw, uint32_t dh, VkImage *out);
int vk_upscale_view(VkCommandBuffer cb, uint32_t slot, VkImageView src, VkFormat f, uint32_t tw, uint32_t th, int sx, int sy, int sw, int sh, int flip,
                    uint32_t dw, uint32_t dh, VkImage *out);
// what a frame is shown from: an NVN texture (x), or the present's own image of a frame an OpenGL game drew (x NULL),
// filled first from a buffer (upload, per frame slot) in the same command buffer
typedef struct { vk_tex *x; VkImage img; VkImageView view; VkFormat fmt; uint32_t w, h; int flip; VkBuffer upload[8]; int fresh; } pres_src;
static void blit_to_window_(const pres_src *s, const int crop[4]);
static void blit_src(const pres_src *s, const int crop[4]) {
    uint64_t t0 = nxl_perf_now();
    blit_to_window_(s, crop);
    nxl_perf_add(NXL_PERF_PRESENT, t0);
}
static void blit_to_window(vk_tex *x, const int crop[4]) {
    pres_src s = {x, vk_tex_image(x), VK_NULL_HANDLE, vk_tex_format(x), 0, 0, g_origin_lower_left, {0}, 0};
    vk_tex_hextent(x, 0, &s.w, &s.h, &(uint32_t){0});
    blit_src(&s, crop);
}
// Shows a frame in the window: the game's picture scaled (FSR 1.0 or plain) into the swapchain image, the overlay and
// counter on top.
static void blit_to_window_(const pres_src *src, const int crop[4]) {
    vk_tex *x = src->x;
    pump_events();
    if (!g_sc && swapchain_make()) return;
    uint32_t fi = g_frame++ % 8;
    vkWaitForFences(g_vk.dev, 1, &g_fences[fi], VK_TRUE, UINT64_MAX);
    uint32_t idx;
    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_sc, 1000000000ull, g_acq[fi], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { swapchain_destroy(); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
    vkResetFences(g_vk.dev, 1, &g_fences[fi]);
    VkCommandBuffer cb = g_cbs[fi];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                               VK_QUEUE_FAMILY_IGNORED, g_sc_img[idx], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(cb, &di);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, g_sc_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo di2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb};
    vkCmdPipelineBarrier2(cb, &di2);
    if (src->upload[fi]) {   // a frame from the CPU (an OpenGL game's): into the present's image first
        VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                                    VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, src->fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                                    VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, src->img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo idi = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &ib};
        vkCmdPipelineBarrier2(cb, &idi);
        VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {src->w, src->h, 1}};
        vkCmdCopyBufferToImage(cb, src->upload[fi], src->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        VkMemoryBarrier2 ub = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        VkDependencyInfo udi = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &ub};
        vkCmdPipelineBarrier2(cb, &udi);
    }
    uint32_t tw = src->w, th = src->h;
    int sx = 0, sy = 0, sw = (int)tw, sh = (int)th;
    if (crop && crop[2] > 0 && crop[3] > 0) {   // the game's crop rectangle, in the image's pixels (at the resolution scale, scaled)
        sx = crop[0], sy = crop[1], sw = crop[2], sh = crop[3];
        if (x && vk_tex_scaled(x)) { int x1 = vk_res_up_i(sx + sw), y1 = vk_res_up_i(sy + sh); sx = vk_res_up_i(sx), sy = vk_res_up_i(sy), sw = x1 - sx, sh = y1 - sy; }
    }
    // fit, keeping the aspect ratio
    double s = (double)g_sc_ext.width / sw < (double)g_sc_ext.height / sh ? (double)g_sc_ext.width / sw : (double)g_sc_ext.height / sh;
    int dw = (int)(sw * s), dh = (int)(sh * s), dx = ((int)g_sc_ext.width - dw) / 2, dy = ((int)g_sc_ext.height - dh) / 2;
    static int fill = -1;   // NXL_FILL=1: the whole window, stretched (a widescreen mod's 16:9 frame holds a wider view)
    if (fill < 0) { const char *e = getenv("NXL_FILL"); fill = e && atoi(e) ? 1 : 0; if (fill) fprintf(stderr, "[vk] the picture fills the window, stretched (NXL_FILL)\n"); }
    if (fill) dw = (int)g_sc_ext.width, dh = (int)g_sc_ext.height, dx = 0, dy = 0;
    int y0 = sy, y1 = sy + sh;
    if (src->flip) y0 = sy + sh, y1 = sy;   // GL's origin: the frame is upside down in memory
    VkImage up;
    if (dw > 0 && dh > 0 && (x ? vk_upscale(cb, fi, x, sx, sy, sw, sh, src->flip, (uint32_t)dw, (uint32_t)dh, &up)
                               : vk_upscale_view(cb, fi, src->view, src->fmt, tw, th, sx, sy, sw, sh, src->flip, (uint32_t)dw, (uint32_t)dh, &up))) {   // FSR (vk_upscale.c): copied 1:1
        VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {dw, dh, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
        vkCmdBlitImage(cb, up, VK_IMAGE_LAYOUT_GENERAL, g_sc_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    } else {
        VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{sx, y0, 0}, {sx + sw, y1, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
        vkCmdBlitImage(cb, src->img, VK_IMAGE_LAYOUT_GENERAL, g_sc_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                       vk_upscale_mode() == 2 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    }
    if (fps_on()) { vkCmdPipelineBarrier2(cb, &di2); fps_draw(cb, fi, g_sc_img[idx]); }   // over the frame (after the blit's writes)
    vkCmdPipelineBarrier2(cb, &di2);
    vk_overlay_draw(cb, g_sc_img[idx], g_sc_fmt, g_sc_ext.width, g_sc_ext.height);   // NativeNX's pop-ups (achievements), at the top right
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_NONE, b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier2(cb, &di);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &g_acq[fi], &ws, 1, &cb, 1, &g_done[fi]};
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &g_done[fi], 1, &g_sc, &idx, NULL};
    vk_queue_lock();
    vkQueueSubmit(g_vk.queue, 1, &si, g_fences[fi]);
    r = vkQueuePresentKHR(g_vk.queue, &pi);
    vk_queue_unlock();
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchain_destroy();
    else if (r != VK_SUCCESS) VRED_ONCE("vkQueuePresentKHR: %d (first case)", r);
    else if (g_frame == 1) fprintf(stderr, "[nxl] first frame shown in the window (%ux%u)\n", g_sc_ext.width, g_sc_ext.height);
}
#endif

uint64_t vk_presents(void) { return atomic_load(&g_presents); }
#ifdef NXL_HAVE_SDL3
static void shot_path(char *path, size_t n) {   // a new screenshot's file: the album folder (NXL_ALBUM_DIR), the time
    char dir[1024];
    const char *a = getenv("NXL_ALBUM_DIR");
    if (a && *a) snprintf(dir, sizeof dir, "%s", a);
    else { nxl_user_dir(NXL_DIR_DATA, dir, sizeof dir - 32); strcat(dir, "/album/nxlinux"); }
    for (char *q = dir + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(dir, 0755); *q = '/'; }
    mkdir(dir, 0755);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[64];
    strftime(ts, sizeof ts, "%Y-%m-%d_%H-%M-%S", &tm);
    snprintf(path, n, "%s/%s.png", dir, ts);
    for (int k = 2; !access(path, F_OK) && k < 100; k++) snprintf(path, n, "%s/%s_%d.png", dir, ts, k);
}
#endif
// The game presents a texture: counted and timed, written to a file when asked, shown in the window.
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)period_s;
    uint64_t n = atomic_fetch_add(&g_presents, 1) + 1;
    g_pres_s[n % PRING] = mono_ns() / 1e9;
    if (n == 1) fprintf(stderr, "[nxl] nvn: first frame presented%s\n", g_headless == 1 ? " (no window: NXL_DUMP_FRAMES=<folder> writes frames)" : "");
    if (!vk_ready() || !t) return n;
    if (n == 1 && getenv("NXL_TEST_UPSCALE")) { void vk_upscale_selftest(const char *); vk_upscale_selftest(getenv("NXL_TEST_UPSCALE")); }   // tests/gfx/t_upscale.py
    if (n == 1 && getenv("NXL_TEST_OVERLAY")) { void vk_overlay_selftest(const char *); vk_overlay_selftest(getenv("NXL_TEST_OVERLAY")); }   // t_window_overlay
    vk_tex *x = vk_tex_get(t, 1);
    if (!vk_tex_image(x)) return n;
    if (g_dump_dir && (n == 1 || n % (uint64_t)g_dump_every == 0)) dump_frame(x, n);
#ifdef NXL_HAVE_SDL3
    { static int at = -1; if (at < 0) { const char *e = getenv("NXL_TEST_SHOT"); at = e ? atoi(e) : 0; } if (at > 0 && n == (uint64_t)at) atomic_store(&g_shot_req, 1); }   // (tests: F12 at that frame)
    if (atomic_exchange(&g_shot_req, 0)) {   // F12: the frame as a PNG in the album (NXL_ALBUM_DIR, from NativeNX: one folder per game)
        char path[1200];
        shot_path(path, sizeof path);
        dump_frame_to(x, path, 0, 0, 0, 0, 1);
        fprintf(stderr, "[vk] screenshot: %s\n", path);
    }
#endif
    {   // a picture the draw side asked for (the hide cycle, vk_draw.c): the middle of the frame, where the player stands
        char path[600];
        if (vk_hide_take_dump(path, sizeof path)) {
            uint32_t w, h, d;
            vk_tex_extent(x, 0, &w, &h, &d);
            dump_frame_to(x, path, 0, 0, 0, 0, 2);   // the whole frame, at half size
            fprintf(stderr, "[vk] hide cycle: wrote %s\n", path);
        }
    }
#ifdef NXL_HAVE_SDL3
    if (g_headless == 0) {
        uint32_t w, h, d;
        vk_tex_extent(x, 0, &w, &h, &d);
        if (window_up(w, h)) blit_to_window(x, crop);
    }
#else
    (void)crop;
#endif
    return n;
}
uint64_t nxl_frames_presented(void);
uint64_t nxl_frames_presented(void) { return atomic_load(&g_presents); }
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
void mtl_present_flush(void) {}
double mtl_presented_time(uint64_t n) { return n && n <= atomic_load(&g_presents) && atomic_load(&g_presents) - n < PRING ? g_pres_s[n % PRING] : -1; }
uint32_t mtl_present_qos(void) { return 0; }
int mtl_has_window(void) { return 0; }   // hle_nvn.c paces presents by the clock
int mtl_display_hz(void) { return 0; }
int mtl_link_active(void) { return 0; }
int mtl_link_stalled(void) { return 0; }
int mtl_link_rate(void) { return 60; }
// Waits n vsync ticks (60 Hz) or the timeout, whichever is shorter.
static int sleep_ticks(int n, uint64_t timeout_ns) {
    uint64_t ns = (uint64_t)(n > 0 ? n : 1) * 16666667ull;
    if (timeout_ns && ns > timeout_ns) ns = timeout_ns;
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    nanosleep(&ts, NULL);
    return 1;
}
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { return sleep_ticks(vsyncs, timeout_ns); }
int mtl_link_wait_ticks(int ticks, uint64_t timeout_ns) { return sleep_ticks(ticks, timeout_ns); }
int mtl_fps_target(void) {
    static int t;
    if (!t) { const char *e = getenv("DM_FPS"); t = e && atoi(e) == 30 ? 30 : 60; }
    return t;
}
int mtl_snap_on(void) { return 0; }
uint64_t mtl_snap_acquire_wait(void) { return 0; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }

// ---------- frames from the CPU: an OpenGL game's (gfx/hle_egl.c reads each frame back from the PC's GL driver) ----------
// vk_cpu_frame_begin gives the memory the frame's RGBA8 pixels go into (w x h, rows bottom first when the frame is GL's
// way up); vk_cpu_frame_end shows it as an NVN game's frame is: scaled to the window (FSR), with the frame rate counter,
// NativeNX's pop-ups and menu, screenshots and NXL_DUMP_FRAMES. With a window the memory is the upload buffer of the
// frame slot the window shows it from (its fence waited first); without one a buffer of the CPU's
static struct {
    uint32_t w, h;
    int win, slot, fresh;
    VkBuffer b[8]; VkDeviceMemory m[8]; uint8_t *p[8]; VkDeviceSize bsz;
    VkImage img; VkDeviceMemory im; VkImageView view; uint32_t iw, ih;
    uint8_t *cpu; size_t cpusz;
    uint8_t *cur;
} g_cf;
// Writes an RGBA picture as a PNG (upside down when flip), for frame dumps and screenshots.
static void write_rgba_png(const char *path, const uint8_t *px, uint32_t w, uint32_t h, int flip) {
    uint8_t *img = malloc((size_t)w * h * 3);
    if (!img) return;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = px + (size_t)(flip ? h - 1 - y : y) * w * 4;
        for (uint32_t i = 0; i < w; i++) memcpy(img + ((size_t)y * w + i) * 3, s + 4 * i, 3);
    }
    if (!stbi_write_png(path, (int)w, (int)h, 3, img, (int)w * 3)) VRED_ONCE("frame pictures: cannot write %s", path);
    free(img);
}
#ifdef NXL_HAVE_SDL3
static int cf_images(uint32_t w, uint32_t h) {   // the present's image and the slots' upload buffers, at this size
    VkDeviceSize need = (VkDeviceSize)w * h * 4;
    if (g_cf.img && g_cf.iw == w && g_cf.ih == h && g_cf.bsz >= need) return 1;
    vk_idle();
    if (g_cf.view) vkDestroyImageView(g_vk.dev, g_cf.view, NULL);
    if (g_cf.img) vkDestroyImage(g_vk.dev, g_cf.img, NULL);
    if (g_cf.im) vkFreeMemory(g_vk.dev, g_cf.im, NULL);
    g_cf.view = VK_NULL_HANDLE, g_cf.img = VK_NULL_HANDLE, g_cf.im = VK_NULL_HANDLE;
    for (int i = 0; i < 8 && g_cf.bsz < need; i++) {
        if (g_cf.b[i]) vkDestroyBuffer(g_vk.dev, g_cf.b[i], NULL), vkFreeMemory(g_vk.dev, g_cf.m[i], NULL);
        g_cf.b[i] = VK_NULL_HANDLE, g_cf.p[i] = NULL;
    }
    if (g_cf.bsz < need) {
        for (int i = 0; i < 8; i++) {
            VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE};
            VkMemoryRequirements mr;
            if (vkCreateBuffer(g_vk.dev, &bci, NULL, &g_cf.b[i]) != VK_SUCCESS) return 0;
            vkGetBufferMemoryRequirements(g_vk.dev, g_cf.b[i], &mr);
            VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                        vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
            void *p = NULL;
            if (vkAllocateMemory(g_vk.dev, &mai, NULL, &g_cf.m[i]) != VK_SUCCESS) { vkDestroyBuffer(g_vk.dev, g_cf.b[i], NULL); g_cf.b[i] = VK_NULL_HANDLE; return 0; }
            vkBindBufferMemory(g_vk.dev, g_cf.b[i], g_cf.m[i], 0);
            vkMapMemory(g_vk.dev, g_cf.m[i], 0, VK_WHOLE_SIZE, 0, &p);
            g_cf.p[i] = p;
        }
        g_cf.bsz = need;
    }
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {w, h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(g_vk.dev, &ici, NULL, &g_cf.img) != VK_SUCCESS) { g_cf.img = VK_NULL_HANDLE; return 0; }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_vk.dev, g_cf.img, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (vkAllocateMemory(g_vk.dev, &mai, NULL, &g_cf.im) != VK_SUCCESS) { vkDestroyImage(g_vk.dev, g_cf.img, NULL); g_cf.img = VK_NULL_HANDLE; return 0; }
    vkBindImageMemory(g_vk.dev, g_cf.img, g_cf.im, 0);
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g_cf.img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                                 {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(g_vk.dev, &vci, NULL, &g_cf.view) != VK_SUCCESS) g_cf.view = VK_NULL_HANDLE;
    g_cf.iw = w, g_cf.ih = h, g_cf.fresh = 1;
    return 1;
}
#endif
void *vk_cpu_frame_begin(uint32_t w, uint32_t h);
// Starts a frame drawn by the CPU (an OpenGL game read back): where to put its w x h pixels, in a mapped image when
// there is a window.
void *vk_cpu_frame_begin(uint32_t w, uint32_t h) {
    if (!w || !h) return NULL;
    g_cf.w = w, g_cf.h = h, g_cf.win = 0;
#ifdef NXL_HAVE_SDL3
    if (vk_ready() && g_headless == 0 && window_up(w, h) && cf_images(w, h)) {
        g_cf.slot = (int)(g_frame % 8);
        vkWaitForFences(g_vk.dev, 1, &g_fences[g_cf.slot], VK_TRUE, UINT64_MAX);   // the slot's last frame is shown: its buffer is free
        g_cf.win = 1;
        return g_cf.cur = g_cf.p[g_cf.slot];
    }
#endif
    size_t need = (size_t)w * h * 4;
    if (g_cf.cpusz < need) { free(g_cf.cpu); g_cf.cpu = malloc(need); g_cf.cpusz = g_cf.cpu ? need : 0; }
    return g_cf.cur = g_cf.cpu;
}
uint64_t vk_cpu_frame_end(int flip);
// Shows (and dumps when asked) the frame the CPU wrote, counted and timed as a game frame.
uint64_t vk_cpu_frame_end(int flip) {
    uint64_t n = atomic_fetch_add(&g_presents, 1) + 1;
    g_pres_s[n % PRING] = mono_ns() / 1e9;
    if (n == 1) fprintf(stderr, "[nxl] gl: first frame presented (%ux%u)%s\n", g_cf.w, g_cf.h, g_headless == 1 ? " (no window: NXL_DUMP_FRAMES=<folder> writes frames)" : "");
    if (!g_cf.cur) return n;
    if (g_dump_dir && (n == 1 || n % (uint64_t)g_dump_every == 0)) {
        char path[4096];
        snprintf(path, sizeof path, "%s/frame%06llu.png", g_dump_dir, (unsigned long long)n);
        write_rgba_png(path, g_cf.cur, g_cf.w, g_cf.h, flip);
        static int said;
        if (!said++) fprintf(stderr, "[vk] first frame picture: %s\n", path);
    }
#ifdef NXL_HAVE_SDL3
    { static int at = -1; if (at < 0) { const char *e = getenv("NXL_TEST_SHOT"); at = e ? atoi(e) : 0; } if (at > 0 && n == (uint64_t)at) atomic_store(&g_shot_req, 1); }
    if (atomic_exchange(&g_shot_req, 0)) {
        char path[1200];
        shot_path(path, sizeof path);
        write_rgba_png(path, g_cf.cur, g_cf.w, g_cf.h, flip);
        fprintf(stderr, "[vk] screenshot: %s\n", path);
    }
    if (g_cf.win) {
        pres_src s = {NULL, g_cf.img, g_cf.view, VK_FORMAT_R8G8B8A8_UNORM, g_cf.w, g_cf.h, flip, {0}, g_cf.fresh};
        s.upload[g_cf.slot] = g_cf.b[g_cf.slot];
        uint32_t before = g_frame;
        blit_src(&s, NULL);
        if (g_frame != before) g_cf.fresh = 0;
    }
#endif
    g_cf.cur = NULL;
    return n;
}
#ifndef NXL_HAVE_SDL3
VkSurfaceKHR vk_game_window(VkInstance inst, uint32_t w, uint32_t h);
VkSurfaceKHR vk_game_window(VkInstance inst, uint32_t w, uint32_t h) { (void)inst; (void)w; (void)h; return VK_NULL_HANDLE; }
#endif
