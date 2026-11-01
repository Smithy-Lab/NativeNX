// SPDX-License-Identifier: GPL-3.0-or-later
// The decoders behind nxnative's movie decoder (dm/host/hle_movie.c) on Linux: the system's FFmpeg (libavcodec, loaded
// when first needed, so building needs no FFmpeg headers and running needs no FFmpeg at all). It stands in for what the
// Mac uses: VideoToolbox for H.264 (the mavc_* functions of nxnative's hle_movie_avc.h), libvpx for VP8 / VP9, and
// AudioToolbox for AAC. Vorbis stays with nxnative's stb_vorbis.
// Without a decoder (no libavcodec, or one without that codec: Fedora's ffmpeg-free decodes H.264 only through
// Cisco's OpenH264) a movie still plays, as black pictures and silence for its length, so the game goes on.
#pragma once
#include <stddef.h>
#include <stdint.h>

// VP8 (2) / VP9 (3): one packet in, at most one picture out (written as NV12, or RGBA with rgba, w x h: a smaller
// picture is padded with black, a larger one cropped). 1 = a picture, 0 = none (a hidden frame)
typedef struct mc_vpx mc_vpx;
mc_vpx *mc_vpx_new(int type);
int mc_vpx_decode(mc_vpx *v, const uint8_t *p, int n, uint8_t *dst, int w, int h, int rgba);
void mc_vpx_free(mc_vpx *v);

// AAC: the AudioSpecificConfig (asc, an bytes; may be absent), then one raw AAC frame per call into interleaved PCM16
// (at most cap bytes). Returns the bytes written
typedef struct mc_aac mc_aac;
mc_aac *mc_aac_new(const uint8_t *asc, int an, int channels, int rate);
size_t mc_aac_decode(mc_aac *a, const uint8_t *p, int n, int16_t *dst, size_t cap);
void mc_aac_free(mc_aac *a);

// the name of the library in use and the decoders it has, for the log (first call loads it)
const char *mc_describe(void);
