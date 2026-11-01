// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX, the launcher's own files: sounds and button pictures (launcher/data), and NativeNX in the desktop's app menu (nativenx.h lists the parts).
#include "nativenx.h"

// ---------- the launcher's own files: sounds and button pictures (launcher/data, Kenney's, CC0) ----------
// found by NXL_LAUNCHER_DATA, else the source folder it was built from, else next to the program
char g_data[1024];
// Finds the launcher's data folder (NXL_LAUNCHER_DATA, beside the program, or the source tree's).
void data_find(void) {
    const char *e = getenv("NXL_LAUNCHER_DATA");
    char exe[1024] = "", c[4][1100];
    nxl_exe_dir(exe, sizeof exe);
    snprintf(c[0], sizeof c[0], "%s", e ? e : "");
#ifdef NXL_LAUNCHER_DATA_DIR
    snprintf(c[1], sizeof c[1], "%s", NXL_LAUNCHER_DATA_DIR);   // the source tree's (a build)
#else
    snprintf(c[1], sizeof c[1], "%s/data", exe);   // beside the program (Windows: the zip's folder)
#endif
    snprintf(c[2], sizeof c[2], "%s/../share/nativenx", exe);   // an install
    snprintf(c[3], sizeof c[3], "%s/../launcher/data", exe);    // a build folder in the source tree
    for (int i = 0; i < 4; i++) {
        char probe[1200];
        snprintf(probe, sizeof probe, "%s/sounds/move.wav", c[i]);
        if (c[i][0] && !access(probe, R_OK)) { snprintf(g_data, sizeof g_data, "%s", c[i]); return; }
    }
    LOG("the launcher's data folder (launcher/data) was not found: no sounds, drawn button icons");
}
static const char *const SND_FILE[SND_N] = {"move", "bump", "confirm", "back", "open", "close", "launch", "toggle", "error", "achievement"};
static const float SND_GAIN[SND_N] = {0.45f, 0.6f, 0.8f, 0.8f, 0.7f, 0.7f, 0.9f, 0.7f, 0.8f, 0.9f};   // the move tick quieter: it plays often
static SDL_AudioDeviceID g_adev;
static SDL_AudioSpec g_aout;
static SDL_AudioStream *g_snd[SND_N];
static Uint8 *g_snd_buf[SND_N];
static Uint32 g_snd_len[SND_N];
static int g_snd_synth[SND_N];   // made here (malloc), not loaded (SDL_free)
static const struct { int n; note_t v[4]; } SYNTH[SND_N] = {
    [SND_MOVE] = {1, {{0, 1250, 1150, 45, 11, 0.55f}}},                                            // a soft tick
    [SND_BUMP] = {1, {{0, 230, 180, 90, 26, 0.8f}}},                                               // a dull knock
    [SND_CONFIRM] = {2, {{0, 990, 990, 70, 22, 0.45f}, {45, 1480, 1480, 120, 38, 0.45f}}},         // up a fifth
    [SND_BACK] = {2, {{0, 1320, 1320, 70, 22, 0.42f}, {45, 880, 880, 120, 38, 0.42f}}},            // down
    [SND_OPEN] = {2, {{0, 520, 880, 150, 70, 0.45f}, {10, 1040, 1760, 140, 55, 0.12f}}},           // a rising swish
    [SND_CLOSE] = {2, {{0, 880, 520, 150, 70, 0.45f}, {10, 1760, 1040, 140, 55, 0.12f}}},
    [SND_LAUNCH] = {4, {{0, 660, 660, 240, 90, 0.4f}, {65, 990, 990, 240, 90, 0.4f}, {130, 1320, 1320, 320, 120, 0.42f}, {130, 2640, 2640, 260, 90, 0.06f}}},
    [SND_TOGGLE] = {2, {{0, 1500, 1500, 40, 10, 0.4f}, {32, 1000, 1000, 55, 15, 0.4f}}},
    [SND_ERROR] = {2, {{0, 330, 330, 130, 60, 0.5f}, {115, 262, 262, 180, 70, 0.5f}}},
    [SND_ACH] = {4, {{0, 1568, 1568, 380, 150, 0.38f}, {0, 3136, 3136, 260, 80, 0.07f}, {90, 2093, 2093, 620, 230, 0.36f}, {90, 4186, 4186, 320, 90, 0.05f}}},   // a bright two-note chime
};
// Makes a soft sound (sine tones with a gentle attack and fade) into a buffer, for the default sound style.
static void synth(int i, Uint8 **buf, Uint32 *len, SDL_AudioSpec *spec) {
    const int rate = 48000;
    float end = 0;
    for (int k = 0; k < SYNTH[i].n; k++) end = fmaxf(end, SYNTH[i].v[k].at + SYNTH[i].v[k].len);
    int n = (int)(end * rate / 1000) + 1;
    float *o = calloc((size_t)n, sizeof *o);
    for (int k = 0; k < SYNTH[i].n; k++) {
        const note_t *v = &SYNTH[i].v[k];
        int s0 = (int)(v->at * rate / 1000), sn = (int)(v->len * rate / 1000);
        double ph = 0;
        for (int j = 0; j < sn && s0 + j < n; j++) {
            float t = j * 1000.f / rate, u = (float)j / sn;
            float f = v->f0 * powf(v->f1 / v->f0, u);   // a glide, evenly in pitch
            ph += 2 * M_PI * f / rate;
            float att = t < 3 ? 0.5f - 0.5f * cosf((float)M_PI * t / 3) : 1;   // 3 ms in: no click
            float rel = v->len - t < 8 ? (v->len - t) / 8 : 1;                    // and 8 ms out
            float env = att * rel * expf(-t / v->tau);
            o[s0 + j] += v->amp * env * (float)(sin(ph) + 0.18 * sin(2 * ph));
        }
    }
    float lp = 0, a = 1 - expf(-2 * (float)M_PI * 5000 / rate), pk = 0;   // a one-pole low-pass at 5 kHz
    for (int j = 0; j < n; j++) lp += a * (o[j] - lp), o[j] = lp, pk = fmaxf(pk, fabsf(lp));
    if (pk > 0.85f) for (int j = 0; j < n; j++) o[j] *= 0.85f / pk;
    *buf = (Uint8 *)o, *len = (Uint32)(n * sizeof *o);
    *spec = (SDL_AudioSpec){SDL_AUDIO_F32, 1, rate};
}
// --export-sounds DIR: the soft sounds as 16-bit WAV files (to hear them, or to start a set of one's own from)
int export_sounds(const char *dir) {
    mkdir(dir, 0755);
    for (int i = 0; i < SND_N; i++) {
        Uint8 *b; Uint32 len; SDL_AudioSpec sp;
        synth(i, &b, &len, &sp);
        int n = (int)(len / 4);
        char p[1200];
        snprintf(p, sizeof p, "%s/%s.wav", dir, SND_FILE[i]);
        FILE *f = fopen(p, "wb");
        if (!f) { free(b); return 1; }
        uint32_t data = (uint32_t)n * 2, riff = 36 + data, fmt = 16, rate = 48000, brate = 96000;
        uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
        fwrite("RIFF", 1, 4, f), fwrite(&riff, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f), fwrite(&fmt, 4, 1, f), fwrite(&pcm, 2, 1, f), fwrite(&ch, 2, 1, f);
        fwrite(&rate, 4, 1, f), fwrite(&brate, 4, 1, f), fwrite(&align, 2, 1, f), fwrite(&bits, 2, 1, f), fwrite("data", 1, 4, f), fwrite(&data, 4, 1, f);
        for (int j = 0; j < n; j++) { float v = ((float *)b)[j] * 32767; int16_t q = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); fwrite(&q, 2, 1, f); }
        fclose(f), free(b);
    }
    printf("the soft sounds written to %s\n", dir);
    return 0;
}
void own_sounds_dir(char *out, size_t n) { nx_dir(1, "sounds", out, n); }
// the sounds of the chosen style (g_cfg.sound_style); any that style lacks are the soft ones
void sound_load(void) {
    if (!g_adev) return;
    for (int i = 0; i < SND_N; i++) {
        if (g_snd[i]) SDL_DestroyAudioStream(g_snd[i]), g_snd[i] = NULL;
        if (g_snd_buf[i]) { if (g_snd_synth[i]) free(g_snd_buf[i]); else SDL_free(g_snd_buf[i]); g_snd_buf[i] = NULL; }
    }
    char dir[1100] = "";
    if (g_cfg.sound_style == 1 && g_data[0]) snprintf(dir, sizeof dir, "%s/sounds", g_data);
    else if (g_cfg.sound_style == 2) own_sounds_dir(dir, sizeof dir);
    int files = 0;
    for (int i = 0; i < SND_N; i++) {
        SDL_AudioSpec spec;
        char p[1200];
        snprintf(p, sizeof p, "%s/%s.wav", dir, SND_FILE[i]);
        g_snd_synth[i] = !(dir[0] && SDL_LoadWAV(p, &spec, &g_snd_buf[i], &g_snd_len[i]));
        if (g_snd_synth[i]) synth(i, &g_snd_buf[i], &g_snd_len[i], &spec); else files++;
        g_snd[i] = SDL_CreateAudioStream(&spec, &g_aout);
        if (g_snd[i]) SDL_BindAudioStream(g_adev, g_snd[i]);
    }
    static const char *const style[3] = {"soft", "Kenney's", "your own"};
    LOG("sounds: %s (%d from files in %s, %d made here), %s", style[g_cfg.sound_style % 3], files, dir[0] ? dir : "-", SND_N - files, g_cfg.sounds ? "on" : "off in the settings");
}
// Opens the sound output and loads the sounds: your own (in the sounds folder), else the ones made here.
void sound_init(void) {
    {   // the folder for your own sounds, made ready (it was left to the user to make)
        char d[1100];
        own_sounds_dir(d, sizeof d);
        for (char *q = d + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(d, 0755); *q = '/'; }
        mkdir(d, 0755);
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    g_adev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
    if (!g_adev) { LOG("no sound output: %s", SDL_GetError()); return; }
    SDL_GetAudioDeviceFormat(g_adev, &g_aout, NULL);
    sound_load();
}
void sound(int i) {
    if (!g_cfg.sounds || i < 0 || i >= SND_N || !g_snd[i]) return;
    SDL_ClearAudioStream(g_snd[i]);
    SDL_SetAudioStreamGain(g_snd[i], (g_snd_synth[i] ? (i == SND_MOVE ? 0.7f : 1.0f) : SND_GAIN[i]) * g_cfg.volume / 100.f);
    SDL_PutAudioStreamData(g_snd[i], g_snd_buf[i], (int)g_snd_len[i]);
}
static prompt_t g_prompts[40];
static int g_nprompts;
void prompts_clear(void) { for (int i = 0; i < g_nprompts; i++) if (g_prompts[i].t) SDL_DestroyTexture(g_prompts[i].t), g_prompts[i].t = NULL; }
// A button picture (a controller's A, B, ... or a key) at the size drawn, made once per size.
SDL_Texture *prompt_tex(const char *name, float size) {
    if (!g_data[0] || !name) return NULL;
    prompt_t *p = NULL;
    for (int i = 0; i < g_nprompts; i++) if (!strcmp(g_prompts[i].name, name)) p = &g_prompts[i];
    if (!p) {
        if (g_nprompts == (int)(sizeof g_prompts / sizeof *g_prompts)) return NULL;
        p = &g_prompts[g_nprompts++];
        memset(p, 0, sizeof *p);
        snprintf(p->name, sizeof p->name, "%s", name);
        char f[1200];
        int ch;
        snprintf(f, sizeof f, "%s/prompts/%s.png", g_data, name);
        p->px = stbi_load(f, &p->w, &p->h, &ch, 4);
    }
    if (!p->px) return NULL;
    int pw = (int)lroundf(size * g_s);
    if (pw < 4) pw = 4;
    if (p->t && p->pw == pw) return p->t;
    if (p->t) SDL_DestroyTexture(p->t);
    int ph = pw * p->h / p->w;
    uint8_t *o = malloc((size_t)pw * ph * 4);
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            int x0 = x * p->w / pw, x1 = (x + 1) * p->w / pw, y0 = y * p->h / ph, y1 = (y + 1) * p->h / ph;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            float acc[4] = {0}, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    const uint8_t *q = p->px + ((size_t)yy * p->w + xx) * 4;
                    float a = q[3] / 255.f;   // premultiplied while averaging: no dark fringes
                    acc[0] += q[0] * a, acc[1] += q[1] * a, acc[2] += q[2] * a, acc[3] += a, n++;
                }
            uint8_t *d = o + ((size_t)y * pw + x) * 4;
            float a = acc[3] / n;
            for (int k = 0; k < 3; k++) d[k] = (uint8_t)(acc[3] > 0 ? acc[k] / acc[3] : 0);
            d[3] = (uint8_t)(a * 255 + 0.5f);
        }
    p->t = tex_from_rgba(o, pw, ph, 1);
    p->pw = pw;
    free(o);
    return p->t;
}
int prompt(const char *name, float x, float y, float size, uint8_t alpha) {   // 1 if drawn
    SDL_Texture *t = prompt_tex(name, size);
    if (!t) return 0;
    float tw, th;
    SDL_GetTextureSize(t, &tw, &th);
    SDL_SetTextureAlphaMod(t, alpha);
    SDL_FRect d = {x, y, tw / g_s, th / g_s};
    SDL_RenderTexture(g_r, t, NULL, &d);
    return 1;
}

// ---------- NativeNX in the desktop's app menu (--install, Settings > About) ----------
#ifdef _WIN32
// A shortcut in the Start menu's Programs (its icon is the program's own, packaging/windows/nativenx.rc)
static void start_menu_link(char *out, size_t n) {
    const char *a = getenv("APPDATA");
    snprintf(out, n, "%s/Microsoft/Windows/Start Menu/Programs/NativeNX.lnk", a && *a ? a : ".");
}
int desktop_installed(void) {
    char p[1100];
    start_menu_link(p, sizeof p);
    return !access(p, F_OK);
}
int desktop_install(void) {
    char exe[1100], p[1100];
    if (nxl_exe_dir(exe, sizeof exe)) return 1;
    strncat(exe, "/nativenx.exe", sizeof exe - strlen(exe) - 1);
    start_menu_link(p, sizeof p);
    if (nxl_shortcut(exe, p, "Play your Switch games")) { LOG("could not make the shortcut %s", p); return 1; }
    LOG("added to the Start menu: %s (starts %s)", p, exe);
    return 0;
}
#else
// ~/.local/share/applications/nativenx.desktop starting this program, and its icon in the hicolor theme (the
// launcher's own app id is "nativenx", so the desktop shows that icon on its window too)
static void data_home(char *out, size_t n) { nx_data_home(out, n); }
static void mkpath(const char *p) { nx_mkdirs(p); }
int desktop_installed(void) {
    char d[1000], p[1100];
    data_home(d, sizeof d);
    snprintf(p, sizeof p, "%s/applications/nativenx.desktop", d);
    return !access(p, F_OK);
}
// Copies a file; 0 on success.
static int copy_file(const char *from, const char *to) {
    FILE *i = fopen(from, "rb"), *o = i ? fopen(to, "wb") : NULL;
    char b[65536];
    size_t n;
    int ok = i && o;
    while (ok && (n = fread(b, 1, sizeof b, i)) > 0) ok = fwrite(b, 1, n, o) == n;
    if (i) fclose(i);
    if (o) ok = !fclose(o) && ok;
    return ok;
}
// Puts NativeNX in the desktop's app menu (a .desktop file and its icon); 0 on success.
int desktop_install(void) {
    char d[1000], exe[1024], dir[1100], p[1200], icon[1200];
    data_home(d, sizeof d);
    const char *appimage = getenv("APPIMAGE");   // started from the AppImage: the menu starts the AppImage (its own folder is gone after)
    if (appimage && *appimage) snprintf(exe, sizeof exe, "%s", appimage);
    else {
        ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
        exe[k > 0 ? k : 0] = 0;
    }
    snprintf(dir, sizeof dir, "%s/icons/hicolor/512x512/apps", d);
    mkpath(dir);
    snprintf(icon, sizeof icon, "%s/nativenx-icon.png", g_data);
    snprintf(p, sizeof p, "%s/nativenx.png", dir);
    if (!g_data[0] || !copy_file(icon, p)) { LOG("could not copy the icon %s to %s", icon, p); return 1; }
    snprintf(dir, sizeof dir, "%s/applications", d);
    mkpath(dir);
    snprintf(p, sizeof p, "%s/nativenx.desktop", dir);
    FILE *f = fopen(p, "w");
    if (!f) { LOG("could not write %s", p); return 1; }
    fprintf(f, "[Desktop Entry]\nType=Application\nName=NativeNX\nComment=Play your Switch games\nExec=\"%s\"\nIcon=nativenx\n"
               "Terminal=false\nCategories=Game;\nStartupWMClass=nativenx\n", exe);
    fclose(f);
    if (system("update-desktop-database -q \"$HOME/.local/share/applications\" >/dev/null 2>&1; gtk-update-icon-cache -q -t \"$HOME/.local/share/icons/hicolor\" >/dev/null 2>&1")) {}
    LOG("added to the app menu: %s (starts %s)", p, exe);
    return 0;
}
#endif
// Sets the window's icon.
void window_icon(void) {
    char p[1200];
    int w, h, ch;
    snprintf(p, sizeof p, "%s/nativenx-icon.png", g_data);
    uint8_t *px = g_data[0] ? stbi_load(p, &w, &h, &ch, 4) : NULL;
    if (!px) return;
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888, px, w * 4);
    if (s) SDL_SetWindowIcon(g_win, s), SDL_DestroySurface(s);
    stbi_image_free(px);
}
// the logo at start: shown, then fading into the dashboard (any button skips it)
uint64_t g_splash_t0;
int g_splash_on, g_splash_hold;   // hold: --splash (pictures), the logo held at full
// Draws the start-up splash (the logo, fading out), until a button is pressed or it has faded.
void draw_splash(void) {
    if (!g_splash_on) return;
    float t = g_splash_hold ? 0.5f : (SDL_GetTicks() - g_splash_t0) / 1000.f, a = t < 0.7f ? 1 : 1 - (t - 0.7f) / 0.45f;
    if (a <= 0) { g_splash_on = 0; return; }
    SDL_FRect all = {0, 0, g_vw, g_vh};
    SDL_SetRenderDrawColor(g_r, 18, 21, 30, (uint8_t)(255 * a));
    SDL_RenderFillRect(g_r, &all);
    float in = t < 0.35f ? t / 0.35f : 1, sz = 200 + 20 * in;   // the logo grows in a little
    prompt("../nativenx-logo", (g_vw - sz) / 2, g_vh / 2 - sz / 2 - 40, sz, (uint8_t)(255 * a * in));
    text(F_TITLE, (g_vw - text_w(F_TITLE, "NativeNX")) / 2, g_vh / 2 + sz / 2 - 10, (rgba){255, 255, 255, (uint8_t)(255 * a * in)}, "NativeNX");
}
