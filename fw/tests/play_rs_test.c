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
    /* The trim is the queue error. A full reading sets it once and does not
     * keep adding. That add was the pitch slam. */
    trim = 0;
    if (play_lock_step(PLAY_48_STEP, PLAY_FILL_TARGET, &trim) != PLAY_48_STEP || trim != 0) {
        printf("lock idle %d\n", trim);
        return 1;
    }
    trim = 0;
    for (i = 0; i < 12; i++)
        play_lock_step(65536u, 500u, &trim);
    printf("lock high trim %d\n", trim);
    if (trim != 500 - (int32_t)PLAY_FILL_TARGET) return 1;
    trim = 0;
    play_lock_step(PLAY_48_STEP, 0u, &trim);
    if (trim != -(int32_t)PLAY_FILL_TARGET) {
        printf("lock low trim %d\n", trim);
        return 1;
    }
    if (play_avg_fill(0, 0, 40) != 40) return 1;
    if (play_avg_fill(40, 1, 80) != 45) return 1;
    if (play_rate48(48u, 0) != 1 || play_rate48(47u, 0) != 1 || play_rate48(49u, 0) != 1) return 1;
    if (play_rate48(44u, 1) != 0 || play_rate48(43u, 1) != 0 || play_rate48(45u, 1) != 0) return 1;
    if (play_rate48(46u, 1) != 1 || play_rate48(46u, 0) != 0 || play_rate48(0u, 1) != 1) return 1;
    {
        int caught = 0;
        if (play_catch_fill(500u, &caught) != PLAY_FILL_TARGET || !caught) return 1;
        if (play_catch_fill(100u, &caught) != 100u || caught) return 1;
    }
    /* Device half is 64 frames (platform.c). Packets land while that half
     * plays; the render then drains them. The I2S clock is 24 MHz / 544.
     * The smoothed queue sets the ratio. A matched stream settles near the
     * clock offset, without underruns and without further catch-ups. */
    {
        static const struct {
            const char *name;
            uint32_t fs_in;
            int rate48;
            int miss;
            uint32_t start;
        } run[] = {
            {"48k steady", 48000u, 1, 0, 140u},
            {"48k full", 48000u, 1, 0, 500u},
            {"44k steady", 44100u, 0, 0, 140u},
            {"44k full", 44100u, 0, 0, 500u},
            {"48k low", 48000u, 1, 0, 40u},
            {"48k packets fix nominal", 48000u, 1, 1, 200u},
        };
        uint32_t ri;
        for (ri = 0; ri < sizeof run / sizeof run[0]; ri++) {
            const uint32_t half = 64u, ring_n = 512u;
            uint32_t nominal = run[ri].rate48 && !run[ri].miss ? PLAY_48_STEP : 65536u;
            uint32_t w = run[ri].start, rd = 0, pos = 0, step = nominal, sample = 0, pkt = 0;
            uint32_t underrun = 0, caps = 0;
            uint32_t under_mark = 0, caps_mark = 0, marked = 0;
            uint32_t tail_lo = 100000u, tail_hi = 0;
            int32_t trimv = 0, avg = 0, max_jump = 0, prev_trim = 0;
            uint8_t primed = 0, avg_on = 0;
            uint64_t limit = 15ull * 24000000ull / 544ull;
            uint64_t tail = limit - 5ull * 24000000ull / 544ull;
            uint32_t want = (uint32_t)((uint64_t)run[ri].fs_in * 65536ull * 544ull / 24000000ull);
            int32_t want_trim = (int32_t)want - (int32_t)(run[ri].rate48 ? PLAY_48_STEP : 65536u);
            while ((uint64_t)sample < limit) {
                uint32_t end = sample + half, n;
                if (!marked && (uint64_t)sample >= tail) {
                    under_mark = underrun;
                    caps_mark = caps;
                    marked = 1;
                    prev_trim = trimv;
                }
                for (n = 0; n < half; n++) {
                    uint32_t guard;
                    if (n + 1u == half) {
                        uint32_t fill = w - rd;
                        int caught = 0;
                        int32_t seen, jump;
                        fill = play_catch_fill(fill, &caught);
                        if (caught) {
                            caps++;
                            rd = w - PLAY_FILL_TARGET;
                            primed = 0;
                            pos = 0;
                            avg = (int32_t)PLAY_FILL_TARGET;
                            avg_on = 1;
                            seen = avg;
                        } else {
                            seen = (int32_t)fill;
                            avg = play_avg_fill(avg, avg_on, seen);
                            avg_on = 1;
                        }
                        if (avg < 0) avg = 0;
                        step = play_lock_step(nominal, (uint32_t)avg, &trimv);
                        if (marked) {
                            jump = trimv - prev_trim;
                            if (jump < 0) jump = -jump;
                            if (jump > max_jump) max_jump = jump;
                            if (fill < tail_lo) tail_lo = fill;
                            if (fill > tail_hi) tail_hi = fill;
                        }
                        prev_trim = trimv;
                    }
                    if (!primed) {
                        if (w == rd) underrun++;
                        else rd++;
                        primed = 1;
                        pos = 0;
                    } else if (step == 65536u) {
                        if (w == rd) underrun++;
                        else rd++;
                    } else {
                        pos += step;
                        guard = 0;
                        while (pos >= 65536u && guard < 3u) {
                            pos -= 65536u;
                            if (w == rd) underrun++;
                            else rd++;
                            guard++;
                        }
                    }
                }
                while (pkt < 1000000u) {
                    uint32_t at = (uint32_t)(((uint64_t)pkt * 24000ull) / 544ull);
                    uint32_t frames, k;
                    if (at >= end) break;
                    frames = run[ri].rate48 ? 48u : 44u + (pkt % 10u == 9u);
                    if (run[ri].miss && pkt == 2u) {
                        nominal = PLAY_48_STEP;
                        trimv = 0;
                        prev_trim = 0;
                        step = nominal;
                        avg_on = 0;
                        primed = 0;
                    }
                    for (k = 0; k < frames; k++) {
                        if (w - rd + 1u >= ring_n) break;
                        w++;
                    }
                    pkt++;
                }
                sample = end;
            }
            printf("%s trim %d want %d caps %u tailcaps %u under %u tailunder %u tail %u..%u jump %d\n",
                   run[ri].name, trimv, want_trim, caps, caps - caps_mark, underrun, underrun - under_mark,
                   tail_lo, tail_hi, max_jump);
            if (max_jump > 8) return 1;
            if (underrun - under_mark) return 1;
            if (caps - caps_mark) return 1;
            if (run[ri].start > PLAY_FILL_CAP && caps < 1u) return 1;
            if (!run[ri].miss && caps > 2u) return 1;
            if (trimv < want_trim - 8 || trimv > want_trim + 8) return 1;
            if (tail_lo < 16u || tail_hi > PLAY_FILL_CAP) return 1;
        }
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
