/* SPDX-License-Identifier: GPL-3.0-only
 * Real-time converter. Coefficients are baked by tools/bake_cast.c.
 * tools/cast_design.c is host-only and is not part of this image.
 */
#include "cast_dac.h"

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

static int32_t mul_q15(int32_t a, int32_t b) {
  int32_t p = (int32_t)((int64_t)a * b >> 15);
  return p;
}

/* Smooth clip, same shape FOMAJAM uses for its soft distortion. */
static int32_t soft(int32_t v) {
  int32_t s = clamp(v, -98304, 98304);
  int32_t u2 = (int32_t)((int64_t)s * s >> 15);
  int32_t num = (27 * 32768 + u2) << 10;
  int32_t den = (27 * 32768 + 9 * u2) >> 5;
  int32_t r = num / den;
  return (int32_t)((int64_t)s * r >> 15);
}

static int32_t biquad(CastBq *s, int32_t x) {
  int32_t y = (int32_t)(((int64_t)s->b0 * x + s->z1) >> 14);
  int32_t z1 = (int32_t)((int64_t)s->b1 * x - (int64_t)s->a1 * y + s->z2);
  int32_t z2 = (int32_t)((int64_t)s->b2 * x - (int64_t)s->a2 * y);
  s->z1 = z1;
  s->z2 = z2;
  return y;
}

static int32_t cascade(CastBq *s, int n, int32_t x) {
  for (int i = 0; i < n; i++) {
    x = biquad(&s[i], x);
  }
  return x;
}

/* N-bit mid-tread on a 16-bit sample. bits >= 16 is a copy. */
static int32_t quant_linear(int32_t x, int bits) {
  if (bits >= 16) {
    return clamp(x, -32767, 32767);
  }
  if (bits < 2) {
    bits = 2;
  }
  int shift = 16 - bits;
  int32_t bias = 1 << (shift - 1);
  int32_t ax = x < 0 ? -x : x;
  int32_t q = (ax + bias) >> shift;
  int32_t maxq = (1 << (bits - 1)) - 1;
  if (q > maxq) {
    q = maxq;
  }
  q <<= shift;
  return x < 0 ? -q : q;
}

/* Leading sign-bits in a 15-bit two's-complement word (bit 14 is the sign). */
static int lead15(int32_t v) {
  int sign = v < 0 ? 1 : 0;
  int count = 0;
  for (int b = 14; b >= 0; b--) {
    if (((v >> b) & 1) != sign) {
      break;
    }
    count++;
  }
  return count;
}

/* DX7 output law, from the BA9221 plus the SF0-SF3 scaler (Ken Shirriff's
 * die photo and the DX7/TX7 service notes): 15-bit operator word, shift left
 * 0..3 so two leading sign bits remain, emit 12 bits, analog divide by
 * 1, 2, 4 or 8. Quiet samples keep low bits; a full-scale sample is 12-bit.
 */
static int32_t quant_dx7(int32_t x) {
  int32_t v = clamp(x, -32767, 32767) >> 1; /* 15-bit */
  int shift = lead15(v) - 2;
  if (shift < 0) {
    shift = 0;
  }
  if (shift > 3) {
    shift = 3;
  }
  int32_t dac = (v << shift) >> 3; /* 12-bit field */
  int32_t back = dac << (3 - shift);
  return clamp(back << 1, -32767, 32767);
}

/* G.711 mu-law, the companding-DAC shape: fine steps near zero. Public-domain
 * layout used by the telephone COMDAC parts of that era. Not E-mu's ROM.
 */
static uint8_t mulaw_encode(int32_t sample) {
  int sign = 0;
  if (sample < 0) {
    sign = 0x80;
    sample = -sample;
  }
  if (sample > 32635) {
    sample = 32635;
  }
  sample += 0x84;
  int exp = 7;
  int mask = 0x4000;
  while ((sample & mask) == 0 && exp > 0) {
    exp--;
    mask >>= 1;
  }
  int mantissa = (sample >> (exp + 3)) & 0x0f;
  return (uint8_t)~(sign | (exp << 4) | mantissa);
}

static int32_t mulaw_decode(uint8_t u) {
  u = (uint8_t)~u;
  int exp = (u >> 4) & 0x07;
  int mantissa = u & 0x0f;
  int32_t t = ((mantissa << 3) + 0x84) << exp;
  t -= 0x84;
  return (u & 0x80) ? -t : t;
}

static int32_t quant_dpcm(CastDac *d, int32_t x) {
  int32_t diff = x - d->dpcm;
  int32_t q;
  if (d->bits == 8) {
    q = mulaw_decode(mulaw_encode(diff));
  } else {
    q = quant_linear(diff, d->bits);
  }
  d->dpcm = clamp(d->dpcm + q, -32767, 32767);
  return d->dpcm;
}

static int32_t dc_block(CastDac *d, int32_t x) {
  /* y = x - x1 + y1 * (1 - 1/256). About 27 Hz at 44.1 kHz. */
  int32_t y = x - d->dc_x + d->dc_y - (d->dc_y >> 8);
  d->dc_x = x;
  d->dc_y = y;
  return y;
}

void cast_dac_reset(CastDac *d) {
  d->phase = 0;
  d->held = 0;
  d->dpcm = 0;
  d->dc_x = 0;
  d->dc_y = 0;
  d->primed = 0;
  for (int i = 0; i < CAST_BQ_MAX; i++) {
    d->pre[i].z1 = d->pre[i].z2 = 0;
    d->post[i].z1 = d->post[i].z2 = 0;
  }
}

int32_t cast_dac_process(CastDac *d, int32_t x) {
  x = clamp(x, -32767, 32767);
  if (d->drive_q8 > 0) {
    x = soft(mul_q15(x, d->drive_q8 << 7)); /* drive_q8 / 256 * x, in Q15: (drive_q8 << 7) is Q15 gain */
  }
  int32_t pre = cascade(d->pre, d->npre, x);

  if (!d->primed) {
    d->held = pre;
    d->dpcm = pre;
    d->primed = 1;
  }

  d->phase += d->step;
  while (d->phase >= 65536u) {
    d->phase -= 65536u;
    int32_t s = pre;
    if (d->law == CAST_LAW_DX7) {
      s = quant_dx7(s);
    } else if (d->law == CAST_LAW_DPCM) {
      s = quant_dpcm(d, s);
    } else {
      s = quant_linear(s, d->bits);
    }
    d->held = s;
  }

  int32_t y = cascade(d->post, d->npost, d->held);
  if (d->dc_block) {
    y = dc_block(d, y);
  }
  return clamp(y, -32767, 32767);
}
