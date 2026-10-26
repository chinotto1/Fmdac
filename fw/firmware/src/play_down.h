/* SPDX-License-Identifier: GPL-3.0-only */
/* Headphone clock helper. The render runs near 44118 Hz. A 48 kHz USB
 * stream is 160/147 of that. Skipping a frame whenever the ring was past
 * 96 threw audio away at the start of every block. The resampler does not
 * do that.
 *
 * The playback ratio is the queue level, not a running sum. Adding to a
 * trim whenever the queue looked full walked the pitch: one packet of
 * timing was enough to send it the wrong way, and a full queue yanked it
 * by hundreds of counts. play_avg_fill smooths eight blocks first.
 * play_lock_step then sets the trim from that average. A clock that is
 * about 28 counts off sits that many frames below PLAY_FILL_TARGET, and
 * it stays there. A long queue is delay. play_catch_fill drops it back
 * to the target instead of calling it a clock error.
 *
 * The filter in play_fir.h is a 32-tap windowed sinc, 64 phases, and the
 * fraction between two phases is blended. On the host it stays level
 * through 17 kHz. A step of exactly 65536 copies one frame, so a matched
 * 44.1 kHz stream is unchanged until the lock trims it. The same filter
 * does that trim. The table lives in RAM: the headphone interrupt must
 * not wait on flash. */
#ifndef PLAY_DOWN_H
#define PLAY_DOWN_H
#include <stdint.h>
#include "play_fir.h"

#define PLAY_48_STEP ((48000u << 16) / 44100u) /* 71331: 160/147 in Q16 */
#define PLAY_FILL_TARGET 96u                     /* smoothed queue the lock aims at */
#define PLAY_FILL_CAP 160u                       /* above this, the queue is late, not a rate error */

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
 * 48. fill is the smoothed queue. The trim is replaced, not accumulated:
 * twelve full readings stay at one offset. */
static inline uint32_t play_lock_step(uint32_t nominal, uint32_t fill, int32_t *trim)
{
    int32_t t = (int32_t)fill - (int32_t)PLAY_FILL_TARGET;
    int32_t step;
    if (t > 8000)
        t = 8000;
    if (t < -8000)
        t = -8000;
    *trim = t;
    step = (int32_t)nominal + t;
    if (step < 20000)
        step = 20000;
    if (step > 120000)
        step = 120000;
    return (uint32_t)step;
}

/* Eight-block average of the queue. primed is 0 on the first sample. */
static inline int32_t play_avg_fill(int32_t avg, int primed, int32_t sample)
{
    if (!primed)
        return sample;
    return avg + (sample - avg) / 8;
}

/* Packet length in frames. 47..49 is a 48 kHz stream, 43..45 is 44.1.
 * 46 and a short packet keep the previous guess. */
static inline int play_rate48(uint32_t frames, int prev)
{
    if (frames >= 47u && frames <= 49u)
        return 1;
    if (frames >= 43u && frames <= 45u)
        return 0;
    return prev ? 1 : 0;
}

/* A queue past PLAY_FILL_CAP is caught up to PLAY_FILL_TARGET. *caught
 * tells the caller to drop. The lock is shown the target, so one backlog
 * does not become a pitch change. */
static inline uint32_t play_catch_fill(uint32_t fill, int *caught)
{
    if (fill > PLAY_FILL_CAP) {
        *caught = 1;
        return PLAY_FILL_TARGET;
    }
    *caught = 0;
    return fill;
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
