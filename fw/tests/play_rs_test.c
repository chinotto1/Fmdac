/* SPDX-License-Identifier: GPL-3.0-only */
/* The headphone resampler. A full ring used to skip one input frame on
 * every read. That skip is the crunch, including with the converter off. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "play_down.h"

#define N 512
static int16_t ring[N];
static uint32_t w, r;

static int pull(void *ctx, int16_t *l, int16_t *rgt)
{
    uint32_t fill;
    (void)ctx;
    fill = w - r;
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
    int32_t ol, orr, trim;
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
    /* A deep ring is played in order. Skipping it is the old crunch. */
    memset(&rs, 0, sizeof rs);
    w = r = 0;
    level = 0;
    for (i = 0; i < 200; i++) {
        ring[w & (N - 1u)] = (int16_t)level;
        level += 20;
        w++;
    }
    prev = 0;
    max_step = 0;
    for (i = 0; i < 80; i++) {
        int32_t step;
        play_rs_out(&rs, 1, pull, 0, &ol, &orr);
        if (i) {
            step = ol - prev;
            if (step < 0) step = -step;
            if (step > max_step) max_step = step;
        }
        prev = ol;
    }
    printf("deep max step %d, read %u\n", max_step, r);
    if (max_step > 40 || r < 80u) return 1;
    /* The lock holds a matched buffer and walks a full one toward the ratio. */
    trim = 0;
    if (play_lock_step(PLAY_48_STEP, 96u, &trim) != PLAY_48_STEP || trim != 0) {
        printf("lock idle %d\n", trim);
        return 1;
    }
    trim = 0;
    for (i = 0; i < 12; i++)
        play_lock_step(65536u, 500u, &trim);
    printf("lock high trim %d\n", trim);
    if (trim < 5000) return 1;
    trim = 0;
    for (i = 0; i < 12; i++)
        play_lock_step(PLAY_48_STEP, 0u, &trim);
    if (trim > -5000) {
        printf("lock low trim %d\n", trim);
        return 1;
    }
    /* Level of a sine through the band-limited filter. A two-point blend is
     * about -0.8 dB at 8 kHz and -1.8 dB at 12 kHz; this must stay within
     * 0.35 dB. 65506 is a 44.1 kHz stream after the clock trim. */
    {
        static const struct { uint32_t step; double hz, fs; } tone[] = {
            {71331u, 1000.0, 48000.0},
            {71331u, 8000.0, 48000.0},
            {71331u, 12000.0, 48000.0},
            {65506u, 8000.0, 44100.0 * 65506.0 / 65536.0},
        };
        uint32_t ti;
        for (ti = 0; ti < sizeof tone / sizeof tone[0]; ti++) {
            double t = 0, acc = 0, db, rms, expect;
            int n = 0;
            memset(&rs, 0, sizeof rs);
            rs.step = tone[ti].step;
            w = r = 0;
            for (i = 0; i < 6000; i++) {
                while (w - r < 80u) {
                    double s = sin(2.0 * 3.141592653589793 * tone[ti].hz * t);
                    ring[w & (N - 1u)] = (int16_t)lrint(8000.0 * s);
                    t += 1.0 / tone[ti].fs;
                    w++;
                }
                play_rs_out(&rs, 1, pull, 0, &ol, &orr);
                if (i >= 1500) {
                    acc += (double)ol * (double)ol;
                    n++;
                }
            }
            rms = sqrt(acc / n);
            expect = 8000.0 / sqrt(2.0);
            db = 20.0 * log10(rms / expect);
            printf("sine %.0f Hz step %u  %+.2f dB\n", tone[ti].hz, tone[ti].step, db);
            if (db < -0.35 || db > 0.35) return 1;
        }
    }
    printf("play_rs ok\n");
    return 0;
}
