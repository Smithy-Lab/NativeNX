// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX, drawing: colours, text (stb_truetype), shapes and icons, pictures and the backgrounds made on a thread (nativenx.h lists the parts).
#define _GNU_SOURCE   // (before any system header: the image libraries include some)
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#undef STB_TRUETYPE_IMPLEMENTATION
#undef STB_IMAGE_IMPLEMENTATION
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#include "nativenx.h"
static void shapes_clear(void);
const rgba C_TEXT = {255, 255, 255, 255}, C_DIM = {255, 255, 255, 170}, C_ACCENT = {40, 220, 235, 255};
void color(rgba c) { SDL_SetRenderDrawColor(g_r, c.r, c.g, c.b, c.a); }
SDL_FColor fcol(rgba c) { return (SDL_FColor){c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f}; }
_Static_assert(NRANGES == 5, "NRANGES counts RANGES");
static const range_t RANGES[NRANGES] = {{0x20, 0x5F}, {0xA0, 0x60}, {0x2010, 0x18}, {0x2122, 1}, {0x2190, 4}};
uint8_t *g_ttf, *g_ttf_bold;
stbtt_fontinfo g_fi, g_fi_bold;
const float FONT_PX[F_N] = {20, 24, 40, 28, 64, 24};
static font_t g_font[F_N];
static float g_font_scale;

// Reads a whole file; NULL when it cannot.
static uint8_t *read_file(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = sz > 0 ? malloc((size_t)sz) : NULL;
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    if (n) *n = b ? (size_t)sz : 0;
    return b;
}
// The PC's font for the launcher's text (Liberation Sans, DejaVu Sans or Noto Sans, bold or regular), read whole.
uint8_t *find_font(int bold) {
    static const char *const reg[] = {"/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf", "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
                                      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                                      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/google-noto/NotoSans-Regular.ttf", NULL};
    static const char *const bld[] = {"/usr/share/fonts/liberation-sans-fonts/LiberationSans-Bold.ttf", "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
                                      "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
                                      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/google-noto/NotoSans-Bold.ttf", NULL};
    const char *const *list = bold ? bld : reg;
#ifdef _WIN32   // Windows' own: Segoe UI, else Arial, in its Fonts folder
    (void)list;
    char path[1024];
    const char *windir = getenv("WINDIR");
    static const char *const win[2][2] = {{"segoeui.ttf", "arial.ttf"}, {"segoeuib.ttf", "arialbd.ttf"}};
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof path, "%s/Fonts/%s", windir && *windir ? windir : "C:/Windows", win[bold != 0][i]);
        uint8_t *b = read_file(path, NULL);
        if (b) return b;
    }
#else
    for (int i = 0; list[i]; i++) { uint8_t *b = read_file(list[i], NULL); if (b) return b; }
    FILE *p = popen(bold ? "fc-match -f '%{file}' sans:bold 2>/dev/null" : "fc-match -f '%{file}' sans 2>/dev/null", "r");   // fontconfig's choice
    char path[1024] = "";
    if (p) { if (!fgets(path, sizeof path, p)) path[0] = 0; pclose(p); }
    if (path[0]) return read_file(path, NULL);
#endif
    char own[1200];   // the last resort: a font that came with NativeNX (the AppImage's), in its data folder
    snprintf(own, sizeof own, "%s/fonts/%s", g_data, bold ? "DejaVuSans-Bold.ttf" : "DejaVuSans.ttf");
    return g_data[0] ? read_file(own, NULL) : NULL;
}
// Bakes a font's glyphs (Latin, punctuation, arrows) at a pixel size into a texture.
static void font_build(font_t *f, float px, int bold) {
    if (f->tex) SDL_DestroyTexture(f->tex);
    for (int i = 0; i < NRANGES; i++) free(f->pc[i]);
    memset(f, 0, sizeof *f);
    f->px = px;
    const stbtt_fontinfo *fi = bold && g_ttf_bold ? &g_fi_bold : &g_fi;
    const uint8_t *ttf = bold && g_ttf_bold ? g_ttf_bold : g_ttf;
    int aw = 1024, ah = px > 90 ? 2048 : 1024;
    uint8_t *alpha = calloc((size_t)aw * ah, 1);
    stbtt_pack_context pk;
    stbtt_pack_range pr[NRANGES];
    for (int i = 0; i < NRANGES; i++) {
        f->pc[i] = calloc((size_t)RANGES[i].n, sizeof(stbtt_packedchar));
        pr[i] = (stbtt_pack_range){px, RANGES[i].first, NULL, RANGES[i].n, f->pc[i], 0, 0};
    }
    stbtt_PackBegin(&pk, alpha, aw, ah, 0, 1, NULL);
    stbtt_PackSetOversampling(&pk, 2, 1);
    stbtt_PackFontRanges(&pk, ttf, 0, pr, NRANGES);
    stbtt_PackEnd(&pk);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(fi, &asc, &desc, &gap);
    f->ascent = asc * stbtt_ScaleForPixelHeight(fi, px);
    uint32_t *px32 = malloc((size_t)aw * ah * 4);
    for (int i = 0; i < aw * ah; i++) px32[i] = 0x00FFFFFFu | (uint32_t)alpha[i] << 24;
    f->tex = SDL_CreateTexture(g_r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, aw, ah);
    SDL_UpdateTexture(f->tex, NULL, px32, aw * 4);
    SDL_SetTextureBlendMode(f->tex, SDL_BLENDMODE_BLEND);
    free(px32), free(alpha);
}
// Bakes the fonts again when the window's scale changed (so text stays sharp at any size).
void fonts_for_scale(void) {
    if (g_font_scale == g_s) return;
    g_font_scale = g_s;
    shapes_clear();   // the shapes are drawn at the window's scale too
    prompts_clear();
    for (int i = 0; i < F_N; i++) font_build(&g_font[i], FONT_PX[i] * g_s, i == F_TITLE || i == F_BIG || i == F_CLOCK || i == F_BODYB);
}
// The next character of a UTF-8 string (the pointer moved past it).
uint32_t utf8_next(const char **s) {
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t c = *p++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    c &= extra == 3 ? 7 : extra == 2 ? 15 : extra == 1 ? 31 : 0x7F;
    while (extra-- > 0 && (*p & 0xC0) == 0x80) c = c << 6 | (*p++ & 0x3F);
    *s = (const char *)p;
    return c;
}
static int glyph(const font_t *f, uint32_t c, int *ri, int *gi) {
    for (int i = 0; i < NRANGES; i++) if (c >= (uint32_t)RANGES[i].first && c < (uint32_t)(RANGES[i].first + RANGES[i].n)) { *ri = i; *gi = (int)c - RANGES[i].first; return 1; }
    if (c != '?') return glyph(f, '?', ri, gi);
    return 0;
}
// width of s in design units
float text_w(int fid, const char *s) {
    const font_t *f = &g_font[fid];
    float x = 0, y = 0;
    while (*s) {
        int ri, gi;
        if (!glyph(f, utf8_next(&s), &ri, &gi)) continue;
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(f->pc[ri], 1024, 1024, gi, &x, &y, &q, 0);
    }
    return x / g_s;
}
// s with its top at y (design units); returns the width
float text(int fid, float x, float y, rgba c, const char *s) {
    const font_t *f = &g_font[fid];
    float aw, ah;
    SDL_GetTextureSize(f->tex, &aw, &ah);
    SDL_SetTextureColorMod(f->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(f->tex, c.a);
    float px = x * g_s, py = y * g_s + f->ascent, x0 = px;
    while (*s) {
        int ri, gi;
        if (!glyph(f, utf8_next(&s), &ri, &gi)) continue;
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(f->pc[ri], (int)aw, (int)ah, gi, &px, &py, &q, 0);
        SDL_FRect src = {q.s0 * aw, q.t0 * ah, (q.s1 - q.s0) * aw, (q.t1 - q.t0) * ah};
        SDL_FRect dst = {q.x0 / g_s, q.y0 / g_s, (q.x1 - q.x0) / g_s, (q.y1 - q.y0) / g_s};
        SDL_RenderTexture(g_r, f->tex, &src, &dst);
    }
    return (px - x0) / g_s;
}

// ---------- shapes ----------
// Drawn on the CPU with anti-aliased edges and kept as textures: each shape is a signed distance (in pixels, negative
// inside) whose edge covers half a pixel either way; a texture per shape and pixel size, white, tinted when drawn
static float sd_circle(float x, float y, float cx, float cy, float r) { return hypotf(x - cx, y - cy) - r; }
static float sd_box(float x, float y, float cx, float cy, float hw, float hh, float r) {
    float qx = fabsf(x - cx) - hw + r, qy = fabsf(y - cy) - hh + r;
    return hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - r;
}
static float sd_seg(float x, float y, float ax, float ay, float bx, float by, float r) {
    float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay, h = fminf(1, fmaxf(0, (px * dx + py * dy) / (dx * dx + dy * dy)));
    return hypotf(px - dx * h, py - dy * h) - r;
}
static float smin(float a, float b, float k) { float h = fmaxf(k - fabsf(a - b), 0) / k; return fminf(a, b) - h * h * k * 0.25f; }
pad_layout pad_lay(int k) {
    if (k == K_PAD_PS) return (pad_layout){-0.25f, 0.1f, 0.25f, 0.1f, -0.53f, -0.12f, 0.53f, -0.12f};
    return (pad_layout){-0.53f, -0.12f, 0.25f, 0.1f, -0.25f, 0.1f, 0.53f, -0.12f};   // Xbox / Nintendo: offset sticks
}
// distance at (x, y) in shape units (the shape's height is 2 units for icons, 1.24 for controllers)
float shape_d(int k, float x, float y, float w_units, float h_units, float rad) {
    switch (k) {
    case K_RRECT: return sd_box(x, y, 0, 0, w_units / 2, h_units / 2, rad);
    case K_GEAR: {
        float d = sd_circle(x, y, 0, 0, 0.68f);
        for (int i = 0; i < 8; i++) {
            float a = (float)M_PI / 4 * i, c = cosf(a), s = sinf(a), qx = x * c + y * s, qy = -x * s + y * c;
            d = fminf(d, sd_box(qx, qy, 0.82f, 0, 0.16f, 0.14f, 0.06f));
        }
        return fmaxf(d, -sd_circle(x, y, 0, 0, 0.29f));
    }
    case K_POWER: {
        float ring = fabsf(hypotf(x, y) - 0.64f) - 0.12f;
        ring = fmaxf(ring, -sd_box(x, y, 0, -0.62f, 0.3f, 0.45f, 0));
        return fminf(ring, sd_seg(x, y, 0, -0.95f, 0, -0.18f, 0.12f));
    }
    case K_FOLDER: return fminf(sd_box(x, y, -0.45f, -0.52f, 0.42f, 0.18f, 0.08f), sd_box(x, y, 0, 0.12f, 0.95f, 0.62f, 0.12f));
    case K_BOLT: {
        float d = sd_seg(x, y, 0.25f, -0.9f, -0.35f, 0.08f, 0.13f);
        d = fminf(d, sd_seg(x, y, -0.35f, 0.08f, 0.35f, -0.08f, 0.13f));
        return fminf(d, sd_seg(x, y, 0.35f, -0.08f, -0.25f, 0.9f, 0.13f));
    }
    case K_CHECK: return fminf(sd_seg(x, y, -0.6f, 0.05f, -0.15f, 0.5f, 0.14f), sd_seg(x, y, -0.15f, 0.5f, 0.65f, -0.45f, 0.14f));
    case K_NEWS: {   // a page: a frame with three lines
        float d = fmaxf(sd_box(x, y, 0, 0, 0.9f, 0.72f, 0.16f), -sd_box(x, y, 0, 0, 0.74f, 0.56f, 0.06f));
        for (int i = 0; i < 3; i++) d = fminf(d, sd_box(x, y, i == 2 ? -0.18f : 0, -0.3f + 0.3f * i, i == 2 ? 0.34f : 0.52f, 0.06f, 0.05f));
        return d;
    }
    case K_ALBUM: {   // a picture: a frame, a hill and a sun
        float d = fmaxf(sd_box(x, y, 0, 0, 0.9f, 0.72f, 0.16f), -sd_box(x, y, 0, 0, 0.74f, 0.56f, 0.06f));
        float hill = fmaxf(y - 0.56f, (-0.05f + 1.1f * fabsf(x + 0.15f) - y) / 1.487f);   // a triangle on the frame's floor
        d = fminf(d, fmaxf(hill, sd_box(x, y, 0, 0, 0.74f, 0.56f, 0.0f)));
        return fminf(d, sd_circle(x, y, 0.38f, -0.22f, 0.14f));
    }
    case K_STAR: {   // a five-pointed star: inside the ten-sided outline, the distance to its nearest edge
        float vx[10], vy[10];
        for (int i = 0; i < 10; i++) { float a = -1.5707963f + i * 0.62831853f, r = i % 2 ? 0.42f : 0.98f; vx[i] = r * cosf(a), vy[i] = r * sinf(a) + 0.08f; }
        float d = 1e9f; int inside = 0;
        for (int i = 0, j = 9; i < 10; j = i++) {
            d = fminf(d, sd_seg(x, y, vx[j], vy[j], vx[i], vy[i], 0));
            if ((vy[i] > y) != (vy[j] > y) && x < (vx[j] - vx[i]) * (y - vy[i]) / (vy[j] - vy[i]) + vx[i]) inside = !inside;
        }
        return inside ? -d : d;
    }
    case K_PLUS: return fminf(sd_box(x, y, 0, 0, 0.75f, 0.13f, 0.08f), sd_box(x, y, 0, 0, 0.13f, 0.75f, 0.08f));
    case K_CLOCK: {
        float d = fabsf(hypotf(x, y) - 0.78f) - 0.12f;
        d = fminf(d, sd_seg(x, y, 0, 0, 0, -0.48f, 0.1f));
        return fminf(d, sd_seg(x, y, 0, 0, 0.36f, 0.2f, 0.1f));
    }
    case K_TROPHY: {   // a cup: the bowl (a half disc under a band), two handles, the stem, the base
        float bowl = fminf(fmaxf(sd_circle(x, y, 0, -0.42f, 0.56f), -0.42f - y), sd_box(x, y, 0, -0.66f, 0.56f, 0.24f, 0.04f));
        float hand = fminf(fabsf(sd_circle(x, y, -0.6f, -0.45f, 0.22f)) - 0.075f, fabsf(sd_circle(x, y, 0.6f, -0.45f, 0.22f)) - 0.075f);
        float d = fminf(bowl, hand);
        d = fminf(d, sd_box(x, y, 0, 0.3f, 0.1f, 0.22f, 0));
        d = fminf(d, sd_box(x, y, 0, 0.56f, 0.26f, 0.07f, 0.03f));
        return fminf(d, sd_box(x, y, 0, 0.78f, 0.46f, 0.13f, 0.05f));
    }
    case K_JOY: {   // a game pad, simple: a rounded body with two grips; a d-pad and four buttons cut out
        float body = sd_box(x, y, 0, -0.14f, 0.6f, 0.3f, 0.28f);
        body = smin(body, sd_seg(x, y, -0.44f, -0.02f, -0.68f, 0.5f, 0.24f), 0.14f);
        body = smin(body, sd_seg(x, y, 0.44f, -0.02f, 0.68f, 0.5f, 0.24f), 0.14f);
        float d = body;
        d = fmaxf(d, -fminf(sd_box(x, y, -0.36f, -0.12f, 0.17f, 0.055f, 0.02f), sd_box(x, y, -0.36f, -0.12f, 0.055f, 0.17f, 0.02f)));
        d = fmaxf(d, -sd_circle(x, y, 0.36f, -0.25f, 0.062f)), d = fmaxf(d, -sd_circle(x, y, 0.36f, 0.01f, 0.062f));
        d = fmaxf(d, -sd_circle(x, y, 0.23f, -0.12f, 0.062f)), d = fmaxf(d, -sd_circle(x, y, 0.49f, -0.12f, 0.062f));
        return d;
    }
    case K_PAD_KEYS: {   // a keyboard: a slab with keys cut out
        float d = sd_box(x, y, 0, 0, 0.98f, 0.5f, 0.1f);
        for (int r = 0; r < 3; r++) for (int c = 0; c < 8; c++) d = fmaxf(d, -sd_box(x, y, -0.77f + c * 0.22f, -0.27f + r * 0.2f, 0.07f, 0.06f, 0.02f));
        return fmaxf(d, -sd_box(x, y, 0, 0.33f, 0.45f, 0.06f, 0.02f));
    }
    case K_PAD_DECK: {   // a wide handheld: the screen and the two trackpads cut out
        float d = sd_box(x, y, 0, 0, 0.98f, 0.42f, 0.3f);
        d = fmaxf(d, -sd_box(x, y, 0, -0.04f, 0.4f, 0.27f, 0.03f));
        d = fmaxf(d, -sd_circle(x, y, -0.72f, -0.12f, 0.09f)), d = fmaxf(d, -sd_circle(x, y, 0.72f, -0.12f, 0.09f));
        d = fmaxf(d, -sd_box(x, y, -0.62f, 0.2f, 0.09f, 0.09f, 0.03f)), d = fmaxf(d, -sd_box(x, y, 0.62f, 0.2f, 0.09f, 0.09f, 0.03f));
        return d;
    }
    default: {   // a game pad: a body with two grips, sticks, a d-pad and four buttons cut out
        pad_layout L = pad_lay(k);
        float body = sd_box(x, y, 0, -0.08f, 0.74f, 0.32f, 0.3f);
        body = smin(body, sd_seg(x, y, -0.52f, 0, -0.76f, 0.38f, 0.23f), 0.22f);
        body = smin(body, sd_seg(x, y, 0.52f, 0, 0.76f, 0.38f, 0.23f), 0.22f);
        float d = body;
        d = fmaxf(d, -(fabsf(sd_circle(x, y, L.lsx, L.lsy, 0.11f)) - 0.035f));
        d = fmaxf(d, -(fabsf(sd_circle(x, y, L.rsx, L.rsy, 0.11f)) - 0.035f));
        d = fmaxf(d, -fminf(sd_box(x, y, L.dpx, L.dpy, 0.13f, 0.04f, 0.015f), sd_box(x, y, L.dpx, L.dpy, 0.04f, 0.13f, 0.015f)));
        for (int i = 0; i < 4; i++) d = fmaxf(d, -sd_circle(x, y, L.btx + (i == 0 ? 0 : i == 1 ? 0.12f : i == 2 ? 0 : -0.12f), L.bty + (i == 0 ? 0.12f : i == 1 ? 0 : i == 2 ? -0.12f : 0), 0.055f));
        if (k == K_PAD_XBOX) d = fmaxf(d, -sd_circle(x, y, 0, -0.25f, 0.075f));
        if (k == K_PAD_PS) d = fmaxf(d, -sd_box(x, y, 0, -0.26f, 0.2f, 0.1f, 0.03f));
        return d;
    }
    }
}
static shape_ent g_shapes[160];
static int g_nshapes;
static void shapes_clear(void) { for (int i = 0; i < g_nshapes; i++) SDL_DestroyTexture(g_shapes[i].t); g_nshapes = 0; }
static SDL_Texture *shape_tex(int k, float w, float h, float rad) {   // sizes in design units
    int pw = (int)ceilf(w * g_s), ph = (int)ceilf(h * g_s), prad = (int)lroundf(rad * g_s * 4);
    if (pw < 1 || ph < 1) return NULL;
    for (int i = 0; i < g_nshapes; i++) if (g_shapes[i].k == k && g_shapes[i].pw == pw && g_shapes[i].ph == ph && g_shapes[i].prad == prad) return g_shapes[i].t;
    if (g_nshapes == (int)(sizeof g_shapes / sizeof *g_shapes)) shapes_clear();
    uint32_t *px = malloc((size_t)pw * ph * 4);
    // units: icons span 2 units of height, controllers 1.24, a rounded rectangle its own size in pixels
    float unit = k == K_RRECT ? 1 : (k >= K_PAD_XBOX && k <= K_PAD_KEYS) ? ph / 1.24f : ph / 2.0f;
    float wu = pw / unit, hu = ph / unit, r = rad * g_s / unit;
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            float ux = (x + 0.5f) / unit - wu / 2, uy = (y + 0.5f) / unit - hu / 2;
            float d = shape_d(k, ux, uy, wu, hu, r) * unit;   // back to pixels
            float a = fminf(1, fmaxf(0, 0.5f - d));
            px[y * pw + x] = (uint32_t)(a * 255 + 0.5f) << 24 | 0xFFFFFF;
        }
    SDL_Texture *t = SDL_CreateTexture(g_r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, pw, ph);
    SDL_UpdateTexture(t, NULL, px, pw * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    free(px);
    g_shapes[g_nshapes++] = (shape_ent){k, pw, ph, prad, t};
    return t;
}
// Draws a shape (a rounded rectangle, an icon) in a colour, from its texture made at the drawn size.
void shape(int k, float x, float y, float w, float h, float rad, rgba c) {
    SDL_Texture *t = shape_tex(k, w, h, rad);
    if (!t) return;
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
    float tw, th;
    SDL_GetTextureSize(t, &tw, &th);
    SDL_FRect dst = {x, y, tw / g_s, th / g_s};
    SDL_RenderTexture(g_r, t, NULL, &dst);
}
void rrect(float x, float y, float w, float h, float rad, rgba c) {
    if (rad > w / 2) rad = w / 2;
    if (rad > h / 2) rad = h / 2;
    shape(K_RRECT, x, y, w, h, rad, c);
}
void circle(float cx, float cy, float rad, rgba c) { rrect(cx - rad, cy - rad, 2 * rad, 2 * rad, rad, c); }
void icon(int k, float cx, float cy, float r, rgba c) { shape(k, cx - r, cy - r, 2 * r, 2 * r, 0, c); }
void icon_folder(float cx, float cy, float r, rgba c) { icon(K_FOLDER, cx, cy, r, c); }
pic_t *g_pics;   // one per game: its tile picture and its background
pic_t g_custom_bg;
// A texture from RGBA pixels (smoothed when scaled, or not).
SDL_Texture *tex_from_rgba(const uint8_t *px, int w, int h, int linear) {
    SDL_Texture *t = SDL_CreateTexture(g_r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);   // bytes R G B A
    if (!t) return NULL;
    SDL_UpdateTexture(t, NULL, px, w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, linear ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    return t;
}
// an area average down to dw x dh (alpha-weighted, so no dark fringes); RGBA bytes in and out
uint8_t *img_down(const uint8_t *px, int w, int h, int dw, int dh) {
    uint8_t *o = malloc((size_t)dw * dh * 4);
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long)x * w / dw), x1 = (int)((long)(x + 1) * w / dw), y0 = (int)((long)y * h / dh), y1 = (int)((long)(y + 1) * h / dh);
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            float acc[4] = {0}, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    const uint8_t *q = px + ((size_t)yy * w + xx) * 4;
                    float al = q[3] / 255.f;
                    acc[0] += q[0] * al, acc[1] += q[1] * al, acc[2] += q[2] * al, acc[3] += al, n++;
                }
            uint8_t *d = o + ((size_t)y * dw + x) * 4;
            for (int k = 0; k < 3; k++) d[k] = (uint8_t)(acc[3] > 0 ? acc[k] / acc[3] + 0.5f : 0);
            d[3] = (uint8_t)(acc[3] / n * 255 + 0.5f);
        }
    return o;
}
// a cubic (Catmull-Rom) resize to dw x dh, in two passes (across, then down)
static float cubic_w(float t) {
    t = fabsf(t);
    return t < 1 ? 1.5f * t * t * t - 2.5f * t * t + 1 : t < 2 ? -0.5f * t * t * t + 2.5f * t * t - 4 * t + 2 : 0;
}
// Enlarges a picture with a cubic (Catmull-Rom) filter, rows then columns.
static uint8_t *img_cubic(const uint8_t *px, int w, int h, int dw, int dh) {
    float *mid = malloc((size_t)dw * h * 4 * sizeof *mid);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < dw; x++) {
            float sx = (x + 0.5f) * w / dw - 0.5f, acc[4] = {0}, ws = 0;
            int ix = (int)floorf(sx);
            for (int k = -1; k <= 2; k++) {
                int xx = ix + k < 0 ? 0 : ix + k >= w ? w - 1 : ix + k;
                float wt = cubic_w(sx - (ix + k));
                const uint8_t *q = px + ((size_t)y * w + xx) * 4;
                for (int c = 0; c < 4; c++) acc[c] += q[c] * wt;
                ws += wt;
            }
            for (int c = 0; c < 4; c++) mid[((size_t)y * dw + x) * 4 + c] = acc[c] / ws;
        }
    uint8_t *o = malloc((size_t)dw * dh * 4);
    for (int y = 0; y < dh; y++) {
        float sy = (y + 0.5f) * h / dh - 0.5f;
        int iy = (int)floorf(sy);
        for (int x = 0; x < dw; x++) {
            float acc[4] = {0}, ws = 0;
            for (int k = -1; k <= 2; k++) {
                int yy = iy + k < 0 ? 0 : iy + k >= h ? h - 1 : iy + k;
                float wt = cubic_w(sy - (iy + k));
                const float *q = mid + ((size_t)yy * dw + x) * 4;
                for (int c = 0; c < 4; c++) acc[c] += q[c] * wt;
                ws += wt;
            }
            for (int c = 0; c < 4; c++) { float v = acc[c] / ws; o[((size_t)y * dw + x) * 4 + c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f); }
        }
    }
    free(mid);
    return o;
}
// a 3 x 3 blur (box, or the gentler 1-2-1 kernel), in place
static void img_blur3(uint8_t *px, int w, int h, int gentle) {
    uint8_t *t = malloc((size_t)w * h * 4);
    static const int k121[3] = {1, 2, 1}, k111[3] = {1, 1, 1};
    const int *k = gentle ? k121 : k111;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 4; c++) {
                int s = 0, n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = x + dx < 0 ? 0 : x + dx >= w ? w - 1 : x + dx, yy = y + dy < 0 ? 0 : y + dy >= h ? h - 1 : y + dy;
                        int wt = k[dx + 1] * k[dy + 1];
                        s += px[((size_t)yy * w + xx) * 4 + c] * wt, n += wt;
                    }
                t[((size_t)y * w + x) * 4 + c] = (uint8_t)((s + n / 2) / n);
            }
    memcpy(px, t, (size_t)w * h * 4);
    free(t);
}
// the background of a picture, as described above; tw: the window's width in pixels
static uint8_t *make_bg(const uint8_t *px, int w, int h, int sharp, int blur, int tw, int *ow, int *oh) {
    if (sharp || !blur) {
        int dw = tw < 320 ? 320 : tw > 2560 ? 2560 : tw, dh = (int)((double)dw * h / w + 0.5);
        if (w >= dw) { *ow = dw, *oh = dh; return img_down(px, w, h, dw, dh); }
        uint8_t *c = malloc((size_t)w * h * 4);
        memcpy(c, px, (size_t)w * h * 4);
        if (w * 2 < dw) img_blur3(c, w, h, 1);   // much enlarged: the JPEG's blocks would show
        uint8_t *o = img_cubic(c, w, h, dw, dh);
        free(c);
        *ow = dw, *oh = dh;
        return o;
    }
    static const int widths[4] = {0, 240, 128, 64}, passes[4] = {0, 1, 2, 3};
    int lv = blur > 3 ? 3 : blur, bw = widths[lv] < w ? widths[lv] : w, bh = (int)((double)bw * h / w + 0.5);
    if (bh < 4) bh = 4;
    uint8_t *small = img_down(px, w, h, bw, bh);
    for (int i = 0; i < passes[lv]; i++) img_blur3(small, bw, bh, 0);
    for (int i = 0; i < bw * bh; i++) small[i * 4 + 3] = 255;
    int dw = 640, dh = (int)((double)dw * bh / bw + 0.5);
    uint8_t *o = img_cubic(small, bw, bh, dw, dh);
    free(small);
    *ow = dw, *oh = dh;
    return o;
}
static bg_job g_bgq[24];
int g_nbgq;
unsigned g_bg_gen = 1;   // bumped when the games are scanned again or the blur changes: older results are dropped
SDL_Mutex *g_bg_mu;
static SDL_Condition *g_bg_cv;
// The background thread: makes the jobs' backgrounds (sharp or blurred, at the window's size) one at a time.
static int bg_thread(void *arg) {
    (void)arg;
    SDL_LockMutex(g_bg_mu);
    for (;;) {
        int k = -1;
        for (int i = 0; i < g_nbgq && k < 0; i++) if (!g_bgq[i].done && !g_bgq[i].px) k = i;
        if (k < 0) { SDL_WaitCondition(g_bg_cv, g_bg_mu); continue; }
        bg_job j = g_bgq[k];
        g_bgq[k].done = -1;   // taken
        SDL_UnlockMutex(g_bg_mu);
        int w, h, ch;
        uint8_t *px = stbi_load(j.file, &w, &h, &ch, 4), *bg = NULL;
        if (px) bg = make_bg(px, w, h, j.sharp, j.blur, j.tw, &j.w, &j.h), stbi_image_free(px);
        SDL_LockMutex(g_bg_mu);
        for (int i = 0; i < g_nbgq; i++)
            if (g_bgq[i].gi == j.gi && g_bgq[i].gen == j.gen && g_bgq[i].done == -1) { g_bgq[i].px = bg, g_bgq[i].w = j.w, g_bgq[i].h = j.h, g_bgq[i].done = 1; bg = NULL; break; }
        free(bg);
    }
    return 0;
}
// ask for game gi's background (gi -1: the one chosen in the settings)
void bg_ask(pic_t *p, int gi, const char *file, const char *bg_file) {
    if (p->asked) return;
    const char *f = bg_file && *bg_file ? bg_file : file;
    if (!f || !*f) { p->asked = 1; return; }
    SDL_LockMutex(g_bg_mu);
    if (g_nbgq < (int)(sizeof g_bgq / sizeof *g_bgq)) {
        bg_job *j = &g_bgq[g_nbgq++];
        memset(j, 0, sizeof *j);
        j->gi = gi, j->gen = g_bg_gen, j->sharp = bg_file && *bg_file, j->blur = g_cfg.blur, j->tw = win_px_w();
        snprintf(j->file, sizeof j->file, "%s", f);
        p->asked = 1;
        SDL_SignalCondition(g_bg_cv);
    }
    SDL_UnlockMutex(g_bg_mu);
}
// the finished ones become textures
void bg_collect(void) {
    SDL_LockMutex(g_bg_mu);
    for (int i = 0; i < g_nbgq;) {
        bg_job *j = &g_bgq[i];
        if (j->done == 1 || (j->done == 0 && j->gen != g_bg_gen)) {
            if (j->px && j->gen == g_bg_gen) {
                pic_t *p = j->gi < 0 ? &g_custom_bg : j->gi < g_ngames ? &g_pics[j->gi] : NULL;
                if (p) { if (p->bg) SDL_DestroyTexture(p->bg); p->bg = tex_from_rgba(j->px, j->w, j->h, 1); }
            }
            free(j->px);
            g_bgq[i] = g_bgq[--g_nbgq];
            continue;
        }
        i++;
    }
    SDL_UnlockMutex(g_bg_mu);
}
void bg_init(void) {
    g_bg_mu = SDL_CreateMutex(), g_bg_cv = SDL_CreateCondition();
    SDL_DetachThread(SDL_CreateThread(bg_thread, "backgrounds", NULL));
}
// the tile picture: at most 512 pixels across (a large cover averaged down: drawn smaller still, it stays smooth)
void pic_load(pic_t *p, const char *file, const char *bg_file) {
    (void)bg_file;
    if (p->tried) return;
    p->tried = 1;
    int w, h, ch;
    uint8_t *px = file && *file ? stbi_load(file, &w, &h, &ch, 4) : NULL;
    if (!px) { if (file && *file) LOG("cannot read the picture %s", file); return; }
    if (w > 512) {
        int dw = 512, dh = (int)((double)512 * h / w + 0.5);
        uint8_t *d = img_down(px, w, h, dw, dh);
        p->tex = tex_from_rgba(d, dw, dh, 1);
        free(d);
    } else p->tex = tex_from_rgba(px, w, h, 1);
    stbi_image_free(px);
}
// fill the rectangle with the texture, cropped to keep its shape
void draw_cover(SDL_Texture *t, float x, float y, float w, float h, uint8_t alpha) {
    float tw, th;
    SDL_GetTextureSize(t, &tw, &th);
    float s = fmaxf(w / tw, h / th), sw = w / s, sh = h / s;
    SDL_FRect src = {(tw - sw) / 2, (th - sh) / 2, sw, sh}, dst = {x, y, w, h};
    SDL_SetTextureAlphaMod(t, alpha);
    SDL_RenderTexture(g_r, t, &src, &dst);
}
// a game without a picture: a colour from its title, and its initials
rgba title_color(const char *t) {
    uint32_t h = 2166136261u;
    for (; *t; t++) h = (h ^ (uint8_t)*t) * 16777619u;
    float hue = (h % 360) / 60.0f;
    float x = 1 - fabsf(fmodf(hue, 2) - 1);
    float r = 0, g = 0, b = 0;
    int k = (int)hue;
    if (k == 0) r = 1, g = x; else if (k == 1) r = x, g = 1; else if (k == 2) g = 1, b = x; else if (k == 3) g = x, b = 1; else if (k == 4) r = x, b = 1; else r = 1, b = x;
    return (rgba){(uint8_t)(40 + 120 * r), (uint8_t)(40 + 120 * g), (uint8_t)(40 + 120 * b), 255};
}
// Draws a game's tile: its picture with rounded corners, the selection's frame and glow when selected.
void draw_tile(int gi, float x, float y, float size, int selected, float glow) {
    pic_t *p = &g_pics[gi];
    pic_load(p, g_games[gi].icon, g_games[gi].background);
    if (selected) {   // the selection: a bright frame and a soft glow that breathes
        for (int k = 4; k >= 1; k--) rrect(x - 4 - 3 * k, y - 4 - 3 * k, size + 8 + 6 * k, size + 8 + 6 * k, 10 + 3 * k, (rgba){C_ACCENT.r, C_ACCENT.g, C_ACCENT.b, (uint8_t)(18 * glow)});
        rrect(x - 6, y - 6, size + 12, size + 12, 10, C_ACCENT);
    }
    if (p->tex) draw_cover(p->tex, x, y, size, size, 255);
    else {
        rrect(x, y, size, size, 6, title_color(g_games[gi].title));
        char ini[8] = {0};
        int n = 0;
        for (const char *t = g_games[gi].title; *t && n < 3; t++) if ((t == g_games[gi].title || t[-1] == ' ') && ((*t >= 'A' && *t <= 'Z') || (*t >= '0' && *t <= '9') || (*t >= 'a' && *t <= 'z'))) ini[n++] = *t;
        int fid = size > 200 ? F_BIG : F_TITLE;
        text(fid, x + (size - text_w(fid, ini)) / 2, y + size / 2 - FONT_PX[fid] * 0.6f, (rgba){255, 255, 255, 230}, ini);
    }
    if (game_waiting(gi)) {   // the game waits at NativeNX's Home (its menu): a mark on its picture
        const char *l = "Paused";
        float fs = size > 200 ? 1 : 0.8f, pw = (text_w(F_SMALL, l) + 40) * fs, ph = 30 * fs;
        rrect(x + 10, y + size - ph - 10, pw, ph, ph / 2, (rgba){20, 24, 34, 220});
        rrect(x + 10 + 12 * fs, y + size - ph - 10 + 9 * fs, 4 * fs, 12 * fs, 1, C_TEXT);
        rrect(x + 10 + 19 * fs, y + size - ph - 10 + 9 * fs, 4 * fs, 12 * fs, 1, C_TEXT);
        if (fs == 1) text(F_SMALL, x + 10 + 32, y + size - ph - 10 + 4, C_TEXT, l);
    }
    if (is_fav(gi)) {   // a favourite: a star in the corner
        float r = size > 200 ? 20 : 15, cx = x + size - r - 8, cy = y + r + 8;
        circle(cx, cy, r, (rgba){20, 24, 34, 200});
        icon(K_STAR, cx, cy, r * 0.72f, (rgba){255, 210, 60, 255});
    }
}
