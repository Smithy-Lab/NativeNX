// SPDX-License-Identifier: GPL-3.0-or-later
// An OpenGL game's calls, as a Switch game makes them (its vi layer, then EGL and OpenGL ES 3), bridged to the PC's
// driver (gfx/hle_egl.c and the generated bridges): a window surface, a context, clears and a drawn triangle read back,
// floats in the vector registers, eglGetProcAddress's addresses, a debug callback that must not be called, and swaps
// shown through the Vulkan present (NXL_DUMP_FRAMES writes them). The tests run it on Mesa's software driver.
#include "nx.h"

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int EGLint;
typedef unsigned EGLBoolean, EGLenum;
EGLDisplay eglGetDisplay(void *native);
EGLBoolean eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor);
EGLBoolean eglBindAPI(EGLenum api);
EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *attr, EGLConfig *cfg, EGLint n, EGLint *num);
EGLBoolean eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint attr, EGLint *v);
EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, void *win, const EGLint *attr);
EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *attr);
EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c);
EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *v);
EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s);
EGLBoolean eglSwapInterval(EGLDisplay d, EGLint n);
void *eglGetProcAddress(const char *name);
EGLint eglGetError(void);
const char *eglQueryString(EGLDisplay d, EGLint name);
// nn::vi, as SDL's Switch driver calls it: the display, a layer of a given size, its native window
Result _ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE(void **out);
Result _ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEii(void **out, void *disp, int w, int h);
Result _ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE(void **out, void *layer);

typedef unsigned GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef unsigned char GLubyte;
const GLubyte *glGetString(GLenum name);
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glClear(GLbitfield mask);
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *px);
GLenum glGetError(void);
GLuint glCreateShader(GLenum type);
void glShaderSource(GLuint s, GLsizei n, const char *const *src, const GLint *len);
void glCompileShader(GLuint s);
void glGetShaderiv(GLuint s, GLenum p, GLint *v);
GLuint glCreateProgram(void);
void glAttachShader(GLuint p, GLuint s);
void glLinkProgram(GLuint p);
void glGetProgramiv(GLuint p, GLenum q, GLint *v);
void glUseProgram(GLuint p);
GLint glGetUniformLocation(GLuint p, const char *name);
void glUniform4f(GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d);
void glGenBuffers(GLsizei n, GLuint *b);
void glBindBuffer(GLenum t, GLuint b);
void glBufferData(GLenum t, long size, const void *data, GLenum usage);
void glGenVertexArrays(GLsizei n, GLuint *a);
void glBindVertexArray(GLuint a);
void glVertexAttribPointer(GLuint i, GLint size, GLenum type, unsigned char norm, GLsizei stride, const void *p);
void glEnableVertexAttribArray(GLuint i);
void glDrawArrays(GLenum mode, GLint first, GLsizei n);
void glDebugMessageCallback(void *cb, const void *user);

enum {
    EGL_NONE = 0x3038, EGL_SURFACE_TYPE = 0x3033, EGL_WINDOW_BIT = 4, EGL_RENDERABLE_TYPE = 0x3040, EGL_OPENGL_ES3_BIT = 0x40,
    EGL_RED_SIZE = 0x3024, EGL_GREEN_SIZE = 0x3023, EGL_BLUE_SIZE = 0x3022, EGL_ALPHA_SIZE = 0x3021, EGL_WIDTH = 0x3057,
    EGL_HEIGHT = 0x3056, EGL_OPENGL_ES_API = 0x30A0, EGL_OPENGL_API = 0x30A2, EGL_OPENGL_BIT = 0x8, EGL_CONTEXT_MINOR_VERSION = 0x30FB,
    EGL_CONTEXT_OPENGL_PROFILE_MASK = 0x30FD, EGL_CONTEXT_MAJOR_VERSION = 0x3098, EGL_VERSION = 0x3054,
    GL_VERSION = 0x1F02, GL_RENDERER = 0x1F01, GL_COLOR_BUFFER_BIT = 0x4000, GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401,
    GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30, GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82,
    GL_ARRAY_BUFFER = 0x8892, GL_STATIC_DRAW = 0x88E4, GL_FLOAT = 0x1406, GL_TRIANGLES = 4,
};

static int g_called;
static void guest_debug_cb(GLenum s, GLenum t, GLuint id, GLenum sev, GLsizei len, const char *msg, const void *u) { g_called = 1; }

static u32 pixel(int x, int y) {
    u32 px = 0;
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &px);
    return px;
}
static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    return ok ? s : 0;
}

int test_main(void) {
    void *disp = NULL, *layer = NULL, *win = NULL;
    _ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE(&disp);
    _ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEii(&layer, disp, 64, 48);
    _ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE(&win, layer);
    CHECK("vi: the display, a 64x48 layer and its native window", disp && layer && win);

    EGLDisplay d = eglGetDisplay(NULL);
    EGLint maj = 0, min = 0;
    CHECK("eglGetDisplay and eglInitialize: one of the PC's displays", d && eglInitialize(d, &maj, &min) && maj == 1);
    print("# EGL %d.%d: %s\n", maj, min, eglQueryString(d, EGL_VERSION));
    CHECK("eglBindAPI(OpenGL ES)", eglBindAPI(EGL_OPENGL_ES_API));
    EGLint attr[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                     EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg = NULL;
    EGLint n = 0, st = 0;
    CHECK("eglChooseConfig with the window bit finds a configuration", eglChooseConfig(d, attr, &cfg, 1, &n) && n == 1 && cfg);
    CHECK("... which says it has windows", eglGetConfigAttrib(d, cfg, EGL_SURFACE_TYPE, &st) && (st & EGL_WINDOW_BIT));
    EGLSurface s = eglCreateWindowSurface(d, cfg, win, NULL);
    EGLint w = 0, h = 0;
    CHECK("eglCreateWindowSurface on the layer's window", s != NULL);
    CHECK("... of the layer's size", eglQuerySurface(d, s, EGL_WIDTH, &w) && eglQuerySurface(d, s, EGL_HEIGHT, &h) && w == 64 && h == 48);
    EGLint cattr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext c = eglCreateContext(d, cfg, NULL, cattr);
    CHECK("an OpenGL ES 3 context, made current", c && eglMakeCurrent(d, s, s, c));
    const char *ver = (const char *)glGetString(GL_VERSION);
    CHECK("glGetString(GL_VERSION): the PC driver's string", ver && ver[0] == 'O');
    print("# %s, %s\n", ver ? ver : "?", (const char *)glGetString(GL_RENDERER));

    glDebugMessageCallback((void *)guest_debug_cb, NULL);   // guest code: must not be handed to the driver
    glViewport(0, 0, 64, 48);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);   // floats: in v0-v3
    glClear(GL_COLOR_BUFFER_BIT);
    CHECK("glClearColor + glClear: red", pixel(10, 10) == 0xff0000ffu);
    void (*clear_color)(GLfloat, GLfloat, GLfloat, GLfloat) = (void (*)(GLfloat, GLfloat, GLfloat, GLfloat))eglGetProcAddress("glClearColor");
    void (*clear)(GLbitfield) = (void (*)(GLbitfield))eglGetProcAddress("glClear");
    CHECK("eglGetProcAddress: glClearColor and glClear", clear_color && clear);
    if (clear_color && clear) { clear_color(0.0f, 0.0f, 1.0f, 1.0f); clear(GL_COLOR_BUFFER_BIT); }
    CHECK("... called through those addresses: blue", pixel(10, 10) == 0xffff0000u);

    GLuint vs = shader(GL_VERTEX_SHADER, "#version 300 es\nlayout(location = 0) in vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n");
    GLuint fs = shader(GL_FRAGMENT_SHADER, "#version 300 es\nprecision mediump float;\nuniform vec4 col;\nout vec4 o;\nvoid main() { o = col; }\n");
    CHECK("GLSL ES shaders compile", vs && fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs), glAttachShader(p, fs);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK("... and link", linked);
    glUseProgram(p);
    glUniform4f(glGetUniformLocation(p, "col"), 0.0f, 1.0f, 0.0f, 1.0f);
    static const float tri[] = {-1, -1, 1, -1, -1, 1};
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao), glBindVertexArray(vao);
    glGenBuffers(1, &vbo), glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, 0, 8, NULL);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    CHECK("a triangle drawn: the uniform's green inside it", pixel(4, 4) == 0xff00ff00u);
    CHECK("... the clear colour outside it", pixel(60, 44) == 0xffff0000u);
    CHECK("no GL error", glGetError() == 0);
    CHECK("eglSwapInterval(1)", eglSwapInterval(d, 1));
    int swaps = 1;
    for (int i = 0; i < 3; i++) swaps &= eglSwapBuffers(d, s) == 1;
    CHECK("eglSwapBuffers three times: shown through the present", swaps);
    CHECK("... the frame stays as it was for the game", pixel(4, 4) == 0xff00ff00u);
    CHECK("the game's debug callback was not called by the driver", !g_called);

    // desktop OpenGL, as engines on NVIDIA's Switch driver use it (a core profile): another context on another window
    if (!eglBindAPI(EGL_OPENGL_API)) { print("# no desktop OpenGL in the PC's EGL: not tested\n"); return 0; }
    EGLint gattr[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig gcfg = NULL;
    n = 0;
    CHECK("desktop OpenGL: a configuration", eglChooseConfig(d, gattr, &gcfg, 1, &n) && n == 1);
    EGLSurface gs = eglCreateWindowSurface(d, gcfg, win, NULL);
    EGLint gc_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK, 1, EGL_NONE};
    EGLContext gc = eglCreateContext(d, gcfg, NULL, gc_attr);
    CHECK("... a 3.3 core context on a window, made current", gs && gc && eglMakeCurrent(d, gs, gs, gc));
    ver = (const char *)glGetString(GL_VERSION);
    print("# %s\n", ver ? ver : "?");
    glClearColor(1.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    CHECK("... cleared yellow", pixel(10, 10) == 0xff00ffffu);
    swaps = 1;
    for (int i = 0; i < 2; i++) swaps &= eglSwapBuffers(d, gs) == 1;
    CHECK("... swapped (read back from its default framebuffer)", swaps && glGetError() == 0);
    return 0;
}
