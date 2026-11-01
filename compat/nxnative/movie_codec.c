// SPDX-License-Identifier: GPL-3.0-or-later
// The movie decoders on Linux (see nxl_movie_codec.h): FFmpeg's libavcodec, opened with dlopen. Only a few of its
// functions and the start of two of its structures are used, which have kept their layout from FFmpeg 4 to 8:
//   AVPacket: buf, pts, dts, data, size      AVFrame: data[8], linesize[8], extended_data, width, height, nb_samples, format
// Times are not read back from FFmpeg's frames (the field moved between versions): pictures come out in presentation
// order, so each takes the smallest time among the packets still inside the decoder.
// NXL_LIBAVCODEC=<path> picks the library (with NXL_LIBAVUTIL for its companion); otherwise libavcodec.so.62 down to .58.
#define _GNU_SOURCE
#include "nxl_movie_codec.h"
#include "hle_movie_avc.h"   // nxnative's H.264 interface (implemented here instead of on VideoToolbox)
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MLOG(...) do { fprintf(stderr, "[movie] " __VA_ARGS__); fputc('\n', stderr); } while (0)

// ---------- libavcodec, loaded on first use ----------
typedef struct {
    void *buf; int64_t pts, dts; uint8_t *data; int size;
} av_packet_head;
typedef struct {
    uint8_t *data[8]; int linesize[8]; uint8_t **extended_data; int width, height, nb_samples, format;
} av_frame_head;
enum { PIX_YUV420P = 0, PIX_YUVJ420P = 12, PIX_NV12 = 23 };
enum { SMP_S16 = 1, SMP_FLT = 3, SMP_S16P = 6, SMP_FLTP = 8 };
#define AVERROR_EAGAIN (-11)
#define AVERROR_EOF (-0x20464f45)   // FFERRTAG('E','O','F',' ')
#define PAD 64                      // AV_INPUT_BUFFER_PADDING_SIZE

static struct {
    int tried, ok;
    char name[160];
    const void *(*find_by_name)(const char *);
    void *(*alloc_ctx)(const void *);
    int (*open2)(void *, const void *, void **);
    void (*free_ctx)(void **);
    int (*send)(void *, const void *);
    int (*recv)(void *, void *);
    void (*flush)(void *);
    void *(*pkt_alloc)(void);
    void (*pkt_free)(void **);
    void *(*frm_alloc)(void);
    void (*frm_free)(void **);
    void (*frm_unref)(void *);
    int (*opt_int)(void *, const char *, int64_t, int);
} F;
static pthread_mutex_t g_load_mu = PTHREAD_MUTEX_INITIALIZER;

// Opens an FFmpeg library: the file the environment variable names, else the newest of its usual names.
static void *open_lib(const char *env, const char *stem, int hi, int lo) {
    const char *e = getenv(env);
    if (e && *e) { void *h = dlopen(e, RTLD_NOW | RTLD_LOCAL); if (!h) MLOG("[RED] %s=%s cannot be opened: %s", env, e, dlerror()); return h; }
    char n[64];
    for (int v = hi; v >= lo; v--) { snprintf(n, sizeof n, "%s.so.%d", stem, v); void *h = dlopen(n, RTLD_NOW | RTLD_LOCAL); if (h) return h; }
    return NULL;
}
// Loads FFmpeg's libavcodec and libavutil and the functions used from them, once; 0 when it is there.
static int load(void) {
    pthread_mutex_lock(&g_load_mu);
    if (!F.tried) {
        F.tried = 1;
        void *avu = open_lib("NXL_LIBAVUTIL", "libavutil", 60, 56);
        void *avc = open_lib("NXL_LIBAVCODEC", "libavcodec", 62, 58);
        if (!avc || !avu) MLOG("[RED] no FFmpeg (libavcodec / libavutil) found: movies play as black pictures and silence. On Fedora: sudo dnf install libavcodec-free openh264");
        else {
#define SYM(lib, f, n) (*(void **)&F.f = dlsym(lib, n))
            int ok = SYM(avc, find_by_name, "avcodec_find_decoder_by_name") && SYM(avc, alloc_ctx, "avcodec_alloc_context3") && SYM(avc, open2, "avcodec_open2") &&
                     SYM(avc, free_ctx, "avcodec_free_context") && SYM(avc, send, "avcodec_send_packet") && SYM(avc, recv, "avcodec_receive_frame") &&
                     SYM(avc, flush, "avcodec_flush_buffers") && SYM(avc, pkt_alloc, "av_packet_alloc") && SYM(avc, pkt_free, "av_packet_free") &&
                     SYM(avu, frm_alloc, "av_frame_alloc") && SYM(avu, frm_free, "av_frame_free") && SYM(avu, frm_unref, "av_frame_unref") && SYM(avu, opt_int, "av_opt_set_int");
#undef SYM
            unsigned (*ver)(void) = (unsigned (*)(void))dlsym(avc, "avcodec_version");
            if (!ok) MLOG("[RED] the FFmpeg found lacks a function this needs: movies play as black pictures and silence");
            else {
                F.ok = 1;
                unsigned v = ver ? ver() : 0;
                snprintf(F.name, sizeof F.name, "libavcodec %u.%u.%u: H.264 %s, VP8 %s, VP9 %s, AAC %s", v >> 16, v >> 8 & 255, v & 255,
                         F.find_by_name("h264") ? "h264" : F.find_by_name("libopenh264") ? "libopenh264" : "none",
                         F.find_by_name("vp8") ? "vp8" : F.find_by_name("libvpx") ? "libvpx" : "none",
                         F.find_by_name("vp9") ? "vp9" : F.find_by_name("libvpx-vp9") ? "libvpx-vp9" : "none", F.find_by_name("aac") ? "aac" : "none");
                MLOG("%s", F.name);
            }
        }
        if (!F.ok) snprintf(F.name, sizeof F.name, "no FFmpeg: black pictures and silence");
    }
    pthread_mutex_unlock(&g_load_mu);
    return F.ok;
}
const char *mc_describe(void) { load(); return F.name; }

// a decoder by the first of the names this FFmpeg has; threads: frame (h264) or slice threading
static void *open_codec(const char *const *names, int frame_threads, const char **used) {
    if (!load()) return NULL;
    for (int i = 0; names[i]; i++) {
        const void *c = F.find_by_name(names[i]);
        if (!c) continue;
        void *ctx = F.alloc_ctx(c);
        if (!ctx) continue;
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        F.opt_int(ctx, "threads", n >= 8 ? 4 : 2, 0);
        F.opt_int(ctx, "thread_type", frame_threads ? 1 : 2, 0);   // FF_THREAD_FRAME 1, FF_THREAD_SLICE 2
        if (F.open2(ctx, c, NULL) < 0) { F.free_ctx(&ctx); continue; }
        if (used) *used = names[i];
        return ctx;
    }
    return NULL;
}
// send one packet (NULL: drain); 0 or a negative AVERROR
static int send_pkt(void *ctx, const uint8_t *p, int n, int64_t pts) {
    if (!p) return F.send(ctx, NULL);
    static __thread uint8_t *buf; static __thread size_t cap;   // FFmpeg reads up to PAD bytes past the end
    if ((size_t)n + PAD > cap) { cap = ((size_t)n + PAD) * 2; free(buf); buf = malloc(cap); if (!buf) { cap = 0; return -12; } }
    memcpy(buf, p, (size_t)n), memset(buf + n, 0, PAD);
    void *pk = F.pkt_alloc();
    if (!pk) return -12;
    av_packet_head *h = pk;
    h->data = buf, h->size = n, h->pts = pts, h->dts = pts;
    int r = F.send(ctx, pk);
    h->data = NULL, h->size = 0;   // not FFmpeg's buffer: not freed with the packet
    F.pkt_free(&pk);
    return r;
}

// ---------- pictures ----------
static void black_nv12(uint8_t *dst, int w, int h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
static void black_rgba(uint8_t *dst, int w, int h) { for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u; }
// a decoded picture -> NV12 (Y stride w, then UV interleaved at w*h, stride w) or RGBA (stride w*4; BT.709 from 720 lines
// up, BT.601 below, video range); 0 if its pixel format is not 4:2:0 8-bit
static int frame_out(const av_frame_head *f, uint8_t *dst, int w, int h, int rgba) {
    int i420 = f->format == PIX_YUV420P || f->format == PIX_YUVJ420P, nv12 = f->format == PIX_NV12;
    if (!i420 && !nv12) { rgba ? black_rgba(dst, w, h) : black_nv12(dst, w, h); return 0; }
    int cw = f->width < w ? f->width : w, ch = f->height < h ? f->height : h;
    if (rgba) {
        if (cw < w || ch < h) black_rgba(dst, w, h);
        int full = f->format == PIX_YUVJ420P, b709 = h >= 720;
        double ky = full ? 1.0 : 255.0 / 219.0, kc = full ? 1.0 : 255.0 / 224.0;
        double rv = b709 ? 1.5748 : 1.402, gu = b709 ? 0.1873 : 0.3441, gv = b709 ? 0.4681 : 0.7141, bu = b709 ? 1.8556 : 1.772;
        int Y = (int)(ky * 65536), RV = (int)(rv * kc * 65536), GU = (int)(gu * kc * 65536), GV = (int)(gv * kc * 65536), BU = (int)(bu * kc * 65536), y0 = full ? 0 : 16;
#define CL(v) ((v) < 0 ? 0 : (v) > 255 ? 255 : (v))
        for (int y = 0; y < ch; y++) {
            const uint8_t *py = f->data[0] + (size_t)y * f->linesize[0];
            const uint8_t *pu = nv12 ? f->data[1] + (size_t)(y >> 1) * f->linesize[1] : f->data[1] + (size_t)(y >> 1) * f->linesize[1];
            const uint8_t *pv = nv12 ? pu + 1 : f->data[2] + (size_t)(y >> 1) * f->linesize[2];
            int cs = nv12 ? 2 : 1;
            uint8_t *o = dst + (size_t)y * w * 4;
            for (int x = 0; x < cw; x++) {
                int l = (py[x] - y0) * Y, u = pu[(x >> 1) * cs] - 128, v = pv[(x >> 1) * cs] - 128;
                int r = (l + RV * v + 32768) >> 16, g = (l - GU * u - GV * v + 32768) >> 16, b = (l + BU * u + 32768) >> 16;
                o[4 * x] = (uint8_t)CL(r), o[4 * x + 1] = (uint8_t)CL(g), o[4 * x + 2] = (uint8_t)CL(b), o[4 * x + 3] = 255;
            }
        }
#undef CL
        return 1;
    }
    if (cw < w || ch < h) black_nv12(dst, w, h);
    for (int y = 0; y < ch; y++) memcpy(dst + (size_t)y * w, f->data[0] + (size_t)y * f->linesize[0], (size_t)cw);
    int cw2 = (cw + 1) / 2;
    for (int y = 0; y < (ch + 1) / 2 && y < h / 2; y++) {
        uint8_t *o = dst + (size_t)w * h + (size_t)y * w;
        if (nv12) { memcpy(o, f->data[1] + (size_t)y * f->linesize[1], (size_t)(2 * cw2 <= w ? 2 * cw2 : w)); continue; }
        const uint8_t *u = f->data[1] + (size_t)y * f->linesize[1], *v = f->data[2] + (size_t)y * f->linesize[2];
        for (int x = 0; x < cw2 && 2 * x + 1 < w; x++) o[2 * x] = u[x], o[2 * x + 1] = v[x];
    }
    return 1;
}

// ---------- H.264: nxnative's mavc interface ----------
#define QMAX 16
struct mavc {
    void *ctx; const char *dec; int tried, red, rgba, draining;
    uint8_t ps[2][512]; int nps[2], ps_new;           // SPS / PPS (NAL units without start codes); ps_new: sent ahead of the next packet
    int64_t pend[64]; int npend;                     // times of packets inside the decoder, ascending
    struct { void *frm; int64_t pts; } q[QMAX]; int nq;   // decoded pictures, in presentation order
    int64_t black[QMAX]; int nblack;                 // no decoder: one black picture per packet
    uint8_t *au; size_t aucap;
};
mavc *mavc_new(void) { mavc *a = calloc(1, sizeof *a); return a; }
// Opens an H.264 decoder for a movie, the first time it is needed (FFmpeg's own, else openh264's).
static void avc_open(mavc *a) {
    if (a->tried) return;
    a->tried = 1;
    static const char *const names[] = {"h264", "libopenh264", NULL};
    a->ctx = open_codec(names, 1, &a->dec);
    if (a->ctx) MLOG("H.264: FFmpeg's %s decoder", a->dec);
    else MLOG("[RED] no H.264 decoder (%s): this movie plays as black pictures. On Fedora: sudo dnf install openh264 (Cisco's repository, fedora-cisco-openh264), or RPM Fusion's ffmpeg-libs", mc_describe());
}
void mavc_drop(mavc *a) {
    for (int i = 0; i < a->nq; i++) F.frm_free(&a->q[i].frm);
    a->nq = 0, a->npend = 0, a->nblack = 0, a->draining = 0;
    if (a->ctx) F.flush(a->ctx), a->ps_new = 1;   // the parameter sets again ahead of the next key frame
}
void mavc_free(mavc *a) { if (!a) return; mavc_drop(a); if (a->ctx) F.free_ctx(&a->ctx); free(a->au); free(a); }
int mavc_queued(const mavc *a) { return a->ctx ? a->nq + a->npend : a->nblack; }
void mavc_set_rgba(mavc *a) { a->rgba = 1; }
static void keep_ps(mavc *a, const uint8_t *nal, size_t n) {
    int k = (nal[0] & 31) == 7 ? 0 : 1;
    if (n > sizeof a->ps[0]) { if (!a->red++) MLOG("[RED] H.264 parameter set of %zu bytes, too long", n); return; }
    if ((size_t)a->nps[k] == n && !memcmp(a->ps[k], nal, n)) return;
    memcpy(a->ps[k], nal, n), a->nps[k] = (int)n, a->ps_new = 1;
}
// NAL units of p: Annex B start codes, or else 4-byte lengths (as nxnative's each_nal)
static size_t next_sc(const uint8_t *p, size_t n, size_t i) { for (; i + 3 <= n; i++) if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i; return n; }
// Calls fn for each NAL unit of H.264 data, in Annex B form (start codes) or length-prefixed.
static void each_nal(const uint8_t *p, size_t n, void (*fn)(void *, const uint8_t *, size_t), void *ctx) {
    size_t s = next_sc(p, n, 0);
    if (s < n && s <= 1) {
        while (s < n) {
            size_t b = s + 3, e = next_sc(p, n, b), end = e;
            while (end > b && !p[end - 1]) end--;
            if (end > b) fn(ctx, p + b, end - b);
            s = e;
        }
    } else for (size_t i = 0; i + 4 <= n;) {
        size_t len = (size_t)p[i] << 24 | (size_t)p[i + 1] << 16 | (size_t)p[i + 2] << 8 | p[i + 3];
        i += 4;
        if (!len || len > n - i) break;
        fn(ctx, p + i, len), i += len;
    }
}
static void ps_only(void *ctx, const uint8_t *nal, size_t n) { int t = nal[0] & 31; if (t == 7 || t == 8) keep_ps(ctx, nal, n); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { each_nal(p, n, ps_only, a); }
// Reads an avcC record (the movie's H.264 parameter sets) and keeps its SPS and PPS for the decoder.
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) {
    if (n < 7 || p[0] != 1) { if (!a->red++) MLOG("[RED] unrecognized avcC record (%zu bytes)", n); return; }
    size_t i = 5;
    for (int set = 0; set < 2 && i < n; set++) {
        int cnt = set ? p[i] : p[i] & 31;
        i++;
        for (int k = 0; k < cnt && i + 2 <= n; k++) { size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2; if (len > n - i) return; if (len) keep_ps(a, p + i, len); i += len; }
    }
}
typedef struct { mavc *a; size_t len; } au_ctx;
static void au_put(mavc *a, size_t *len, const uint8_t *nal, size_t n) {
    if (*len + 4 + n > a->aucap) { a->aucap = (*len + 4 + n) * 2; a->au = realloc(a->au, a->aucap); }
    static const uint8_t sc[4] = {0, 0, 0, 1};
    memcpy(a->au + *len, sc, 4), memcpy(a->au + *len + 4, nal, n), *len += 4 + n;
}
// Adds a NAL unit to the access unit being collected (parameter sets kept aside, delimiters dropped).
static void au_add(void *ctx, const uint8_t *nal, size_t n) {
    au_ctx *c = ctx;
    int t = nal[0] & 31;
    if (t == 7 || t == 8) { keep_ps(c->a, nal, n); return; }   // sent ahead of the picture (below)
    if (t == 9) return;
    au_put(c->a, &c->len, nal, n);
}
static void pend_add(mavc *a, int64_t t) {
    if (a->npend == 64) memmove(a->pend, a->pend + 1, --a->npend * sizeof *a->pend);
    int k = a->npend;
    while (k > 0 && a->pend[k - 1] > t) a->pend[k] = a->pend[k - 1], k--;
    a->pend[k] = t, a->npend++;
}
// Takes the pictures the decoder has ready into the queue (the oldest dropped when it is full).
static void avc_receive(mavc *a) {
    for (;;) {
        if (a->nq == QMAX) { if (!a->red++) MLOG("[RED] H.264 picture queue full, dropping the oldest"); F.frm_free(&a->q[0].frm); memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]); }
        void *f = F.frm_alloc();
        if (!f) return;
        int r = F.recv(a->ctx, f);
        if (r < 0) { F.frm_free(&f); return; }
        int64_t t = 0;
        if (a->npend) t = a->pend[0], memmove(a->pend, a->pend + 1, --a->npend * sizeof *a->pend);
        a->q[a->nq].frm = f, a->q[a->nq].pts = t, a->nq++;
    }
}
// Decodes an H.264 packet; its pictures go into the queue in presentation order.
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    avc_open(a);
    au_ctx c = {a, 0};
    if (a->ctx) {   // the parameter sets first when new (the decoder reads them in band)
        size_t pre = 0;
        // collect the picture's NAL units after the parameter sets: parse once to pick up in-band SPS / PPS
        each_nal(p, (size_t)n, ps_only, a);
        if (a->ps_new && a->nps[0] && a->nps[1]) { au_put(a, &pre, a->ps[0], (size_t)a->nps[0]); au_put(a, &pre, a->ps[1], (size_t)a->nps[1]); a->ps_new = 0; }
        c.len = pre;
    }
    size_t before = c.len;
    each_nal(p, (size_t)n, au_add, &c);
    if (c.len == before) return 0;   // nothing but parameter sets
    if (!a->ctx) { if (a->nblack < QMAX) a->black[a->nblack++] = pts_us; return 0; }
    if (a->draining) F.flush(a->ctx), a->draining = 0;
    int r = send_pkt(a->ctx, a->au, (int)c.len, pts_us);
    if (r == AVERROR_EAGAIN) { avc_receive(a); r = send_pkt(a->ctx, a->au, (int)c.len, pts_us); }
    if (r < 0) { if (!a->red++) MLOG("[RED] H.264 decode error %d (first case)", r); return -1; }
    pend_add(a, pts_us);
    avc_receive(a);
    return 0;
}
// Takes the next picture (NV12) from the queue; without a decoder, a black picture per packet.
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    if (!a->ctx) {   // no decoder: a black picture per packet, in time order
        if (!a->nblack) return 0;
        int k = 0;
        for (int i = 1; i < a->nblack; i++) if (a->black[i] < a->black[k]) k = i;
        if (pts_us) *pts_us = a->black[k];
        a->black[k] = a->black[--a->nblack];
        a->rgba ? black_rgba(dst, w, h) : black_nv12(dst, w, h);
        return 1;
    }
    if (flush && !a->draining) { F.send(a->ctx, NULL); a->draining = 1; avc_receive(a); }   // the end of the stream: every picture left
    if (!a->nq) { if (flush) a->npend = 0; return 0; }
    void *f = a->q[0].frm;
    if (pts_us) *pts_us = a->q[0].pts;
    memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]);
    if (!frame_out(f, dst, w, h, a->rgba) && !a->red++) MLOG("[RED] H.264 picture in pixel format %d, not 4:2:0 8-bit: black", ((av_frame_head *)f)->format);
    F.frm_free(&f);
    return 1;
}

// ---------- VP8 / VP9 ----------
struct mc_vpx { void *ctx; int red; void *frm; };
// A VP8 or VP9 decoder (FFmpeg's own, else libvpx's).
mc_vpx *mc_vpx_new(int type) {
    mc_vpx *v = calloc(1, sizeof *v);
    static const char *const n8[] = {"vp8", "libvpx", NULL}, *const n9[] = {"vp9", "libvpx-vp9", NULL};
    const char *used = NULL;
    v->ctx = open_codec(type == 2 ? n8 : n9, 0, &used);
    if (v->ctx) { v->frm = F.frm_alloc(); MLOG("%s: FFmpeg's %s decoder", type == 2 ? "VP8" : "VP9", used); }
    else MLOG("[RED] no %s decoder (%s): this movie plays as black pictures", type == 2 ? "VP8" : "VP9", mc_describe());
    return v;
}
// Decodes a VP8 / VP9 packet into dst (NV12 or RGBA); black when there is no decoder.
int mc_vpx_decode(mc_vpx *v, const uint8_t *p, int n, uint8_t *dst, int w, int h, int rgba) {
    if (!v->ctx) { rgba ? black_rgba(dst, w, h) : black_nv12(dst, w, h); return 1; }
    int r = send_pkt(v->ctx, p, n, 0);
    if (r < 0 && r != AVERROR_EAGAIN) { if (!v->red++) MLOG("[RED] VP8 / VP9 decode error %d (first case)", r); return 0; }
    int got = 0;
    while (F.recv(v->ctx, v->frm) >= 0) { got = frame_out(v->frm, dst, w, h, rgba) || got; F.frm_unref(v->frm); }   // the last one stays in dst
    return got;
}
void mc_vpx_free(mc_vpx *v) { if (!v) return; if (v->frm) F.frm_free(&v->frm); if (v->ctx) F.free_ctx(&v->ctx); free(v); }

// ---------- AAC ----------
// fed as ADTS frames (a 7-byte header from the AudioSpecificConfig), which needs no codec context fields set
struct mc_aac { void *ctx; void *frm; int ch, rate, obj, fi, red; };
// An AAC decoder from the movie's AudioSpecificConfig (its channels and rate).
mc_aac *mc_aac_new(const uint8_t *asc, int an, int channels, int rate) {
    static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    mc_aac *a = calloc(1, sizeof *a);
    a->ch = channels > 0 ? channels : 2, a->rate = rate > 0 ? rate : 48000, a->obj = 2, a->fi = 3;
    if (an >= 2) {
        a->obj = asc[0] >> 3, a->fi = (asc[0] & 7) << 1 | asc[1] >> 7;
        int cc = asc[1] >> 3 & 15;
        if (cc && cc < 8) a->ch = cc == 7 ? 8 : cc;
        if (a->fi < 13) a->rate = rates[a->fi];
    } else for (int i = 0; i < 13; i++) if (rates[i] == a->rate) a->fi = i;
    static const char *const names[] = {"aac", "aac_fixed", NULL};
    const char *used = NULL;
    a->ctx = open_codec(names, 0, &used);
    if (a->ctx) { a->frm = F.frm_alloc(); MLOG("AAC: FFmpeg's %s decoder, %d channels %d Hz (object type %d)", used, a->ch, a->rate, a->obj); }
    else MLOG("[RED] no AAC decoder (%s): this movie's sound is silent", mc_describe());
    return a;
}
// Decodes an AAC packet into 16-bit samples; silence of a frame's length when there is no decoder.
size_t mc_aac_decode(mc_aac *a, const uint8_t *p, int n, int16_t *dst, size_t cap) {
    size_t frame_samples = 1024;
    if (!a->ctx) {   // silence of one frame's length
        size_t b = frame_samples * a->ch * 2;
        if (b > cap) b = cap;
        memset(dst, 0, b);
        return b;
    }
    if (n <= 0 || n > 8191 - 7) return 0;
    uint8_t *f = malloc((size_t)n + 7);
    if (!f) return 0;
    int prof = (a->obj >= 1 && a->obj <= 4 ? a->obj : 2) - 1, len = n + 7, cc = a->ch == 8 ? 7 : a->ch;
    f[0] = 0xFF, f[1] = 0xF1;   // sync, MPEG-4, layer 0, no CRC
    f[2] = (uint8_t)(prof << 6 | a->fi << 2 | (cc >> 2 & 1));
    f[3] = (uint8_t)((cc & 3) << 6 | (len >> 11 & 3));
    f[4] = (uint8_t)(len >> 3), f[5] = (uint8_t)((len & 7) << 5 | 0x1F), f[6] = 0xFC;
    memcpy(f + 7, p, (size_t)n);
    int r = send_pkt(a->ctx, f, len, 0);
    free(f);
    if (r < 0 && r != AVERROR_EAGAIN) { if (!a->red++) MLOG("[RED] AAC decode error %d (first case)", r); return 0; }
    size_t out = 0;
    while (F.recv(a->ctx, a->frm) >= 0) {
        av_frame_head *fr = a->frm;
        int ns = fr->data[0] ? fr->nb_samples : 0, ch = a->ch;   // (a frame without samples: nothing)
        for (int i = 0; i < ns && out + (size_t)ch * 2 <= cap; i++) {
            for (int c = 0; c < ch; c++) {
                float v = 0;
                int pc = c < 8 && fr->data[c] ? c : 0;   // fewer planes than channels (a mono stream): the first again
                switch (fr->format) {
                case SMP_FLTP: v = ((const float *)fr->data[pc])[i]; break;
                case SMP_FLT: v = ((const float *)fr->data[0])[i * ch + c]; break;
                case SMP_S16P: v = ((const int16_t *)fr->data[pc])[i] / 32768.0f; break;
                case SMP_S16: v = ((const int16_t *)fr->data[0])[i * ch + c] / 32768.0f; break;
                default: if (!a->red++) MLOG("[RED] AAC sample format %d not handled: silence", fr->format); break;
                }
                float s = v * 32767.0f;
                dst[out / 2] = (int16_t)(s > 32767.0f ? 32767 : s < -32768.0f ? -32768 : s);
                out += 2;
            }
        }
        F.frm_unref(a->frm);
    }
    return out;
}
void mc_aac_free(mc_aac *a) { if (!a) return; if (a->frm) F.frm_free(&a->frm); if (a->ctx) F.free_ctx(&a->ctx); free(a); }
