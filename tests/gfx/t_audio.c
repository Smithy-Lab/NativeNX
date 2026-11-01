// SPDX-License-Identifier: GPL-3.0-or-later
// The audio output (compat/nxnative/pcmout.c) driven the way nxnative's renderer thread drives it (hle_audren.c
// render_main: a few 5 ms blocks in flight, each handed back when played), into SDL's "disk" audio driver, which writes
// what the sound card would play to a file at the card's pace. Checks: every sample arrives in order, the blocks come back
// at real-time speed (the game is paced by them), and NXL_AUDIO=0 gives the silent output at the same speed.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void *dm_pcmout_open(int ch);
void dm_pcmout_close(void *k);
void dm_pcmout_start(void *k);
int dm_pcmout_append(void *k, void *b);
void *dm_pcmout_released(void *k);
void dm_pcmout_wait(void *k, int ms);

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++, printf("ok - " __VA_ARGS__), puts(""); else fails++, printf("not ok - " __VA_ARGS__), puts(""); } while (0)

typedef struct { void *ptr; uint64_t cap, size; } gbuf;
enum { NBUF = 4, FRAME = 240, BLOCKS = 200 };   // 200 blocks of 5 ms: one second
static int16_t sample(long i, int ch) { return (int16_t)(8000 * sin(2 * M_PI * 440 * (double)i / 48000) + ch * 3); }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

// the renderer's loop: returns the seconds the blocks took
static double play(long *made) {
    void *out = dm_pcmout_open(2);
    static int16_t data[NBUF][FRAME * 2];
    static gbuf bufs[NBUF];
    gbuf *free_list[NBUF];
    int nfree = NBUF;
    for (int i = 0; i < NBUF; i++) bufs[i] = (gbuf){data[i], sizeof data[i], 0}, free_list[i] = &bufs[i];
    dm_pcmout_start(out);
    double t0 = now_s();
    long n = 0;
    for (int blk = 0; blk < BLOCKS; blk++) {
        while (!nfree) {
            gbuf *b = dm_pcmout_released(out);
            if (b) { free_list[nfree++] = b; break; }
            dm_pcmout_wait(out, 10);
        }
        gbuf *b = free_list[--nfree];
        int16_t *s = b->ptr;
        for (int f = 0; f < FRAME; f++, n++) s[2 * f] = sample(n, 0), s[2 * f + 1] = sample(n, 1);
        b->size = FRAME * 4;
        if (!dm_pcmout_append(out, b)) free_list[nfree++] = b;
    }
    for (int got = nfree; got < NBUF;) {   // until the last block is played
        if (dm_pcmout_released(out)) got++;
        else dm_pcmout_wait(out, 10);
    }
    double t = now_s() - t0;
    dm_pcmout_close(out);
    *made = n;
    return t;
}

int main(int argc, char **argv) {
    char path[512];
    snprintf(path, sizeof path, "%s/t_audio.raw", argc > 1 ? argv[1] : ".");
    unlink(path);
    setenv("SDL_AUDIO_DRIVER", "disk", 1);
    setenv("SDL_AUDIO_DISK_OUTPUT_FILE", path, 1);
    long made;
    double t = play(&made);
    printf("# %ld frames in %.2f s\n", made, t);
    CHECK(t > 0.85 && t < 3.0, "one second of sound takes about a second through the card (%.2f s)", t);

    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "the disk driver wrote what the card would play");
    long n = 0, start = -1, match = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        long bytes = ftell(f);
        fseek(f, 0, SEEK_SET);
        int16_t *s = malloc(bytes ? bytes : 1);
        n = (long)fread(s, 4, bytes / 4, f);
        fclose(f);
        for (long i = 0; i < n && start < 0; i++) if (s[2 * i] || s[2 * i + 1]) start = i;   // silence before the start, then ours
        for (long i = 0; start >= 0 && start + i < n && i < made; i++) match += s[2 * (start + i)] == sample(i, 0) && s[2 * (start + i) + 1] == sample(i, 1);
        free(s);
    }
    printf("# file: %ld frames, sound from frame %ld, %ld of %ld frames match\n", n, start, match, made);
    CHECK(start >= 0 && match == made, "every sample, in order, left and right");

    setenv("NXL_AUDIO", "0", 1);
    t = play(&made);
    CHECK(t > 0.85 && t < 3.0, "NXL_AUDIO=0: the silent output keeps the same pace (%.2f s)", t);
    unlink(path);
    printf("nxl-tests: %d passed, %d failed\n", passes, fails);
    if (!fails) puts("nxl-tests: passed");
    return fails != 0;
}
