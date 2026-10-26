/* SPDX-License-Identifier: GPL-3.0-only */
/* Headphone clock helper. The render runs near 44118 Hz. A 48 kHz USB
 * stream is 160/147 of that. Reading it one frame per output, then
 * skipping a frame whenever the ring was past 96, threw audio away at
 * the start of every block. That crunch is gone: frames are kept, and
 * play_lock_step trims the ratio from the buffer once per block.
 *
 * A two-point blend of the kept frames is still a lowpass. At the 48 kHz
 * ratio it is about -0.8 dB at 8 kHz and -3 dB at 16 kHz, on every
 * machine, including OFF, which does not call the converter. The filter
 * in play_fir.h replaces that blend. It is a 32-tap windowed sinc, 64
 * phases, and the fraction between two phases is blended. On the host it
 * stays level through 17 kHz. A step of exactly 65536 still copies one
 * frame, so a matched 44.1 kHz stream is unchanged until the lock trims
 * it. The same filter does that trim. */
#ifndef PLAY_DOWN_H
#define PLAY_DOWN_H
#include <stdint.h>
#include "play_fir.h"

#define PLAY_48_STEP ((48000u << 16) / 44100u) /* 71331: 160/147 in Q16 */

typedef struct {
    int16_t hl[PLAY_FIR_T];
    int16_t hr[PLAY_FIR_T];
    uint32_t pos;
    uint32_t step; /* 0: the nominal for rate48. Else the locked step. */
    uint8_t primed;
} PlayRs;

typedef int (*play_pull_fn)(void *ctx, int16_t *l, int16_t *r);

static inline void play_rs_reset(PlayRs *s)
{
    s->primed = 0;
    s->pos = 0;
}

static inline int32_t play_lerp(int32_t a, int32_t b, uint32_t pos)
{
    return a + (int32_t)(((int64_t)(b - a) * (int64_t)pos) >> 16);
}

/* Once per headphone block. nominal is 65536 at 44.1 or PLAY_48_STEP at
 * 48. fill is how many input frames are waiting. The trim is remembered
 * by the caller. A full buffer keeps moving the ratio until the buffer
 * comes back; a normal buffer only nudges it. */
static inline uint32_t play_lock_step(uint32_t nominal, uint32_t fill, int32_t *trim)
{
    int32_t err = (int32_t)fill - 96;
    int32_t add = err / 2;
    int32_t t, step;
    if (fill > 400u)
        add = 500;
    else if (fill < 8u)
        add = -500;
    if (add > 500)
        add = 500;
    if (add < -500)
        add = -500;
    t = *trim + add;
    if (t > 14000)
        t = 14000;
    if (t < -14000)
        t = -14000;
    *trim = t;
    step = (int32_t)nominal + t;
    if (step < 20000)
        step = 20000;
    if (step > 120000)
        step = 120000;
    return (uint32_t)step;
}

static inline void play_push(int16_t *h, int16_t s)
{
    uint32_t k;
    for (k = 0; k + 1u < PLAY_FIR_T; k++)
        h[k] = h[k + 1u];
    h[PLAY_FIR_T - 1u] = s;
}

/* The taps' absolute sum stays under 2.0, so this fits in int32. */
static inline int32_t play_dot(const int16_t *c, const int16_t *h)
{
    int32_t a = 0;
    uint32_t k;
    for (k = 0; k < PLAY_FIR_T; k++)
        a += (int32_t)c[k] * (int32_t)h[k];
    return a;
}

static inline int32_t play_fir_at(const int16_t *h, uint32_t pos)
{
    uint32_t phase = pos >> PLAY_FIR_SHIFT;
    uint32_t frac = pos & PLAY_FIR_MASK;
    int32_t a, b, acc;
    if (phase >= PLAY_FIR_P)
        phase = PLAY_FIR_P - 1u;
    a = play_dot(PLAY_FIR[phase], h);
    if (frac == 0u || phase + 1u >= PLAY_FIR_P)
        acc = a;
    else {
        b = play_dot(PLAY_FIR[phase + 1u], h);
        acc = a + (int32_t)((((int64_t)b - (int64_t)a) * (int64_t)frac) >> PLAY_FIR_SHIFT);
    }
    acc = (acc + (1 << 14)) >> 15;
    if (acc > 32767)
        acc = 32767;
    if (acc < -32768)
        acc = -32768;
    return acc;
}

/* One headphone frame. rate48 selects the 48 kHz nominal when step is 0.
 * pull returns 0 to hold. A step of exactly 65536 copies one frame. */
static inline int play_rs_out(PlayRs *s, int rate48, play_pull_fn pull, void *ctx, int32_t *ol, int32_t *or)
{
    int16_t sl, sr;
    uint32_t step = s->step ? s->step : (rate48 ? PLAY_48_STEP : 65536u);
    uint32_t guard;
    if (!s->primed) {
        if (!pull(ctx, &sl, &sr)) {
            *ol = 0;
            *or = 0;
            return 0;
        }
        for (guard = 0; guard < PLAY_FIR_T; guard++) {
            s->hl[guard] = sl;
            s->hr[guard] = sr;
        }
        s->pos = 0;
        s->primed = 1;
        *ol = sl;
        *or = sr;
        return 1;
    }
    if (step == 65536u) {
        if (!pull(ctx, &sl, &sr)) {
            *ol = s->hl[PLAY_FIR_T - 1u];
            *or = s->hr[PLAY_FIR_T - 1u];
            return 1;
        }
        play_push(s->hl, sl);
        play_push(s->hr, sr);
        s->pos = 0;
        *ol = sl;
        *or = sr;
        return 1;
    }
    s->pos += step;
    guard = 0;
    while (s->pos >= 65536u && guard < 3u) {
        s->pos -= 65536u;
        if (!pull(ctx, &sl, &sr)) {
            sl = s->hl[PLAY_FIR_T - 1u];
            sr = s->hr[PLAY_FIR_T - 1u];
        }
        play_push(s->hl, sl);
        play_push(s->hr, sr);
        guard++;
    }
    *ol = play_fir_at(s->hl, s->pos);
    *or = play_fir_at(s->hr, s->pos);
    return 1;
}
#endif
