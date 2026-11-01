// SPDX-License-Identifier: GPL-3.0-or-later
// nn::audio AudioOut on the PC's sound card. Games that mix their own sound (FMOD, Wwise, SDL's Switch driver: Dead
// Cells) hand 48 kHz PCM16 blocks to an AudioOut, which plays them and hands them back ("released", with an event).
// On the console the SDK reaches the audout service; nxnative's dm/host/hle_audio.c takes it over with CoreAudio. This
// is the same take-over (the same entry points and the same guest-visible state: the AudioOut's first words, the
// AudioOutBuffer's first 24 bytes) on nxlinux's sound output (pcmout.c: SDL3, so PipeWire / PulseAudio / ALSA; a silent
// sink paced as a card would be without one, or with NXL_AUDIO=0). A 6-channel output goes to SDL as 5.1, which
// mixes it down to the PC's speakers. Microphones: none (an AudioIn open fails).
#include "hle.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

void *dm_pcmout_open(int ch);
void dm_pcmout_start(void *k);
int dm_pcmout_append(void *k, void *b);
void *dm_pcmout_released(void *k);
void dm_pcmout_close(void *k);
void dm_pcmout_stop(void *k);
int dm_pcmout_queued(void *k);
void dm_pcmout_set_volume(void *k, float v);
uint64_t dm_pcmout_wait_release(void *k, uint64_t seen, int ms);
void hle_bind_system_event(void *ev, int autoclear);
int hle_signal_bound_system_event(void *ev);

enum { FMT_PCM16 = 2, STATE_STARTED = 0, STATE_STOPPED = 1, RATE = 48000 };
typedef struct { void *ptr; uint64_t cap, size; } gbuf;   // the first 24 bytes of the guest's AudioOutBuffer
typedef struct {
    void *out;                // pcmout
    void *event;              // the guest's SystemEvent, signalled when a block is released (NULL: the game polls)
    int ch, id;
    _Atomic int state, run;
    float vol;
    pthread_t th;             // signals the event on releases
} aout;
static aout *ao(const void *g) { aout *a = objmap_get(g); if (!a) die("AudioOut %p was not opened by the host", g); return a; }
// An AudioOut's thread: signals the game's buffer event each time the output gives a buffer back.
static void *notifier(void *p) {
    aout *a = p;
    uint64_t seen = 0;
    while (atomic_load(&a->run)) {
        uint64_t n = dm_pcmout_wait_release(a->out, seen, 100);
        if (n != seen && a->event) hle_signal_bound_system_event(a->event);
        seen = n;
    }
    return NULL;
}
// AudioOut's front as the SDK's OpenAudioOut fills it: sample rate, channels, sample format, state (+0 / +4 / +8 / +0xc)
static void au_info(void *g, int ch, int state) { uint32_t *f = g; f[0] = RATE; f[1] = (uint32_t)ch; f[2] = FMT_PCM16; f[3] = (uint32_t)state; }
// OpenAudioOut: an output of the channels and rate the game asks for, played through pcmout.
static Result au_open(void *g, void *ev, const char *name, const uint8_t *param) {
    static _Atomic int nopen;
    int ch = *(const uint16_t *)(param + 4), rate = *(const int32_t *)param;
    if (!ch) ch = 2;
    if (rate && rate != RATE) LOG("[RED] AudioOut at %d Hz asked: played at 48 kHz", rate);
    aout *a = calloc(1, sizeof *a);
    a->event = ev, a->ch = ch, a->vol = 1.0f, a->id = atomic_fetch_add(&nopen, 1);
    atomic_init(&a->state, STATE_STOPPED), atomic_init(&a->run, 1);
    a->out = dm_pcmout_open(ch);
    if (ev) hle_bind_system_event(ev, 1);   // on this guest thread; the notifier signals it from its own
    pthread_create(&a->th, NULL, notifier, a);
    objmap_put(g, a);
    au_info(g, ch, STATE_STOPPED);
    LOG("audio #%d OpenAudioOut %s, %d channels%s", a->id, name ? name : "(default)", ch, ev ? "" : ", no release event (the game polls)");
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", "audio output parameter defaults", void, h_au_InitParam, uint8_t *p) { memset(p, 0, 8); }
HLE_FORCE("_ZN2nn5audio13ListAudioOutsEPNS0_12AudioOutInfoEi", "audio output devices: the PC's", int, h_au_List, char *info, int n) {
    if (n < 1) return 0;
    memset(info, 0, 256);
    strcpy(info, "DeviceOut");
    return 1;
}
HLE_FORCE("_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventEPKcRKNS0_17AudioOutParameterE", "audout: the PC's sound card",
          Result, h_au_Open, void *g, void *ev, const char *name, const uint8_t *param) { return au_open(g, ev, name, param); }
HLE_FORCE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE", "audout: the PC's sound card",
          Result, h_au_OpenDef, void *g, void *ev, const uint8_t *param) { return au_open(g, ev, NULL, param); }
HLE_FORCE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", "audout: the PC's sound card (no event)",
          Result, h_au_OpenDef0, void *g, const uint8_t *param) { return au_open(g, NULL, NULL, param); }
HLE_FORCE("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", "start playback", Result, h_au_Start, void *g) {
    aout *a = ao(g);
    if (atomic_exchange(&a->state, STATE_STARTED) != STATE_STARTED) dm_pcmout_start(a->out);
    au_info(g, a->ch, STATE_STARTED);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", "stop playback: the blocks not played are handed back", void, h_au_Stop, void *g) {
    aout *a = ao(g);
    atomic_store(&a->state, STATE_STOPPED);
    dm_pcmout_stop(a->out);
    au_info(g, a->ch, STATE_STOPPED);
}
HLE_FORCE("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", "close the audio output", void, h_au_Close, void *g) {
    aout *a = objmap_del(g);
    if (!a) return;
    atomic_store(&a->run, 0);
    dm_pcmout_stop(a->out);
    pthread_join(a->th, NULL);
    dm_pcmout_close(a->out);
    au_info(g, a->ch, STATE_STOPPED);
    LOG("audio #%d CloseAudioOut", a->id);
    free(a);
}
HLE_FORCE("_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", "playback state", int, h_au_State, const void *g) { return atomic_load(&ao(g)->state); }
HLE_FORCE("_ZN2nn5audio22GetAudioOutBufferCountEPKNS0_8AudioOutE", "blocks appended and not played to the end", unsigned, h_au_BufCount, const void *g) { return (unsigned)dm_pcmout_queued(ao(g)->out); }
HLE_FORCE("_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", "sample rate", int, h_au_Rate, const void *g) { (void)ao(g); return RATE; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", "channel count", int, h_au_Ch, const void *g) { return ao(g)->ch; }
HLE_FORCE("_ZN2nn5audio15GetAudioOutNameEPKNS0_8AudioOutE", "device name", const char *, h_au_Name, const void *g) { (void)g; return "DeviceOut"; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", "sample format: PCM16", int, h_au_Fmt, const void *g) { (void)ao(g); return FMT_PCM16; }
HLE_FORCE("_ZN2nn5audio17SetAudioOutVolumeEPNS0_8AudioOutEf", "output volume: a gain on the samples", void, h_au_SetVol, void *g, float v) {
    aout *a = ao(g);
    if (!(v >= 0.f)) v = 0.f;
    a->vol = v;
    dm_pcmout_set_volume(a->out, v);
}
HLE_FORCE("_ZN2nn5audio17GetAudioOutVolumeEPKNS0_8AudioOutE", "output volume", float, h_au_GetVol, const void *g) { return ao(g)->vol; }
HLE_FORCE("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", "a block's description, in the guest's struct", void, h_au_SetBuf, gbuf *b, void *p, uint64_t cap, uint64_t size) {
    b->ptr = p, b->cap = cap, b->size = size;
}
HLE_FORCE("_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", "a block's data", void *, h_au_BufPtr, const gbuf *b) { return b->ptr; }
HLE_FORCE("_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", "a block's size", uint64_t, h_au_BufSize, const gbuf *b) { return b->size; }
HLE_FORCE("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", "a block to play", unsigned, h_au_Append, void *g, gbuf *b) {
    return b && b->ptr ? (unsigned)dm_pcmout_append(ao(g)->out, b) : 0;
}
HLE_FORCE("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", "a played block, NULL if none", gbuf *, h_au_Released, void *g) { return dm_pcmout_released(ao(g)->out); }

// audio input: the host has no microphone; an open fails
HLE_FORCE("_ZN2nn5audio12ListAudioInsEPNS0_11AudioInInfoEi", "audio inputs: none", int, h_ai_List, void *info, int n) { (void)info; (void)n; return 0; }
HLE_FORCE("_ZN2nn5audio11OpenAudioInEPNS0_7AudioInEPNS_2os11SystemEventEPKcRKNS0_16AudioInParameterE", "audio input: none (fails)", Result, h_ai_Open, void *g, void *ev, const char *n, const void *p) {
    (void)g; (void)ev; (void)n; (void)p; return RES_FAIL;
}
HLE_FORCE("_ZN2nn5audio11OpenAudioInEPNS0_7AudioInEPKcRKNS0_16AudioInParameterE", "audio input: none (fails)", Result, h_ai_Open2, void *g, const char *n, const void *p) { (void)g; (void)n; (void)p; return RES_FAIL; }
HLE_FORCE("_ZN2nn5audio18OpenDefaultAudioInEPNS0_7AudioInEPNS_2os11SystemEventERKNS0_16AudioInParameterE", "audio input: none (fails)", Result, h_ai_OpenDef, void *g, void *ev, const void *p) {
    (void)g; (void)ev; (void)p; return RES_FAIL;
}
