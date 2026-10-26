/* SPDX-License-Identifier: GPL-3.0-only */
/* The headphone resampler. A 48 kHz decay used to be read one-for-one and
 * then jumped 64 frames when the ring got ahead. That jump is the crunch. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "play_down.h"

#define N 256
static int16_t ring[N];
static uint32_t w, r;

static int pull(void *ctx, int16_t *l, int16_t *rgt)
{
    uint32_t fill;
    (void)ctx;
    fill = w - r;
    if (fill > 96u) {
        r++;
        fill--;
    }
    if (!fill) return 0;
    *l = ring[r & (N - 1u)];
    *rgt = *l;
    r++;
    return 1;
}

static int32_t old_jump(void)
{
    uint32_t fill = w - r;
    if (fill > 128u) r = w - 64u;
    if (w == r) return 0;
    return ring[r++ & (N - 1u)];
}

int main(void)
{
    PlayRs rs;
    int32_t prev = 0, max_step = 0, old_max = 0;
    int32_t level = 0, i, out_n = 0;
    int32_t ol, orr;
    if (PLAY_48_STEP != 71331u) {
        printf("step %u\n", PLAY_48_STEP);
        return 1;
    }
    if (play_lerp(-32768, 32767, 65535u) != 32766) {
        printf("lerp %d\n", play_lerp(-32768, 32767, 65535u));
        return 1;
    }
    memset(&rs, 0, sizeof rs);
    /* 44.1 passes samples through and holds on an empty ring. */
    ring[0] = 1000;
    ring[1] = -2000;
    w = 2;
    r = 0;
    play_rs_out(&rs, 0, pull, 0, &ol, &orr);
    play_rs_out(&rs, 0, pull, 0, &ol, &orr);
    if (ol != -2000) {
        printf("44.1 %d\n", ol);
        return 1;
    }
    play_rs_out(&rs, 0, pull, 0, &ol, &orr);
    if (ol != -2000) {
        printf("hold %d\n", ol);
        return 1;
    }
    /* A rising ramp, 48 frames in for every 44.1 out. The output may step by
     * a little more than one input sample. It must not step by a dropped block. */
    memset(&rs, 0, sizeof rs);
    w = r = 0;
    level = 0;
    for (i = 0; i < 32; i++) {
        ring[w & (N - 1u)] = (int16_t)level;
        level += 20;
        w++;
    }
    for (i = 0; i < 1000; i++) {
        uint32_t k;
        for (k = 0; k < 48u; k++) {
            if (w - r >= N - 2u) break;
            ring[w & (N - 1u)] = (int16_t)level;
            if (level < 28000) level += 20;
            w++;
        }
        for (k = 0; k < 44u + (i % 10u == 0u); k++) {
            int32_t step;
            play_rs_out(&rs, 1, pull, 0, &ol, &orr);
            if (out_n) {
                step = ol - prev;
                if (step < 0) step = -step;
                if (step > max_step) max_step = step;
            }
            prev = ol;
            out_n++;
        }
    }
    /* The old reader jumps 64 frames of this same ramp once the ring is ahead. */
    w = r = 0;
    level = 0;
    for (i = 0; i < 160; i++) {
        ring[w & (N - 1u)] = (int16_t)level;
        level += 20;
        w++;
    }
    {
        int32_t s = old_jump();
        old_max = s - ring[0];
        if (old_max < 0) old_max = -old_max;
    }
    printf("new max step %d, old jump %d, frames %d\n", max_step, old_max, out_n);
    if (max_step > 80 || old_max < 1000) return 1;
    /* A high ring slips one frame per read, not 64. */
    w = 200;
    r = 0;
    pull(0, &ring[0], &ring[1]);
    if (r != 2u) {
        printf("slip %u\n", r);
        return 1;
    }
    printf("play_rs ok\n");
    return 0;
}
