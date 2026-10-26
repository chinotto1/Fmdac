/* SPDX-License-Identifier: GPL-3.0-only */
/* Headphone clock helper. The render runs at the nominal 44100. A 48 kHz
 * USB stream used to be read one frame per output and, once the ring was
 * 128 frames ahead, jumped back by 64. That hole is a click on every decay,
 * on every converter including bypass. This steps the read by 48000/44100
 * and holds the last frame if the ring is empty. */
#ifndef PLAY_DOWN_H
#define PLAY_DOWN_H
#include <stdint.h>

#define PLAY_48_STEP ((48000u << 16) / 44100u) /* 71331: 160/147 in Q16 */

typedef struct {
    int16_t al, ar, bl, br;
    uint32_t pos;
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

/* One headphone frame. rate48 selects the 48 kHz step. pull returns 0 to hold. */
static inline int play_rs_out(PlayRs *s, int rate48, play_pull_fn pull, void *ctx, int32_t *ol, int32_t *or)
{
    int16_t sl, sr;
    if (!rate48) {
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
        s->bl = sl;
        s->br = sr;
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
    s->pos += PLAY_48_STEP;
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
