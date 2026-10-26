/* SPDX-License-Identifier: GPL-3.0-only */
/* Headphone clock helper. The render runs near 44118 Hz. A 48 kHz USB
 * stream is 160/147 of that. Reading it one frame per output, then
 * skipping a frame whenever the ring was past 96, threw audio away at
 * the start of every block: the ring fills while the headphones are not
 * rendering. That is a crunch on every machine, including OFF, which
 * does not call the converter. This keeps the frames and steps the read
 * by the real ratio. A two-point interpolation is enough at 160/147; the
 * images sit against the headphone Nyquist. play_lock_step trims that
 * ratio from the buffer once per block so a clock error, or a host that
 * never sent SET_CUR, settles without discarding a block. */
#ifndef PLAY_DOWN_H
#define PLAY_DOWN_H
#include <stdint.h>

#define PLAY_48_STEP ((48000u << 16) / 44100u) /* 71331: 160/147 in Q16 */

typedef struct {
    int16_t al, ar, bl, br;
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

/* One headphone frame. rate48 selects the 48 kHz nominal when step is 0.
 * pull returns 0 to hold. A step of exactly 65536 copies one frame. */
static inline int play_rs_out(PlayRs *s, int rate48, play_pull_fn pull, void *ctx, int32_t *ol, int32_t *or)
{
    int16_t sl, sr;
    uint32_t step = s->step ? s->step : (rate48 ? PLAY_48_STEP : 65536u);
    if (step == 65536u) {
        if (!pull(ctx, &sl, &sr)) {
            if (!s->primed) {
                *ol = 0;
                *or = 0;
                return 0;
            }
            *ol = s->bl;
            *or = s->br;
            return 1;
        }
        s->al = s->bl = sl;
        s->ar = s->br = sr;
        s->primed = 1;
        *ol = sl;
        *or = sr;
        return 1;
    }
    if (!s->primed) {
        if (!pull(ctx, &sl, &sr)) {
            *ol = 0;
            *or = 0;
            return 0;
        }
        s->al = s->bl = sl;
        s->ar = s->br = sr;
        s->pos = 0;
        s->primed = 1;
        *ol = sl;
        *or = sr;
        return 1;
    }
    s->pos += step;
    {
        uint32_t guard = 0;
        while (s->pos >= 65536u && guard < 3u) {
            s->pos -= 65536u;
            s->al = s->bl;
            s->ar = s->br;
            if (pull(ctx, &sl, &sr)) {
                s->bl = sl;
                s->br = sr;
            }
            guard++;
        }
    }
    *ol = play_lerp(s->al, s->bl, s->pos);
    *or = play_lerp(s->ar, s->br, s->pos);
    return 1;
}
#endif
