/* SPDX-License-Identifier: GPL-3.0-only
 * Cast converter. Integer only: the FM-1 has no FPU, and audio() may not use
 * float, double, or 64-bit division. 32x32->64 multiplies match FOMAJAM's fx.c.
 *
 * One instance is one channel of the mix Fmdac plays.
 */
#ifndef CAST_DAC_H
#define CAST_DAC_H

#include <stdint.h>

enum {
  CAST_LAW_LINEAR = 0, /* SP-1200, Prophet VS, S950: mid-tread PCM */
  CAST_LAW_DX7 = 1,    /* BA9221 12-bit field plus the SF0-SF3 /1 /2 /4 /8, Ken Shirriff */
  CAST_LAW_DPCM = 2    /* Emulator II: Rossum's companding difference, then integrate */
};

#define CAST_BQ_MAX 4

typedef struct {
  int32_t b0, b1, b2, a1, a2; /* Q14, a0 normalized away. a1/a2 are the feedback signs as in DF2T (already negated if the cookbook a is negative... see design). */
  int32_t z1, z2;             /* Q29-style state: coef Q14 * sample Q15 */
} CastBq;

typedef struct {
  int law;
  int bits;          /* word width the law uses, 4..16 */
  uint32_t step;     /* vintage clock, added to phase each host sample. 65536 = host rate */
  int npre;
  int npost;
  CastBq pre[CAST_BQ_MAX];
  CastBq post[CAST_BQ_MAX];
  int drive_q8;      /* 0: no input stage. else gain in Q8 before the soft clip (256 = 1) */
  int dc_block;      /* 1: a slow high-pass after the law (Emulator II integrator) */
  uint32_t phase;
  int32_t held;
  int32_t dpcm;
  int32_t dc_x, dc_y;
  int primed;
} CastDac;

void cast_dac_reset(CastDac *d);
int32_t cast_dac_process(CastDac *d, int32_t x);

#endif
