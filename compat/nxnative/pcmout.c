// SPDX-License-Identifier: GPL-3.0-or-later
// The sound card side of nxnative's audio renderer (hle_audren.c calls dm_pcmout_*; on the Mac, hle_audio.c's CoreAudio
// output). Self-contained, so tests/gfx/t_audio.c can drive it without a game.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define LOG(...) (fprintf(stderr, "[dm] " __VA_ARGS__), fputc('\n', stderr))
static void die(const char *m) { fprintf(stderr, "[nxl] [RED] fatal: %s\n", m); exit(1); }

// nxnative's audio renderer (hle_audren.c) mixes the game's sound and hands 48 kHz PCM16 blocks to this output, then waits
// for each to be played: the game paces itself on that. The blocks go to the sound card through SDL3 (PipeWire, PulseAudio
// or ALSA underneath), whose callback takes them as the card needs them, so the card's clock paces the game as the
// Switch's does. Without SDL3 or a sound card (or with NXL_AUDIO=0) they are "played" silently at the rate a card would take
// them. NXL_AUDIO_VOLUME scales the sound (0 to 2, default 1).
void dm_audio_suspend(int on) { (void)on; }
typedef struct { void *ptr; uint64_t cap, size; } gbuf;
#define PCM_Q 64
typedef struct {
    int ch, run, started;
    gbuf *q[PCM_Q]; int qh, qn;          // appended, not yet played
    uint64_t qoff;                       // bytes of the first queued block the card has already taken
    gbuf *rel[PCM_Q]; int rh, rn;        // played, not yet taken back
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t th;
    void *dev;                           // the SDL audio stream, or NULL: the silent sink thread
    uint64_t underruns;
    uint64_t nrel;                       // blocks played so far (an AudioOut's release event, hle_audout.c)
    float vol;                           // this output's volume (an AudioOut's SetAudioOutVolume), times NXL_AUDIO_VOLUME
} pcmout;
static void pcm_release_head(pcmout *o) {   // under o->mu: the first queued block is played
    gbuf *b = o->q[o->qh];
    o->qh = (o->qh + 1) % PCM_Q, o->qn--, o->qoff = 0;
    if (o->rn < PCM_Q) o->rel[(o->rh + o->rn++) % PCM_Q] = b;
    o->nrel++;
    pthread_cond_broadcast(&o->cv);
}
// An audio output's thread: takes the game's buffers in order and plays them (paced at the output's rate without a
// device).
static void *pcm_main(void *p) {
    pcmout *o = p;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    pthread_mutex_lock(&o->mu);
    while (o->run) {
        if (!o->qn || !o->started) {
            pthread_cond_wait(&o->cv, &o->mu);
            clock_gettime(CLOCK_MONOTONIC, &next);   // resume from now after an underrun
            continue;
        }
        gbuf *b = o->q[o->qh];
        uint64_t ns = b->size / (2 * (uint64_t)o->ch) * 1000000000ull / 48000;   // bytes -> frames -> time
        next.tv_nsec += (long)(ns % 1000000000ull), next.tv_sec += (time_t)(ns / 1000000000ull);
        if (next.tv_nsec >= 1000000000) next.tv_sec++, next.tv_nsec -= 1000000000;
        pthread_mutex_unlock(&o->mu);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        pthread_mutex_lock(&o->mu);
        pcm_release_head(o);
    }
    pthread_mutex_unlock(&o->mu);
    return NULL;
}

#ifdef NXL_HAVE_SDL3
#include <SDL3/SDL.h>
static float g_volume = 1.0f;
// SDL's audio thread asks for `need` more bytes: whole and partial blocks from the queue, silence for what is missing
static void SDLCALL pcm_feed(void *ud, SDL_AudioStream *st, int need, int total) {
    (void)total;
    pcmout *o = ud;
    uint8_t buf[4096];
    while (need > 0) {
        int n = 0;
        pthread_mutex_lock(&o->mu);
        while (n < (int)sizeof buf && n < need && o->qn && o->started) {
            gbuf *b = o->q[o->qh];
            uint64_t left = b->size > o->qoff ? b->size - o->qoff : 0;
            uint64_t take = left;
            if (take > (uint64_t)(sizeof buf - n)) take = sizeof buf - n;
            if (take > (uint64_t)(need - n)) take = (uint64_t)(need - n);
            memcpy(buf + n, (const uint8_t *)b->ptr + o->qoff, take);
            n += (int)take, o->qoff += take;
            if (o->qoff >= b->size) pcm_release_head(o);
        }
        if (n < need && n < (int)sizeof buf && o->started && !o->qn) o->underruns++;
        pthread_mutex_unlock(&o->mu);
        if (!n) {   // nothing queued: silence for the rest, so the card's clock keeps going
            int z = need < (int)sizeof buf ? need : (int)sizeof buf;
            memset(buf, 0, (size_t)z);
            n = z;
        } else if (g_volume * o->vol != 1.0f) {
            int16_t *smp = (int16_t *)buf;
            float gain = g_volume * o->vol;
            for (int i = 0; i < n / 2; i++) {
                float v = smp[i] * gain;
                smp[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        }
        SDL_PutAudioStreamData(st, buf, n);
        need -= n;
    }
}
// Opens the sound device with SDL (small buffers, for low delay); NXL_AUDIO=0 plays nothing.
static int pcm_sdl_open(pcmout *o) {
    const char *e = getenv("NXL_AUDIO");
    if (e && !atoi(e)) return 0;
    if ((e = getenv("NXL_AUDIO_VOLUME"))) { float v = strtof(e, NULL); if (v >= 0 && v <= 2) g_volume = v; }
    // small device buffers: the renderer waits for each 5 ms block, so a long buffer would only add delay
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "480");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "game");
    SDL_SetHint(SDL_HINT_APP_NAME, "nxlinux");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { LOG("[RED] audio: SDL could not start sound (%s): silent", SDL_GetError()); return 0; }
    SDL_AudioSpec spec = {SDL_AUDIO_S16LE, o->ch, 48000};
    SDL_AudioStream *st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, pcm_feed, o);
    if (!st) { LOG("[RED] audio: no sound output (%s): silent", SDL_GetError()); return 0; }
    o->dev = st;
    SDL_AudioSpec dev;
    int frames = 0;
    SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(st), &dev, &frames);
    LOG("audio: %d channels at 48 kHz to %s (SDL %s, the device at %d Hz, %d-frame buffer)", o->ch,
        SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(st)) ? SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(st)) : "the default output",
        SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", dev.freq, frames);
    return 1;
}
static void pcm_sdl_start(pcmout *o) { if (o->dev) SDL_ResumeAudioStreamDevice(o->dev); }
static void pcm_sdl_close(pcmout *o) {
    if (!o->dev) return;
    SDL_DestroyAudioStream(o->dev);   // waits for a callback in progress; none comes after
    o->dev = NULL;
    if (o->underruns) LOG("audio: the sound card ran out of sound %llu times", (unsigned long long)o->underruns);
}
#else
static int pcm_sdl_open(pcmout *o) { (void)o; return 0; }
static void pcm_sdl_start(pcmout *o) { (void)o; }
static void pcm_sdl_close(pcmout *o) { (void)o; }
#endif

// A new audio output of ch channels (its thread started, waiting for dm_pcmout_start).
void *dm_pcmout_open(int ch) {
    pcmout *o = calloc(1, sizeof *o);
    o->ch = ch > 0 ? ch : 2, o->run = 1, o->vol = 1.0f;
    pthread_mutex_init(&o->mu, NULL);
    pthread_cond_init(&o->cv, NULL);
    if (pcm_sdl_open(o)) return o;
    if (pthread_create(&o->th, NULL, pcm_main, o)) die("audio output thread");
    LOG("audio: silent output, %d channels at 48 kHz", o->ch);
    return o;
}
// Starts playing an output's queued buffers.
void dm_pcmout_start(void *k) {
    pcmout *o = k;
    pthread_mutex_lock(&o->mu);
    o->started = 1;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->mu);
    pcm_sdl_start(o);
}
// Queues a buffer of the game's to play; 0 when the queue is full.
int dm_pcmout_append(void *k, void *b) {
    pcmout *o = k;
    pthread_mutex_lock(&o->mu);
    int ok = o->qn < PCM_Q;
    if (ok) o->q[(o->qh + o->qn++) % PCM_Q] = b, pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->mu);
    return ok;
}
// A buffer that has been played (given back to the game), or NULL.
void *dm_pcmout_released(void *k) {
    pcmout *o = k;
    pthread_mutex_lock(&o->mu);
    gbuf *b = NULL;
    if (o->rn) b = o->rel[o->rh], o->rh = (o->rh + 1) % PCM_Q, o->rn--;
    pthread_mutex_unlock(&o->mu);
    return b;
}
// Waits up to ms milliseconds for a buffer to be played.
void dm_pcmout_wait(void *k, int ms) {
    pcmout *o = k;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000;
    if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
    pthread_mutex_lock(&o->mu);
    if (!o->rn) pthread_cond_timedwait(&o->cv, &o->mu, &ts);
    pthread_mutex_unlock(&o->mu);
}
// Stops an output and frees it.
void dm_pcmout_close(void *k) {
    pcmout *o = k;
    pcm_sdl_close(o);
    pthread_mutex_lock(&o->mu);
    o->run = 0;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->mu);
    if (!o->dev && o->th) pthread_join(o->th, NULL);
    free(o);
}


// For an AudioOut (hle_audout.c): stopping hands back every block not played yet; the blocks queued; this output's
// volume; and a wait for the next release (the count of releases so far: returned when it passes seen, or after ms)
void dm_pcmout_stop(void *k) {
    pcmout *o = k;
    pthread_mutex_lock(&o->mu);
    o->started = 0;
    while (o->qn) pcm_release_head(o);
    pthread_mutex_unlock(&o->mu);
}
// The number of buffers queued and not yet played.
int dm_pcmout_queued(void *k) {
    pcmout *o = k;
    pthread_mutex_lock(&o->mu);
    int n = o->qn;
    pthread_mutex_unlock(&o->mu);
    return n;
}
void dm_pcmout_set_volume(void *k, float v) { ((pcmout *)k)->vol = v; }
// Waits until more buffers than seen have been played (or ms pass); the count of played buffers.
uint64_t dm_pcmout_wait_release(void *k, uint64_t seen, int ms) {
    pcmout *o = k;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000;
    while (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
    pthread_mutex_lock(&o->mu);
    while (o->nrel <= seen && o->run) if (pthread_cond_timedwait(&o->cv, &o->mu, &ts)) break;
    uint64_t n = o->nrel;
    pthread_mutex_unlock(&o->mu);
    return n;
}
