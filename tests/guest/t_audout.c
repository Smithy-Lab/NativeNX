// SPDX-License-Identifier: GPL-3.0-or-later
// nn::audio AudioOut (compat/nxnative/hle_audout.c), as a game that mixes its own sound uses it (FMOD, SDL's Switch
// driver): open, start, blocks appended and handed back once played, at the sound card's pace (the silent sink here,
// NXL_AUDIO=0), stop handing back what is left, close.
#include "nx.h"
typedef struct { u8 b[0x100]; } AudioOut;
typedef struct { u8 b[0x40]; } AudioOutBuffer;
typedef struct { s32 rate; u16 ch; u16 r; u32 pad; } AudioOutParameter;
void init_param(AudioOutParameter *p) __asm__("_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE");
Result open_default(AudioOut *a, const AudioOutParameter *p) __asm__("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE");
Result start(AudioOut *a) __asm__("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE");
void stop(AudioOut *a) __asm__("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE");
void close_(AudioOut *a) __asm__("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE");
int state(const AudioOut *a) __asm__("_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE");
int channels(const AudioOut *a) __asm__("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE");
int rate(const AudioOut *a) __asm__("_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE");
void set_buf(AudioOutBuffer *b, void *p, u64 cap, u64 size) __asm__("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm");
unsigned append(AudioOut *a, AudioOutBuffer *b) __asm__("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE");
AudioOutBuffer *released(AudioOut *a) __asm__("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE");
unsigned count(const AudioOut *a) __asm__("_ZN2nn5audio22GetAudioOutBufferCountEPKNS0_8AudioOutE");
static inline u64 ticks(void) { u64 t; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t)); return t; }   // 19.2 MHz

static short pcm[4][480 * 2];   // 10 ms each, stereo
int test_main(void) {
    AudioOutParameter p;
    init_param(&p);
    CHECK("the parameter's defaults: 0 (48 kHz, stereo)", p.rate == 0 && p.ch == 0);
    static AudioOut a;
    CHECK("OpenDefaultAudioOut", open_default(&a, &p) == 0);
    CHECK("... 48 kHz, 2 channels, stopped (also in the AudioOut's first words)", rate(&a) == 48000 && channels(&a) == 2 && state(&a) == 1 && *(u32 *)a.b == 48000 && ((u32 *)a.b)[1] == 2);
    static AudioOutBuffer b[4];
    for (int i = 0; i < 4; i++) { for (int k = 0; k < 960; k++) pcm[i][k] = (short)(k * 30); set_buf(&b[i], pcm[i], sizeof pcm[i], sizeof pcm[i]); }
    int ok = 1;
    for (int i = 0; i < 3; i++) ok &= append(&a, &b[i]) == 1;
    CHECK("three blocks appended", ok && count(&a) == 3);
    u64 t0 = ticks();
    CHECK("StartAudioOut", start(&a) == 0 && state(&a) == 0);
    int got = 0, order = 1;
    while (got < 3 && ticks() - t0 < 19200000ull) {   // up to a second
        AudioOutBuffer *r = released(&a);
        if (r) { order &= r == &b[got]; got++; } else svcSleepThread(1000000);
    }
    u64 ms = (ticks() - t0) / 19200;
    CHECK("... handed back once played, in order", got == 3 && order);
    print("# 30 ms of sound played in %u ms\n", (u32)ms);
    CHECK("... at the sound card's pace (about 30 ms)", ms >= 25 && ms < 500);
    CHECK("nothing more to hand back", released(&a) == NULL && count(&a) == 0);
    append(&a, &b[3]);
    stop(&a);
    CHECK("StopAudioOut hands back the block not played", released(&a) == &b[3] && state(&a) == 1);
    close_(&a);
    CHECK("CloseAudioOut", 1);
    return 0;
}
