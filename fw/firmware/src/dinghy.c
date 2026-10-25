/* SPDX-License-Identifier: GPL-3.0-only */
/* Fmdac 0.2. Pages, synthesized drums, a 16-step pattern, and the converter.
 *
 *   FX     machine, bits, rate, tone. SEL: wet, drive, bits, rate.
 *          The law is on the mix this FM-1 plays. Headphones hear that mix.
 *          USB audio records it. Audio from the computer is not an input.
 *   HOME   drum levels. Black keys F# G# A# C# hit the four drums.
 *   SEQ    white keys are the 16 steps. PLAY starts. REC writes the playhead.
 *   EDIT   tune, decay, snap, drop. Any key hits the selected drum.
 *   ARP    up, down, up-down, random. Held keys, MIDI out.
 *   LFO    MIDI CC, and level or tune on this mix when DEST says so.
 *   ENV    attack, decay, sustain, release of the key tone.
 *   GLO    tempo, swing, length, MIDI channel. OCT+ twice clears the pattern.
 *   PRESETS  pattern 1..8.  SELECT track.  ALGORITHM tempo.  OCT the keys.
 *
 * Not in this file: sample ROMs, MIDI-only tracks, an OCTACHRON map,
 * retrospective take, a piano roll, a song chain, host-to-device USB audio.
 *
 * Integers only. No float, no 64-bit division. Converter coefficients are
 * cast_tab.h, baked on a computer by tools/bake_cast.c. */
#define APP_NAME "FMDAC"
#define STORE_MAGIC 0x43444D46u
static void app_idle(void);
#define LCD_IDLE app_idle
#include "platform.c"
#if BOARD_SMK37
#include "smk_wheel.h"
#endif
#include "cast_dac.h"
#include "cast_tab.h"
#include "cast_dac.c"

#define SET_VER 2u
#define KEY0 53u
enum { PG_FX, PG_HOME, PG_SEQ, PG_EDIT, PG_ARP, PG_LFO, PG_ENV, PG_GLO, PG_N };

typedef struct {
    uint8_t ver;
    int8_t oct;
    uint8_t ch, tempo, swing, length, pattern, track, machine;
    uint8_t bits[6], rate[6], tone[6];
    uint8_t wet, drive;
    uint8_t mix[4];
    uint8_t drum[4][4];             /* tune, decay, snap, drop */
    uint8_t atk, dec, sus, rel;
    uint8_t arp_mode, arp_div, arp_gate, arp_oct;
    uint8_t lfo_rate, lfo_depth, lfo_cc, lfo_dest;
    uint8_t pat[8][4][16];
} Set;
typedef char set_fits[(sizeof(Set) <= 4000u) ? 1 : -1];

static volatile Set set;
static uint8_t page, fx_alt, rec, hot, save_req;
static volatile uint8_t seq_run, audio_ready, sounding, ext_on, fx_bypass;
static uint8_t idle_ok, in_midi;
static volatile uint32_t seq_epoch, seq_gen, ext_count, clk_out_n, wet_q8;
static volatile uint8_t seq_pos, seq_vel_out[4], trig_n[4], trig_v[4];
static volatile uint8_t gain[4];
static volatile int32_t lfo_tune, lfo_lvl = 256;
static CastDac live;

static const uint32_t INC_C9[12] = {
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};
static const uint32_t DRUM_F[4] = {4480011u, 17530479u, 0u, 9739155u}; /* 46, 180, hat, 100 Hz */
static const uint16_t TUNE_Q12[11] = {
    2048, 2340, 2676, 3059, 3497, 4096, 4796, 5616, 6576, 7700, 8192};
static const uint8_t BITV[5] = {8, 10, 12, 14, 16};
static const uint8_t GM[4] = {36, 38, 42, 45};
static const char *const MACH[6] = {"VS", "DX7", "SP-1200", "EMU II", "S950", "OFF"};
static const char *const TNAM[5] = {"RAW", "DARK", "OPEN", "BRIGHT", "FULL"};
static const char *const TRK[4] = {"KICK", "SNARE", "HAT", "TOM"};
static const char *const MODE[5] = {"OFF", "UP", "DOWN", "UP-DN", "RAND"};
static const char *const DIVN[4] = {"1/4", "1/8", "1/16", "1/32"};
static const char *const DEST[3] = {"OFF", "TUNE", "LEVEL"};

static uint32_t clampu(int v, int lo, int hi) {
    if (v < lo) return (uint32_t)lo;
    if (v > hi) return (uint32_t)hi;
    return (uint32_t)v;
}
static int32_t sat(int32_t x) {
    if (x > 32767) return 32767;
    if (x < -32767) return -32767;
    return x;
}
static int32_t sine(uint32_t ph) {
    int32_t x = (int32_t)ph >> 16, ax = x < 0 ? -x : x, y, ay;
    y = (x * (32768 - ax)) >> 13;
    ay = y < 0 ? -y : y;
    y += ((((y * ay) >> 15) - y) * 7373) >> 15;
    return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}
static uint32_t lcg = 0x6D2B79F5u;
static int32_t noise(void) {
    lcg = lcg * 1664525u + 1013904223u;
    return (int32_t)(lcg >> 17) - 16384;
}
static uint32_t put_str(char *d, uint32_t n, uint32_t i, const char *s) {
    while (*s && i + 1u < n) d[i++] = *s++;
    d[i] = 0;
    return i;
}
static void put_u(char *d, uint32_t n, uint32_t v) {
    char b[10];
    uint32_t k = 0, i = 0;
    if (!v) b[k++] = '0';
    while (v && k < 10u) { b[k++] = (char)('0' + v % 10u); v /= 10u; }
    while (k && i + 1u < n) d[i++] = b[--k];
    d[i] = 0;
}
static uint32_t put_u_at(char *d, uint32_t n, uint32_t i, uint32_t v) {
    char b[12];
    put_u(b, 12, v);
    return put_str(d, n, i, b);
}

/* ---- drums and keys (the audio ISR owns the oscillators) ---- */
typedef struct {
    uint32_t ph, inc, target;
    int32_t amp, namp, lp;
    uint16_t click;
    uint8_t alive;
} Drum;
static Drum drumv[4];

static void drum_hit(uint32_t d, uint32_t vel) {
    Drum *v;
    uint32_t tune, snap, drop, i, f, mul, base, end;
    if (d > 3u || !vel) return;
    if (vel > 127u) vel = 127u;
    v = &drumv[d];
    tune = set.drum[d][0];
    snap = set.drum[d][2];
    drop = set.drum[d][3];
    if (tune > 100u) tune = 100u;
    if (snap > 100u) snap = 100u;
    if (drop > 100u) drop = 100u;
    i = tune / 10u;
    f = tune - i * 10u;
    if (i > 10u) i = 10u;
    mul = TUNE_Q12[i];
    if (i < 10u && f) mul += (uint32_t)(TUNE_Q12[i + 1u] - mul) * f / 10u;
    if (d == 2u) {
        v->amp = 0;
        v->namp = (int32_t)vel * 220;
        if (v->namp > 32767) v->namp = 32767;
        v->lp = 0;
        v->click = 0;
    } else {
        base = (uint32_t)(((uint64_t)DRUM_F[d] * mul) >> 12);
        end = base ? base : 1u;
        v->target = end;
        v->inc = end + (uint32_t)(((uint64_t)end * drop) >> 4);
        v->ph = 0;
        v->amp = (int32_t)vel * 150;
        if (v->amp > 30000) v->amp = 30000;
        v->click = 0;
        v->namp = 0;
        if (d == 1u) {
            v->namp = (int32_t)vel * (int32_t)snap * 2;
            if (v->namp > 32767) v->namp = 32767;
        } else {
            v->click = (uint16_t)(snap * 4u);
        }
    }
    v->alive = 1;
}

typedef struct {
    uint32_t inc, ph, age;
    int32_t amp, peak, sus;
    uint8_t note, stage;            /* 0 off, 1 attack, 2 decay, 3 hold, 4 release */
} Voice;
#define NV 4u
static Voice voice[NV];
static uint32_t ages;
static volatile uint32_t bend_q16 = 65536u;

static void tone_start(uint32_t note, uint32_t vel) {
    uint32_t v, pick = 0, best = 0xFFFFFFFFu, oct;
    if (note > 127u || !vel) return;
    if (vel > 127u) vel = 127u;
    for (v = 0; v < NV; v++) {
        uint32_t score = voice[v].note == note && voice[v].stage && voice[v].stage != 4u ? 0u
                       : voice[v].stage && voice[v].stage != 4u ? 0x80000000u + voice[v].age
                       : (uint32_t)voice[v].amp + 1u;
        if (score < best) { best = score; pick = v; }
    }
    oct = note / 12u;
    if (oct > 10u) oct = 10u;
    voice[pick].note = (uint8_t)note;
    voice[pick].age = ++ages;
    voice[pick].inc = INC_C9[note % 12u] >> (10u - oct);
    voice[pick].peak = (int32_t)vel * 120;
    voice[pick].stage = 1;
}
static void tone_stop(uint32_t note) {
    uint32_t v;
    for (v = 0; v < NV; v++)
        if (voice[v].note == note && voice[v].stage && voice[v].stage != 4u)
            voice[v].stage = 4;
}
static void tones_release(void) {
    uint32_t v;
    for (v = 0; v < NV; v++)
        if (voice[v].stage && voice[v].stage != 4u)
            voice[v].stage = 4;
}

static uint8_t step_i, trig_seen[4];
static uint32_t step_acc, clk_acc, epoch_seen, ext_seen, clk_len_hold;

static uint32_t step_len(uint32_t idx, uint32_t tempo, uint32_t swing) {
    uint32_t eighth, longl, shortl;
    if (tempo < 40u) tempo = 40u;
    if (tempo > 240u) tempo = 240u;
    if (swing < 50u) swing = 50u;
    if (swing > 75u) swing = 75u;
    eighth = 1323000u / tempo;
    longl = eighth * swing / 100u;
    shortl = eighth - longl;
    if (longl < 128u) longl = 128u;
    if (shortl < 128u) shortl = 128u;
    return (idx & 1u) ? shortl : longl;
}
static void render_step(void) {
    uint8_t p = set.pattern, len = set.length, d;
    if (p > 7u) p = 0;
    if (len < 1u) len = 1u;
    if (len > 16u) len = 16u;
    if (step_i >= len) step_i = 0;
    for (d = 0; d < 4u; d++) {
        uint8_t v = *(volatile uint8_t *)&set.pat[p][d][step_i];
        seq_vel_out[d] = v;
        if (v) drum_hit(d, v);
    }
    seq_pos = step_i;
    seq_gen++;
}

static void app_audio(int32_t *out, uint32_t n) {
    uint32_t i, d, tempo, swing, len, sh[4], tune[4], drop[4], gv[4];
    int32_t lt, lvl, wq;
    uint32_t any = 0;
    if (!audio_ready) return;
    tempo = set.tempo;
    swing = set.swing;
    len = set.length;
    if (tempo < 40u) tempo = 40u;
    if (len < 1u) len = 1u;
    if (len > 16u) len = 16u;
    lt = lfo_tune;
    lvl = lfo_lvl;
    if (lvl < 32) lvl = 32;
    if (lvl > 512) lvl = 512;
    wq = (int32_t)wet_q8;
    for (d = 0; d < 4u; d++) {
        uint32_t dec = set.drum[d][1];
        gv[d] = gain[d];
        tune[d] = set.drum[d][0];
        drop[d] = set.drum[d][3];
        if (dec > 100u) dec = 100u;
        sh[d] = 9u + dec / 10u;
        if (trig_n[d] != trig_seen[d]) {
            trig_seen[d] = trig_n[d];
            drum_hit(d, trig_v[d]);
        }
    }
    if (seq_epoch != epoch_seen) {
        epoch_seen = seq_epoch;
        step_i = 0;
        step_acc = 0;
        clk_acc = 0;
        ext_seen = ext_count;
        if (seq_run) render_step();
    }
    if (seq_run && ext_on) {
        uint32_t want = ext_count, guard = 0;
        while (ext_seen != want && guard++ < 4u) {
            ext_seen++;
            step_i++;
            if (step_i >= len) step_i = 0;
            render_step();
        }
    } else if (seq_run) {
        uint32_t lim = step_len(step_i, tempo, swing), guard = 0, cl = 110250u / tempo;
        step_acc += n;
        while (step_acc >= lim && guard++ < 4u) {
            step_acc -= lim;
            step_i++;
            if (step_i >= len) step_i = 0;
            render_step();
            lim = step_len(step_i, tempo, swing);
        }
        if (cl < 64u) cl = 64u;
        clk_acc += n;
        while (clk_acc >= cl) { clk_acc -= cl; clk_out_n++; }
        clk_len_hold = cl;
    }
    {
        uint32_t atk = set.atk, dec = set.dec, sus = set.sus, rel = set.rel;
        uint32_t as, ds, rs;
        if (atk > 100u) atk = 100u;
        if (dec > 100u) dec = 100u;
        if (sus > 100u) sus = 100u;
        if (rel > 100u) rel = 100u;
        as = 1u + (100u - atk) * 8u;
        ds = 6u + dec / 10u;
        rs = 6u + rel / 10u;
        for (d = 0; d < NV; d++) {
            Voice *v = &voice[d];
            uint32_t ph, step;
            int32_t amp;
            if (!v->stage && !v->amp) continue;
            any = 1;
            if (v->peak) v->sus = v->peak * (int32_t)sus / 100;
            ph = v->ph;
            amp = v->amp;
            step = (uint32_t)(((uint64_t)v->inc * bend_q16) >> 16);
            if (lt) {
                int32_t m = 1024 + lt;
                if (m < 256) m = 256;
                step = (uint32_t)(((int64_t)step * m) >> 10);
            }
            for (i = 0; i < n; i++) {
                int32_t s;
                if (v->stage == 1u) {
                    amp += (int32_t)as;
                    if (amp >= v->peak) { amp = v->peak; v->stage = 2u; }
                } else if (v->stage == 2u) {
                    if (amp > v->sus) {
                        amp -= (amp - v->sus) >> ds;
                        if (amp < v->sus) amp = v->sus;
                    } else v->stage = 3u;
                } else if (v->stage == 3u) {
                    amp = v->sus;
                } else if (v->stage == 4u || !v->stage) {
                    if (amp > 0) amp -= amp >> rs;
                    if (amp < 24) { amp = 0; v->stage = 0; }
                }
                ph += step;
                s = amp ? (sine(ph) * amp) >> 15 : 0;
                out[2u * i] += (s * 100) >> 7;
            }
            v->ph = ph;
            v->amp = amp;
        }
    }
    for (d = 0; d < 4u; d++) {
        Drum *v = &drumv[d];
        uint32_t cut, dsh;
        int32_t gs;
        if (!v->alive) continue;
        any = 1;
        gs = (int32_t)gv[d];
        dsh = sh[d];
        if (d == 2u && dsh > 3u) dsh -= 2u;
        cut = 1u + (tune[d] > 100u ? 0u : (100u - tune[d]) / 25u);
        if (cut < 1u) cut = 1u;
        if (cut > 6u) cut = 6u;
        for (i = 0; i < n; i++) {
            int32_t s = 0;
            uint32_t step;
            if (d == 2u) {
                int32_t nz = noise(), hp, mixed;
                v->lp += (nz - v->lp) >> cut;
                hp = nz - v->lp;
                mixed = hp + ((v->lp * (int32_t)drop[d]) >> 7);
                s = (mixed * v->namp) >> 15;
                if (v->namp > 0) {
                    v->namp -= v->namp >> dsh;
                    if (v->namp < 40) v->namp = 0;
                }
            } else {
                step = v->inc;
                if (lt) {
                    int32_t m = 1024 + lt;
                    if (m < 256) m = 256;
                    step = (uint32_t)(((int64_t)step * m) >> 10);
                }
                s = (sine(v->ph) * v->amp) >> 15;
                v->ph += step;
                if (v->click) { s += noise() >> 2; v->click--; }
                if (d == 1u) {
                    s += (noise() * v->namp) >> 15;
                    if (v->namp > 0) {
                        uint32_t nsh = dsh > 1u ? dsh - 1u : 1u;
                        v->namp -= v->namp >> nsh;
                        if (v->namp < 40) v->namp = 0;
                    }
                }
                if (v->amp > 0) {
                    uint32_t ash = (d == 1u && dsh > 2u) ? dsh - 2u : dsh;
                    v->amp -= v->amp >> ash;
                    if (v->amp < 30) v->amp = 0;
                }
                if (v->inc > v->target) {
                    uint32_t diff = v->inc - v->target, st = diff >> 10;
                    v->inc = st ? v->inc - st : v->target;
                }
            }
            s = (s * gs) >> 7;
            out[2u * i] += s;
        }
        if (!v->amp && !v->namp && !v->click) v->alive = 0;
    }
    for (i = 0; i < n; i++) {
        int32_t x = out[2u * i], y, wet;
        x = sat(x);
        if (!fx_bypass && wq) {
            wet = cast_dac_process(&live, x);
            y = x + (int32_t)(((int64_t)(wet - x) * wq) >> 8);
        } else y = x;
        y = (int32_t)(((int64_t)y * lvl) >> 8);
        y = sat(y);
        out[2u * i] = y;
        out[2u * i + 1u] = y;
        if (y > 40 || y < -40) any = 1;
    }
    sounding = (uint8_t)any;
    (void)clk_len_hold;
}

static void bq_coef(CastBq *d, const CastCoef *s) {
    d->b0 = s->b0; d->b1 = s->b1; d->b2 = s->b2; d->a1 = s->a1; d->a2 = s->a2;
}
static void fx_apply(int reset_state) {
    uint32_t m, tone, rate, bits, hz, step, i;
    fm1_irq_off();
    m = set.machine;
    if (m > 5u) m = 5u;
    fx_bypass = (uint8_t)(m >= 5u);
    live.npre = 0;
    live.npost = 0;
    live.dc_block = 0;
    live.drive_q8 = set.drive ? (int)(256 + (uint32_t)set.drive * 40u) : 0;
    if (m >= 5u) {
        live.law = CAST_LAW_LINEAR;
        live.bits = 16;
        live.step = 65536u;
    } else {
        tone = set.tone[m];
        rate = set.rate[m];
        bits = set.bits[m];
        if (tone > 4u) tone = 4u;
        if (rate > 4u) rate = 4u;
        if (bits > 4u) bits = 4u;
        hz = CAST_RATE[m][rate];
        step = (hz << 16) / 44100u;
        if (step < 256u) step = 256u;
        live.step = step;
        live.bits = (m == 1u) ? 12 : (int)BITV[bits];
        if (m == 0u) {
            live.law = CAST_LAW_LINEAR;
            for (i = 0; i < 2u; i++) bq_coef(&live.post[i], &CAST_VS_POST[tone][i]);
            live.npost = 2;
        } else if (m == 1u) {
            live.law = CAST_LAW_DX7;
            live.bits = 12;
        } else if (m == 2u) {
            live.law = CAST_LAW_LINEAR;
            bq_coef(&live.pre[0], &CAST_SP_PRE[0]);
            bq_coef(&live.pre[1], &CAST_SP_PRE[1]);
            live.npre = 2;
            live.npost = CAST_SP_NPOST[tone];
            for (i = 0; i < live.npost; i++) bq_coef(&live.post[i], &CAST_SP_POST[tone][i]);
        } else if (m == 3u) {
            live.law = CAST_LAW_DPCM;
            live.dc_block = 1;
            bq_coef(&live.pre[0], &CAST_EII_PRE[0]);
            live.npre = 1;
            live.npost = 2;
            for (i = 0; i < 2u; i++) bq_coef(&live.post[i], &CAST_EII_POST[tone][i]);
        } else {
            live.law = CAST_LAW_LINEAR;
            live.npre = 3;
            live.npost = 3;
            for (i = 0; i < 3u; i++) {
                bq_coef(&live.pre[i], &CAST_S950_PRE[rate][i]);
                bq_coef(&live.post[i], &CAST_S950_POST[rate][i]);
            }
        }
    }
    if (reset_state) cast_dac_reset(&live);
    audio_ready = 1;
    fm1_irq_on();
    (void)CAST_TONE_MILLI[0];
}

/* ---- panel text ---- */
static uint8_t paint_req = 1, hint_req, skip_keys;
static const char *msg;
static uint32_t msg_until;
static char lab[4][20], val[4][12];
static int barv[4], barlo[4], barhi[4];
static uint8_t white_ord[27], black_ord[27], white_at[16];
static uint8_t nwhite, nblack;

static void message(const char *s) {
    msg = s;
    msg_until = fm1_ms + 1400u;
    hint_req = 1;
}
static int msg_on(void) { return msg && (int32_t)(fm1_ms - msg_until) < 0; }

static void row_set(uint32_t i, const char *k, const char *name, const char *value, int v, int lo, int hi) {
    uint32_t n = put_str(lab[i], 20, 0, k);
    n = put_str(lab[i], 20, n, "  ");
    put_str(lab[i], 20, n, name);
    put_str(val[i], 12, 0, value);
    barv[i] = v;
    barlo[i] = lo;
    barhi[i] = hi;
}
static uint32_t bar_fill(int v, int lo, int hi, uint32_t w) {
    if (hi <= lo) return 0;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    uint32_t fill = (uint32_t)(v - lo) * w / (uint32_t)(hi - lo);
    if (fill < 6u && w >= 6u) fill = 6u;
    return fill;
}
static void num(char *d, uint32_t v) { put_u(d, 12, v); }

static void fill_rows(void) {
    char n0[12], n1[12], n2[12], n3[12];
    uint32_t m = set.machine, tr = set.track;
    if (m > 5u) m = 5u;
    if (tr > 3u) tr = 0;
    if (page == PG_FX && !fx_alt) {
        row_set(0, "K1", "MACHINE", MACH[m], (int)m, 0, 5);
        if (m == 5u) {
            row_set(1, "K2", "BITS", "OFF", 0, 0, 1);
            row_set(2, "K3", "RATE", "OFF", 0, 0, 1);
            row_set(3, "K4", "TONE", "OFF", 0, 0, 1);
        } else if (m == 4u) {
            num(n1, BITV[set.bits[m] > 4u ? 2u : set.bits[m]]);
            num(n2, CAST_RATE[m][set.rate[m] > 4u ? 0u : set.rate[m]]);
            num(n3, set.drive);
            row_set(1, "K2", "BITS", n1, (int)set.bits[m], 0, 4);
            row_set(2, "K3", "RATE", n2, (int)set.rate[m], 0, 4);
            row_set(3, "K4", "DRIVE", n3, (int)set.drive, 0, 8);
        } else {
            uint32_t b = set.bits[m] > 4u ? 2u : set.bits[m];
            uint32_t r = set.rate[m] > 4u ? 0u : set.rate[m];
            uint32_t t = set.tone[m] > 4u ? 2u : set.tone[m];
            num(n1, m == 1u ? 12u : BITV[b]);
            num(n2, CAST_RATE[m][r]);
            row_set(1, "K2", "BITS", n1, (int)(m == 1u ? 2u : b), 0, 4);
            row_set(2, "K3", "RATE", n2, (int)r, 0, 4);
            row_set(3, "K4", "TONE", TNAM[t], (int)t, 0, 4);
        }
    } else if (page == PG_FX) {
        uint32_t b = m < 5u && set.bits[m] <= 4u ? set.bits[m] : 2u;
        uint32_t r = m < 5u && set.rate[m] <= 4u ? set.rate[m] : 0u;
        num(n0, set.wet);
        num(n1, set.drive);
        num(n2, m == 5u ? 0u : (m == 1u ? 12u : BITV[b]));
        num(n3, m < 5u ? CAST_RATE[m][r] : 0u);
        row_set(0, "K1", "WET", n0, (int)set.wet, 0, 100);
        row_set(1, "K2", "DRIVE", n1, (int)set.drive, 0, 8);
        row_set(2, "K3", "BITS", m == 5u ? "OFF" : n2, (int)b, 0, 4);
        row_set(3, "K4", "RATE", m == 5u ? "OFF" : n3, (int)r, 0, 4);
    } else if (page == PG_HOME) {
        uint32_t i;
        for (i = 0; i < 4u; i++) {
            char k[3] = {'K', (char)('1' + i), 0};
            num(n0, set.mix[i]);
            row_set(i, k, TRK[i], n0, (int)set.mix[i], 0, 100);
            put_u(n0, 12, 0);
        }
    } else if (page == PG_EDIT) {
        static const char *NM[4] = {"TUNE", "DECAY", "SNAP", "DROP"};
        uint32_t i;
        for (i = 0; i < 4u; i++) {
            char k[3] = {'K', (char)('1' + i), 0};
            num(n0, set.drum[tr][i]);
            row_set(i, k, NM[i], n0, (int)set.drum[tr][i], 0, 100);
        }
    } else if (page == PG_ARP) {
        uint32_t mode = set.arp_mode > 4u ? 0u : set.arp_mode;
        uint32_t div = set.arp_div > 3u ? 2u : set.arp_div;
        num(n2, set.arp_gate);
        num(n3, set.arp_oct);
        row_set(0, "K1", "MODE", MODE[mode], (int)mode, 0, 4);
        row_set(1, "K2", "RATE", DIVN[div], (int)div, 0, 3);
        row_set(2, "K3", "GATE", n2, (int)set.arp_gate, 10, 100);
        row_set(3, "K4", "OCTAVE", n3, (int)set.arp_oct, 1, 3);
    } else if (page == PG_LFO) {
        uint32_t dest = set.lfo_dest > 2u ? 0u : set.lfo_dest;
        num(n0, set.lfo_rate);
        num(n1, set.lfo_depth);
        num(n2, set.lfo_cc);
        row_set(0, "K1", "RATE", n0, (int)set.lfo_rate, 0, 100);
        row_set(1, "K2", "DEPTH", n1, (int)set.lfo_depth, 0, 100);
        row_set(2, "K3", "CC", n2, (int)set.lfo_cc, 1, 127);
        row_set(3, "K4", "DEST", DEST[dest], (int)dest, 0, 2);
    } else if (page == PG_ENV) {
        static const char *NM[4] = {"ATTACK", "DECAY", "SUSTAIN", "RELEASE"};
        uint8_t envv[4];
        uint32_t i;
        envv[0] = set.atk; envv[1] = set.dec; envv[2] = set.sus; envv[3] = set.rel;
        for (i = 0; i < 4u; i++) {
            char k[3] = {'K', (char)('1' + i), 0};
            num(n0, envv[i]);
            row_set(i, k, NM[i], n0, (int)envv[i], 0, 100);
        }
    } else {
        char ch[12];
        num(n0, set.tempo);
        num(n1, set.swing);
        num(n2, set.length);
        if (!set.ch) put_str(ch, 12, 0, "OMNI");
        else { uint32_t n = put_str(ch, 12, 0, "CH "); put_u(ch + n, 12 - n, set.ch); }
        row_set(0, "K1", "TEMPO", n0, (int)set.tempo, 40, 240);
        row_set(1, "K2", "SWING", n1, (int)set.swing, 50, 75);
        row_set(2, "K3", "LENGTH", n2, (int)set.length, 1, 16);
        row_set(3, "K4", "CHANNEL", ch, (int)set.ch, 0, 16);
    }
}

static uint8_t cursor;

static void fill_seq(void) {
    char n0[12], n1[12], n2[12], n3[12];
    uint32_t len = set.length, vel = 0;
    if (len < 1u) len = 1u;
    if (len > 16u) len = 16u;
    if (cursor >= len) cursor = (uint8_t)(len - 1u);
    vel = set.pat[set.pattern][set.track][cursor];
    num(n0, (uint32_t)cursor + 1u);
    num(n1, vel);
    num(n2, len);
    num(n3, set.swing);
    row_set(0, "K1", "STEP", n0, (int)cursor, 0, (int)len - 1);
    row_set(1, "K2", "VELOCITY", n1, (int)vel, 0, 127);
    row_set(2, "K3", "LENGTH", n2, (int)len, 1, 16);
    row_set(3, "K4", "SWING", n3, (int)set.swing, 50, 75);
}

static void draw_big(uint32_t i, uint32_t y) {
    uint32_t fill = bar_fill(barv[i], barlo[i], barhi[i], 104);
    uint16_t vc = (hot == i) ? C_WHITE : C_HI;
    cv_begin(240, 48, C_BLACK);
    cv_text(8, 0, &FONT_S, lab[i], C_GRAY);
    cv_rect(124, 4, 104, 8, C_DIM);
    if (fill) cv_rect(124, 4, (int32_t)fill, 8, vc);
    cv_text(8, 16, &FONT_L, val[i], vc);
    cv_blit(0, y);
}
static void draw_small(uint32_t i, uint32_t y) {
    uint32_t fill = bar_fill(barv[i], barlo[i], barhi[i], 224);
    uint16_t vc = (hot == i) ? C_WHITE : C_HI;
    cv_begin(240, 36, C_BLACK);
    cv_text(8, 0, &FONT_S, lab[i], C_GRAY);
    cv_text(156, 0, &FONT_S, val[i], vc);
    cv_rect(8, 20, 224, 10, C_DIM);
    if (fill) cv_rect(8, 20, (int32_t)fill, 10, vc);
    cv_blit(0, y);
}
static void draw_grid(void) {
    uint32_t i, x = 0, len = set.length, tr = set.track, pat = set.pattern;
    if (len < 1u) len = 1u;
    if (len > 16u) len = 16u;
    if (tr > 3u) tr = 0;
    if (pat > 7u) pat = 0;
    cv_begin(240, 48, C_BLACK);
    for (i = 0; i < 16u; i++) {
        uint32_t vel = 0, h = 0;
        uint16_t col = C_DIM;
        if (i < len) {
            vel = set.pat[pat][tr][i];
            if (seq_run && i == seq_pos) col = C_WHITE;
            else if (vel) col = C_HI;
            h = vel * 30u / 127u;
            if (vel && h < 3u) h = 3u;
        }
        cv_rect((int32_t)x, 4, 12, 30, C_LINE);
        if (h) cv_rect((int32_t)x, 4 + (int32_t)(30u - h), 12, (int32_t)h, col);
        if (i == cursor) cv_rect((int32_t)x, 40, 12, 4, C_WHITE);
        x += 15u;
    }
    cv_blit(0, 16);
}
static void header_text(char *d) {
    const char *name = "FX", *sub = "ON THIS MIX";
    uint32_t n;
    if (page == PG_FX && fx_alt) sub = "WET AND DRIVE";
    else if (page == PG_HOME) { name = "HOME"; sub = "DRUM LEVELS"; }
    else if (page == PG_SEQ) { name = "SEQ"; sub = TRK[set.track > 3u ? 0u : set.track]; }
    else if (page == PG_EDIT) { name = "EDIT"; sub = TRK[set.track > 3u ? 0u : set.track]; }
    else if (page == PG_ARP) { name = "ARP"; sub = "HELD KEYS"; }
    else if (page == PG_LFO) { name = "LFO"; sub = "CC AND THE MIX"; }
    else if (page == PG_ENV) { name = "ENV"; sub = "KEY TONE"; }
    else if (page == PG_GLO) { name = "GLO"; sub = "TEMPO AND MIDI"; }
    n = put_str(d, 40, 0, " ");
    n = put_str(d, 40, n, name);
    n = put_str(d, 40, n, "  ");
    n = put_str(d, 40, n, sub);
    n = put_str(d, 40, n, "  v");
    put_str(d, 40, n, APP_VER);
}
static void status_text(char *d) {
    uint32_t n, tr = set.track > 3u ? 0u : set.track;
    n = put_str(d, 40, 0, seq_run ? " PLAY " : " ");
    n = put_u_at(d, 40, n, set.tempo);
    n = put_str(d, 40, n, " BPM  PAT ");
    n = put_u_at(d, 40, n, (uint32_t)set.pattern + 1u);
    n = put_str(d, 40, n, "  ");
    if (seq_run) {
        n = put_str(d, 40, n, "STEP ");
        n = put_u_at(d, 40, n, (uint32_t)seq_pos + 1u);
        n = put_str(d, 40, n, "  ");
    }
    put_str(d, 40, n, TRK[tr]);
}
static const char *hint_text(void) {
    if (msg_on()) return msg;
    if (page == PG_FX && fx_alt) return " SEL = THE LAW";
    if (page == PG_FX && set.machine == 4u) return " RATE SETS THE BAND";
    if (page == PG_FX && set.machine == 5u) return " K1 PICKS A MACHINE";
    if (page == PG_FX) return " PLAY STARTS DRUMS";
    if (page == PG_HOME) return " BLACK KEYS HIT DRUMS";
    if (page == PG_SEQ && rec) return " REC WRITES THE STEP";
    if (page == PG_SEQ) return " WHITE KEYS SET STEPS";
    if (page == PG_EDIT) return " KEYS HIT THIS DRUM";
    if (page == PG_ARP) return " HOLD KEYS TO RUN IT";
    if (page == PG_LFO) return " DEPTH IS AUDIBLE";
    if (page == PG_ENV) return " THE KEYS USE THIS";
    return " OCT+ TWICE CLEARS";
}

static uint32_t stamp, stamp_drawn;
static void paint(int full) {
    char line[40];
    uint32_t i;
    idle_ok = 1;
    if (page == PG_SEQ) fill_seq();
    else fill_rows();
    if (full) {
        header_text(line);
        draw_text_box(0, 0, 240, &FONT_S, line, C_HI, 0);
        if (page == PG_SEQ) {
            draw_grid();
            for (i = 0; i < 4u; i++) draw_small(i, 64u + i * 36u);
        } else {
            for (i = 0; i < 4u; i++) draw_big(i, 16u + i * 48u);
        }
    } else if (page == PG_SEQ) {
        draw_grid();
    }
    status_text(line);
    draw_text_box(0, 208, 240, &FONT_S, line, C_AMB, 0);
    draw_text_box(0, 224, 240, &FONT_S, hint_text(), msg_on() ? C_WHITE : C_GRAY, 0);
    idle_ok = 0;
    hint_req = 0;
}

/* ---- MIDI out queue (the screen wait must not call midi_send) ---- */
static uint8_t oq[32][3], oq_w, oq_r;
static void midi_out(uint32_t st, uint32_t d1, uint32_t d2) {
    uint8_t n = (uint8_t)((oq_w + 1u) & 31u);
    if (n == oq_r) return;
    oq[oq_w][0] = (uint8_t)st;
    oq[oq_w][1] = (uint8_t)d1;
    oq[oq_w][2] = (uint8_t)d2;
    oq_w = n;
}
static void midi_flush(void) {
    if (idle_ok) return;
    while (oq_r != oq_w) {
        midi_send(oq[oq_r][0], oq[oq_r][1], oq[oq_r][2]);
        oq_r = (uint8_t)((oq_r + 1u) & 31u);
    }
}
static uint8_t off_on[8], off_ch[8], off_note[8];
static uint32_t off_at[8];
static void note_off_later(uint32_t ch, uint32_t note, uint32_t ms) {
    uint32_t i, slot = 0;
    for (i = 0; i < 8u; i++)
        if (off_on[i] && off_ch[i] == ch && off_note[i] == note) { off_at[i] = fm1_ms + ms; return; }
    for (i = 0; i < 8u; i++) if (!off_on[i]) { slot = i; break; }
    off_on[slot] = 1;
    off_ch[slot] = (uint8_t)ch;
    off_note[slot] = (uint8_t)note;
    off_at[slot] = fm1_ms + ms;
}
static void offs_flush(void) {
    uint32_t i;
    for (i = 0; i < 8u; i++)
        if (off_on[i] && (int32_t)(fm1_ms - off_at[i]) >= 0) {
            midi_out(0x80u | off_ch[i], off_note[i], 0);
            off_on[i] = 0;
        }
}
static uint32_t out_ch(void) { return set.ch ? (uint32_t)set.ch - 1u : 0u; }

static void request_drum(uint32_t d, uint32_t vel, int record) {
    if (d > 3u || !vel) return;
    if (vel > 127u) vel = 127u;
    trig_v[d] = (uint8_t)vel;
    trig_n[d]++;
    midi_out(0x99u, GM[d], vel);
    note_off_later(9u, GM[d], 70u);
    if (record && rec && seq_run) {
        uint8_t s = seq_pos, len = set.length;
        if (len < 1u) len = 1u;
        if (s < len) {
            set.pat[set.pattern][d][s] = (uint8_t)vel;
            save_req = 0;
            paint_req = 1;
        }
    }
}

/* ---- arp ---- */
static uint8_t held[16], nheld, arp_idx, arp_oct_i, arp_dir = 1, arp_on, arp_note;
static uint32_t arp_next, arp_off;
static void held_add(uint8_t note) {
    uint32_t i;
    for (i = 0; i < nheld; i++) if (held[i] == note) return;
    if (nheld >= 16u) return;
    i = nheld;
    while (i && held[i - 1u] > note) { held[i] = held[i - 1u]; i--; }
    held[i] = note;
    nheld++;
}
static void held_del(uint8_t note) {
    uint32_t i, j;
    for (i = 0; i < nheld; i++) if (held[i] == note) {
        for (j = i + 1u; j < nheld; j++) held[j - 1u] = held[j];
        nheld--;
        return;
    }
}
static void arp_stop(void) {
    if (!arp_on) return;
    tone_stop(arp_note);
    midi_out(0x80u | out_ch(), arp_note, 0);
    arp_on = 0;
}
static void arp_run(void) {
    uint32_t bpm, quarter, iv, mode, gate, note, octs;
    if (!set.arp_mode || page == PG_SEQ || page == PG_EDIT || !nheld) {
        arp_stop();
        return;
    }
    bpm = set.tempo < 40u ? 40u : set.tempo;
    quarter = 60000u / bpm;
    iv = quarter >> (set.arp_div > 3u ? 2u : set.arp_div);
    if (iv < 20u) iv = 20u;
    if (arp_on && (int32_t)(fm1_ms - arp_off) >= 0) arp_stop();
    if ((int32_t)(fm1_ms - arp_next) < 0) return;
    arp_next = fm1_ms + iv;
    mode = set.arp_mode;
    octs = set.arp_oct < 1u ? 1u : set.arp_oct;
    if (octs > 3u) octs = 3u;
    if (mode == 4u) {
        lcg = lcg * 1664525u + 1013904223u;
        arp_idx = (uint8_t)((lcg >> 16) % nheld);
        arp_oct_i = (uint8_t)((lcg >> 8) % octs);
    } else if (mode == 2u) {
        if (!arp_idx) { arp_idx = (uint8_t)(nheld - 1u); arp_oct_i = (uint8_t)((arp_oct_i + octs - 1u) % octs); }
        else arp_idx--;
    } else if (mode == 3u) {
        if (arp_dir) {
            if (arp_idx + 1u >= nheld) arp_dir = 0;
            else arp_idx++;
        } else if (!arp_idx) { arp_dir = 1; arp_oct_i = (uint8_t)((arp_oct_i + 1u) % octs); }
        else arp_idx--;
    } else {
        arp_idx++;
        if (arp_idx >= nheld) { arp_idx = 0; arp_oct_i = (uint8_t)((arp_oct_i + 1u) % octs); }
    }
    if (arp_idx >= nheld) arp_idx = 0;
    note = (uint32_t)held[arp_idx] + 12u * arp_oct_i;
    if (note > 127u) note = 127u;
    arp_stop();
    tone_start(note, 108u);
    midi_out(0x90u | out_ch(), note, 108u);
    arp_note = (uint8_t)note;
    arp_on = 1;
    gate = set.arp_gate < 10u ? 10u : set.arp_gate;
    if (gate > 100u) gate = 100u;
    arp_off = fm1_ms + iv * gate / 100u;
}

/* ---- settings ---- */
static uint32_t t_change, dirty;
static void set_zero(void) {
    volatile uint8_t *p = (volatile uint8_t *)&set;
    uint32_t i;
    for (i = 0; i < sizeof set; i++) p[i] = 0;
}
static void pat_put(uint32_t p, uint32_t d, uint32_t s, uint32_t v) { set.pat[p][d][s] = (uint8_t)v; }
static void defaults(void) {
    static const uint8_t K0[16] = {120,0,0,0, 100,0,0,0, 120,0,0,0, 100,0,0,0};
    static const uint8_t S0[16] = {0,0,0,0, 110,0,0,0, 0,0,0,0, 110,0,40,0};
    static const uint8_t H0[16] = {80,0,48,0, 80,0,48,0, 80,0,48,0, 80,0,48,0};
    uint32_t i, m;
    set_zero();
    set.ver = SET_VER;
    set.tempo = 120;
    set.swing = 50;
    set.length = 16;
    set.machine = 2;
    for (m = 0; m < 6u; m++) {
        set.bits[m] = (m == 3u) ? 0u : 2u;
        set.rate[m] = (m == 2u || m == 3u) ? 3u : (m == 4u ? 2u : 0u);
        set.tone[m] = 2;
    }
    set.wet = 100;
    set.mix[0] = 100; set.mix[1] = 92; set.mix[2] = 64; set.mix[3] = 78;
    set.drum[0][0] = 42; set.drum[0][1] = 64; set.drum[0][2] = 28; set.drum[0][3] = 78;
    set.drum[1][0] = 58; set.drum[1][1] = 46; set.drum[1][2] = 72; set.drum[1][3] = 18;
    set.drum[2][0] = 74; set.drum[2][1] = 22; set.drum[2][2] = 100; set.drum[2][3] = 12;
    set.drum[3][0] = 48; set.drum[3][1] = 56; set.drum[3][2] = 20; set.drum[3][3] = 64;
    set.atk = 2; set.dec = 28; set.sus = 100; set.rel = 24;
    set.arp_div = 2; set.arp_gate = 55; set.arp_oct = 1;
    set.lfo_rate = 36; set.lfo_cc = 1; set.lfo_dest = 2;
    for (i = 0; i < 16u; i++) {
        pat_put(0, 0, i, K0[i]); pat_put(0, 1, i, S0[i]); pat_put(0, 2, i, H0[i]);
        if ((i & 3u) == 0u) { pat_put(1, 0, i, 110); pat_put(1, 2, i, 90); }
        else pat_put(1, 2, i, 50);
        if (i == 4 || i == 12) pat_put(1, 1, i, 104);
        if ((i & 1u) == 0u) pat_put(2, 2, i, 46);
    }
    pat_put(0, 3, 15, 96);
    pat_put(2, 0, 0, 120); pat_put(2, 0, 10, 80); pat_put(2, 1, 8, 115);
    pat_put(2, 3, 14, 70); pat_put(2, 3, 15, 100);
    pat_put(3, 0, 0, 114); pat_put(3, 0, 3, 90); pat_put(3, 0, 6, 110); pat_put(3, 0, 10, 76);
    pat_put(3, 1, 4, 108); pat_put(3, 1, 12, 64); pat_put(3, 3, 14, 100);
    for (i = 0; i < 16u; i += 2u) pat_put(3, 2, i, 60);
}
static void clamp_set(void) {
    uint32_t i, m;
    if (set.oct < -2 || set.oct > 2) set.oct = 0;
    if (set.ch > 16u) set.ch = 0;
    if (set.tempo < 40u || set.tempo > 240u) set.tempo = 120;
    if (set.swing < 50u || set.swing > 75u) set.swing = 50;
    if (set.length < 1u || set.length > 16u) set.length = 16;
    if (set.pattern > 7u) set.pattern = 0;
    if (set.track > 3u) set.track = 0;
    if (set.machine > 5u) set.machine = 2;
    for (m = 0; m < 6u; m++) {
        if (set.bits[m] > 4u) set.bits[m] = 2;
        if (set.rate[m] > 4u) set.rate[m] = 0;
        if (set.tone[m] > 4u) set.tone[m] = 2;
    }
    if (set.wet > 100u) set.wet = 100;
    if (set.drive > 8u) set.drive = 0;
    for (i = 0; i < 4u; i++) {
        uint32_t k;
        if (set.mix[i] > 100u) set.mix[i] = 80;
        for (k = 0; k < 4u; k++) if (set.drum[i][k] > 100u) set.drum[i][k] = 40;
    }
    if (set.atk > 100u) set.atk = 2;
    if (set.dec > 100u) set.dec = 28;
    if (set.sus > 100u) set.sus = 100;
    if (set.rel > 100u) set.rel = 24;
    if (set.arp_mode > 4u) set.arp_mode = 0;
    if (set.arp_div > 3u) set.arp_div = 2;
    if (set.arp_gate < 10u || set.arp_gate > 100u) set.arp_gate = 55;
    if (set.arp_oct < 1u || set.arp_oct > 3u) set.arp_oct = 1;
    if (set.lfo_rate > 100u) set.lfo_rate = 36;
    if (set.lfo_depth > 100u) set.lfo_depth = 0;
    if (set.lfo_cc < 1u || set.lfo_cc > 127u) set.lfo_cc = 1;
    if (set.lfo_dest > 2u) set.lfo_dest = 2;
}
static void audio_params(void) {
    uint32_t i, w = set.wet;
    if (w > 100u) w = 100u;
    wet_q8 = w * 256u / 100u;
    for (i = 0; i < 4u; i++) {
        uint32_t m = set.mix[i];
        if (m > 100u) m = 100u;
        gain[i] = (uint8_t)(m * 128u / 100u);
    }
}
static void do_save(void) {
    if (settings_save((const void *)&set, sizeof set)) message("NOT SAVED");
    else { message("SAVED"); dirty = 0; }
}

static void go(uint32_t pg) {
    if (pg == page) return;
    if (pg == PG_SEQ || pg == PG_EDIT) { tones_release(); arp_stop(); nheld = 0; }
    page = (uint8_t)pg;
    fx_alt = 0;
    skip_keys = 1;
    paint_req = 1;
}
static uint32_t clk_seen;
static void transport(int run, int send_midi) {
    fm1_irq_off();
    if (run) { seq_run = 1; seq_epoch++; }
    else seq_run = 0;
    clk_seen = clk_out_n;
    fm1_irq_on();
    if (send_midi) midi_out(run ? 0xFAu : 0xFCu, 0, 0);
    paint_req = 1;
}

static int black_note(uint32_t note) { return (0x54Au >> (note % 12u)) & 1; }
static void keys_init(void) {
    uint32_t k, w = 0, b = 0;
    for (k = 0; k < 27u; k++) {
        if (!black_note(KEY0 + k) && w < 16u) { white_at[w] = (uint8_t)k; white_ord[k] = (uint8_t)w++; }
        else white_ord[k] = 255u;
        if (black_note(KEY0 + k)) { black_ord[k] = (uint8_t)b++; }
        else black_ord[k] = 255u;
    }
    nwhite = (uint8_t)w;
    nblack = (uint8_t)b;
}

static uint32_t lfo_phase, lfo_ms;
static int16_t lfo_cc_sent = -1;
static void lfo_update(void) {
    uint32_t period, dt, depth, dest;
    int32_t tri;
    depth = set.lfo_depth;
    dest = set.lfo_dest > 2u ? 0u : set.lfo_dest;
    period = 30u + (set.lfo_rate > 100u ? 0u : (100u - set.lfo_rate) * 20u);
    dt = fm1_ms - lfo_ms;
    lfo_ms = fm1_ms;
    if (dt > 40u) dt = 40u;
    lfo_phase += dt * 1024u / period;
    if (lfo_phase >= 1024u) lfo_phase %= 1024u;
    tri = lfo_phase < 512u ? (int32_t)lfo_phase - 256 : 768 - (int32_t)lfo_phase;
    lfo_tune = (dest == 1u && depth) ? tri * (int32_t)depth / 200 : 0;
    if (dest == 2u && depth) {
        int32_t d = 256 + tri * (int32_t)depth / 200;
        if (d < 32) d = 32;
        if (d > 512) d = 512;
        lfo_lvl = d;
    } else lfo_lvl = 256;
    if (depth) {
        int32_t cc = 64 + tri * (int32_t)depth / 400;
        if (cc < 0) cc = 0;
        if (cc > 127) cc = 127;
        if (cc != lfo_cc_sent) {
            midi_out(0xB0u | out_ch(), set.lfo_cc ? set.lfo_cc : 1u, (uint32_t)cc);
            lfo_cc_sent = (int16_t)cc;
        }
    }
}

static uint32_t ext_ms, ext_flag, clk_n, clk_t0, clk_div;
static int drum_note(uint32_t note) {
    if (note == 35u || note == 36u) return 0;
    if (note == 38u || note == 40u) return 1;
    if (note == 42u || note == 44u || note == 46u) return 2;
    if (note == 41u || note == 43u || note == 45u || note == 47u || note == 48u || note == 50u) return 3;
    return -1;
}
static void midi_in(void) {
    uint8_t st, d1, d2;
    uint32_t nmsg = 0;
    int src;
    if (in_midi) return;
    in_midi = 1;
    while (nmsg++ < 24u && (src = midi_get(&st, &d1, &d2)) != 0) {
        uint32_t ty = st & 0xF0u, ch = st & 15u;
        (void)src;
        if (st == 0xF8u) {
            ext_flag = 1;
            ext_ms = fm1_ms;
            if (clk_n && clk_n % 24u == 0u) {
                uint32_t ms = fm1_ms - clk_t0, bpm;
                if (ms) {
                    bpm = (60000u + ms / 2u) / ms;
                    if (bpm >= 40u && bpm <= 240u) set.tempo = (uint8_t)bpm;
                }
            }
            if (clk_n % 24u == 0u) clk_t0 = fm1_ms;
            clk_n++;
            if (seq_run) {
                clk_div++;
                if (clk_div >= 6u) { clk_div = 0; ext_count++; }
            }
            continue;
        }
        if (st == 0xFAu) { clk_div = 0; transport(1, 0); continue; }
        if (st == 0xFBu) { transport(1, 0); continue; }
        if (st == 0xFCu) { transport(0, 0); continue; }
        if (ch == 9u) {
            int d = drum_note(d1);
            if (ty == 0x90u && d2 && d >= 0) request_drum((uint32_t)d, d2, 1);
            continue;
        }
        if (set.ch && ch != (uint32_t)set.ch - 1u) continue;
        if (ty == 0x90u && d2) {
            if (set.arp_mode && page != PG_SEQ && page != PG_EDIT) held_add(d1);
            else { tone_start(d1, d2); }
        } else if (ty == 0x80u || ty == 0x90u) {
            if (set.arp_mode) held_del(d1);
            tone_stop(d1);
        }
    }
    in_midi = 0;
}
static void app_idle(void) { if (idle_ok) midi_in(); }

static void turn_page(uint32_t k, int e) {
    uint32_t m;
    if (!e) return;
    hot = (uint8_t)k;
    dirty = 1;
    t_change = fm1_ms;
    paint_req = 1;
    m = set.machine > 5u ? 5u : set.machine;
    if (page == PG_FX && !fx_alt) {
        if (k == 0u) { set.machine = (uint8_t)clampu((int)set.machine + e, 0, 5); fx_apply(1); return; }
        if (m == 5u) { message("MACHINE IS OFF"); return; }
        if (k == 1u && m == 1u) { message("DX7 IS 12 BITS"); return; }
        if (k == 2u && (m == 0u || m == 1u)) { message("RATE IS FIXED"); return; }
        if (k == 1u) { set.bits[m] = (uint8_t)clampu((int)set.bits[m] + e, 0, 4); fx_apply(0); }
        else if (k == 2u) { set.rate[m] = (uint8_t)clampu((int)set.rate[m] + e, 0, 4); fx_apply(1); }
        else if (m == 4u) { set.drive = (uint8_t)clampu((int)set.drive + e, 0, 8); fx_apply(0); }
        else { set.tone[m] = (uint8_t)clampu((int)set.tone[m] + e, 0, 4); fx_apply(1); }
    } else if (page == PG_FX) {
        if (k == 0u) set.wet = (uint8_t)clampu((int)set.wet + e, 0, 100);
        else if (k == 1u) { set.drive = (uint8_t)clampu((int)set.drive + e, 0, 8); fx_apply(0); }
        else if (m == 5u) message("MACHINE IS OFF");
        else if (k == 2u && m == 1u) message("DX7 IS 12 BITS");
        else if (k == 3u && (m == 0u || m == 1u)) message("RATE IS FIXED");
        else if (k == 2u) { set.bits[m] = (uint8_t)clampu((int)set.bits[m] + e, 0, 4); fx_apply(0); }
        else { set.rate[m] = (uint8_t)clampu((int)set.rate[m] + e, 0, 4); fx_apply(1); }
        audio_params();
    } else if (page == PG_HOME) {
        set.mix[k] = (uint8_t)clampu((int)set.mix[k] + e, 0, 100);
    } else if (page == PG_SEQ) {
        uint32_t len = set.length < 1u ? 1u : set.length;
        if (k == 0u) cursor = (uint8_t)clampu((int)cursor + e, 0, (int)len - 1);
        else if (k == 1u) set.pat[set.pattern][set.track][cursor] = (uint8_t)clampu((int)set.pat[set.pattern][set.track][cursor] + e, 0, 127);
        else if (k == 2u) set.length = (uint8_t)clampu((int)set.length + e, 1, 16);
        else set.swing = (uint8_t)clampu((int)set.swing + e, 50, 75);
    } else if (page == PG_EDIT) {
        uint32_t tr = set.track > 3u ? 0u : set.track;
        set.drum[tr][k] = (uint8_t)clampu((int)set.drum[tr][k] + e, 0, 100);
    } else if (page == PG_ARP) {
        if (k == 0u) set.arp_mode = (uint8_t)clampu((int)set.arp_mode + e, 0, 4);
        else if (k == 1u) set.arp_div = (uint8_t)clampu((int)set.arp_div + e, 0, 3);
        else if (k == 2u) set.arp_gate = (uint8_t)clampu((int)set.arp_gate + e, 10, 100);
        else set.arp_oct = (uint8_t)clampu((int)set.arp_oct + e, 1, 3);
    } else if (page == PG_LFO) {
        if (k == 0u) set.lfo_rate = (uint8_t)clampu((int)set.lfo_rate + e, 0, 100);
        else if (k == 1u) set.lfo_depth = (uint8_t)clampu((int)set.lfo_depth + e, 0, 100);
        else if (k == 2u) set.lfo_cc = (uint8_t)clampu((int)set.lfo_cc + e, 1, 127);
        else set.lfo_dest = (uint8_t)clampu((int)set.lfo_dest + e, 0, 2);
    } else if (page == PG_ENV) {
        uint8_t v = k == 0u ? set.atk : k == 1u ? set.dec : k == 2u ? set.sus : set.rel;
        v = (uint8_t)clampu((int)v + e, 0, 100);
        if (k == 0u) set.atk = v;
        else if (k == 1u) set.dec = v;
        else if (k == 2u) set.sus = v;
        else set.rel = v;
    } else {
        if (k == 0u) set.tempo = (uint8_t)clampu((int)set.tempo + e, 40, 240);
        else if (k == 1u) set.swing = (uint8_t)clampu((int)set.swing + e, 50, 75);
        else if (k == 2u) set.length = (uint8_t)clampu((int)set.length + e, 1, 16);
        else set.ch = (uint8_t)clampu((int)set.ch + e, 0, 16);
    }
}

static uint32_t page_of(uint32_t b) {
    if (b & (1u << BTN_FX)) return PG_FX;
    if (b & (1u << BTN_HOME)) return PG_HOME;
    if (b & (1u << BTN_SEQ)) return PG_SEQ;
    if (b & (1u << BTN_EDIT)) return PG_EDIT;
    if (b & (1u << BTN_ARP)) return PG_ARP;
    if (b & (1u << BTN_LFO)) return PG_LFO;
    if (b & (1u << BTN_ENV)) return PG_ENV;
    if (b & (1u << BTN_GLO)) return PG_GLO;
    return PG_N;
}
static uint8_t btn_for(uint32_t pg) {
    static const uint8_t b[PG_N] = { BTN_FX, BTN_HOME, BTN_SEQ, BTN_EDIT, BTN_ARP, BTN_LFO, BTN_ENV, BTN_GLO };
    return b[pg < PG_N ? pg : 0];
}

static void app_main(void) {
    uint32_t prev = 0, k, rel, seen_gen = 0, clear_arm = 0;
    uint8_t down_note[27];
#if BOARD_SMK37
    uint8_t ext_note[FM1_NKEY];
    smk_pw_t wheel = {0};
    smk_pad_events = 1;
#endif
    for (k = 0; k < 27u; k++) down_note[k] = 0;
    palette_set(1);
    if (settings_load((void *)&set, sizeof set) || set.ver != SET_VER) defaults();
    else clamp_set();
    keys_init();
    audio_params();
    fx_apply(1);
    page = PG_FX;
    paint(1);
    paint_req = 0;

    for (;;) {
        uint32_t keys, down, up, b, pg, base, st;
        int e;
        platform_poll();
        ext_on = (uint8_t)(ext_flag && (fm1_ms - ext_ms) < 500u);
        audio_params();
        offs_flush();
        lfo_update();
        arp_run();
        midi_in();

        if (seq_gen != seen_gen) {
            uint32_t d;
            seen_gen = seq_gen;
            for (d = 0; d < 4u; d++)
                if (seq_vel_out[d]) {
                    midi_out(0x99u, GM[d], seq_vel_out[d]);
                    note_off_later(9u, GM[d], 60u);
                }
            if (page == PG_SEQ) paint_req = 1;
        }
        {
            uint32_t due = clk_out_n, send = due - clk_seen;
            if (send > 4u) send = 4u;
            while (send--) { midi_out(0xF8u, 0, 0); clk_seen++; }
        }

        keys = fm1_in.notes;
        if (skip_keys) { prev = keys; skip_keys = 0; }
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        base = (uint32_t)((int32_t)KEY0 + 12 * (int32_t)set.oct);
        for (k = 0; k < 27u; k++) {
            uint32_t vel = 100;
#if BOARD_SMK37
            vel = fm1_in.key_vel[SMK_KEY_F3 + k];
            if (!vel) vel = 100;
#endif
            if (down >> k & 1u) {
                if (page == PG_SEQ && white_ord[k] != 255u && white_ord[k] < set.length) {
                    uint8_t w = white_ord[k], *cell = (uint8_t *)&set.pat[set.pattern][set.track][w];
                    cursor = w;
                    *cell = *cell ? 0 : 100;
                    if (*cell) request_drum(set.track, 100, 0);
                    dirty = 1; t_change = fm1_ms; paint_req = 1;
                } else if (page == PG_SEQ && black_ord[k] < 4u) {
                    request_drum(black_ord[k], vel, 1);
                } else if (page == PG_EDIT) {
                    request_drum(set.track, vel, 1);
                } else if (black_ord[k] < 4u) {
                    request_drum(black_ord[k], vel, 1);
                } else {
                    uint32_t note = base + k;
                    if (note > 127u) note = 127u;
                    down_note[k] = (uint8_t)note;
                    if (set.arp_mode) held_add((uint8_t)note);
                    else { tone_start(note, vel); midi_out(0x90u | out_ch(), note, vel); }
                }
            }
            if ((up >> k & 1u) && down_note[k]) {
                if (set.arp_mode) held_del(down_note[k]);
                tone_stop(down_note[k]);
                midi_out(0x80u | out_ch(), down_note[k], 0);
                down_note[k] = 0;
            }
        }
#if BOARD_SMK37
        while ((e = smk_extkey_take()) >= 0) {
            uint32_t key = (uint32_t)e >> 8, vel = (uint32_t)e & 0x7Fu;
            if (key >= FM1_NKEY) continue;
            if (vel) {
                uint32_t note = 48u + key + (uint32_t)(12 * (int32_t)set.oct);
                if (note > 127u) note = 127u;
                ext_note[key] = (uint8_t)note;
                if (page != PG_SEQ && page != PG_EDIT) {
                    if (set.arp_mode) held_add((uint8_t)note);
                    else { tone_start(note, vel); midi_out(0x90u | out_ch(), note, vel); }
                }
            } else if (ext_note[key]) {
                if (set.arp_mode) held_del(ext_note[key]);
                tone_stop(ext_note[key]);
                midi_out(0x80u | out_ch(), ext_note[key], 0);
                ext_note[key] = 0;
            }
        }
        while ((e = smk_pad_take()) >= 0) {
            uint32_t pad = ((uint32_t)e >> 8) & 15u, vel = (uint32_t)e & 0x7Fu;
            if (vel) request_drum(pad & 3u, vel, 1);
        }
        {
            int32_t bd = smk_pw_bend(&wheel, fm1_in.wheel[0], fm1_ms);
            bend_q16 = (uint32_t)(65536 + (bd * 3785 >> 12) + (((bd * bd) >> 12) * 109 >> 12));
        }
#endif

        b = fm1_input_edges(&rel);
        pg = page_of(b);
        if (pg < PG_N) go(pg);
        if (b & (1u << BTN_SEL)) {
            if (page == PG_FX) { fx_alt ^= 1u; paint_req = 1; }
            else { set.track = (uint8_t)((set.track + 1u) & 3u); paint_req = 1; }
        }
        if (b & (1u << BTN_PLAY)) transport(!seq_run, 1);
        if (b & (1u << BTN_REC)) { rec ^= 1u; paint_req = 1; }
        if (b & (1u << BTN_SAVE)) save_req = 1;
        if (b & (1u << BTN_OCT_DOWN) && set.oct > -2) { set.oct--; dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_OCT_UP)) {
            if (page == PG_GLO) {
                if (clear_arm && (int32_t)(fm1_ms - clear_arm) < 0) {
                    uint32_t d, s;
                    for (d = 0; d < 4u; d++) for (s = 0; s < 16u; s++) set.pat[set.pattern][d][s] = 0;
                    dirty = 1; t_change = fm1_ms; paint_req = 1; clear_arm = 0;
                    message("CLEARED");
                } else { clear_arm = fm1_ms + 1600u; message("PRESS OCT+ TO CLEAR"); }
            } else if (set.oct < 2) { set.oct++; dirty = 1; t_change = fm1_ms; }
        }
        if ((e = fm1_enc_take(ENC_KNOB1)) != 0) turn_page(0, e);
        if ((e = fm1_enc_take(ENC_KNOB2)) != 0) turn_page(1, e);
        if ((e = fm1_enc_take(ENC_KNOB3)) != 0) turn_page(2, e);
        if ((e = fm1_enc_take(ENC_KNOB4)) != 0) turn_page(3, e);
        if ((e = fm1_enc_take(ENC_SELECT)) != 0) {
            int t = (int)set.track + e;
            while (t < 0) t += 4;
            set.track = (uint8_t)(t & 3);
            paint_req = 1;
        }
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {
            int p = (int)set.pattern + e;
            while (p < 0) p += 8;
            set.pattern = (uint8_t)(p & 7);
            paint_req = 1;
        }
        if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0) {
            set.tempo = (uint8_t)clampu((int)set.tempo + e, 40, 240);
            dirty = 1; t_change = fm1_ms; paint_req = 1;
        }

        for (k = 0; k < PG_N; k++) fm1_led_key(btn_for(k), k == page);
        fm1_led_key(BTN_PLAY, seq_run);
        fm1_led_key(BTN_REC, rec);
        fm1_led_key(BTN_OCT_DOWN, set.oct < 0);
        fm1_led_key(BTN_OCT_UP, set.oct > 0);
        for (k = 0; k < 27u; k++) {
            int on = 0, glow = 0;
            if (page == PG_SEQ) {
                uint8_t w = white_ord[k];
                if (w != 255u && w < set.length) {
                    if (set.pat[set.pattern][set.track][w]) glow = 1;
                    if (seq_run && w == seq_pos) { on = 1; glow = 0; }
                }
            } else if (black_ord[k] < 4u) glow = 1;
            fm1_led_key(KEY_LED(k), on || (down_note[k] && voice[0].stage));
            fm1_led_key_glow(KEY_LED(k), glow && !on);
        }
        {
            uint32_t v;
            for (k = 0; k < 27u && page != PG_SEQ; k++) {
                int lit = 0;
                for (v = 0; v < NV; v++)
                    if (voice[v].stage && voice[v].stage != 4u && voice[v].note == base + k) lit = 1;
                if (lit) fm1_led_key(KEY_LED(k), 1);
            }
        }
#if BOARD_SMK37
        for (k = 0; k < 16u; k++) {
            uint32_t on = 0;
            padled_set(k, on ? 90 : 8, on ? 50 : 4, on ? 10 : 0);
        }
        padled_flush();
#endif

        if (!msg_on() && msg) { msg = 0; hint_req = 1; }
        st = (uint32_t)seq_pos | ((uint32_t)seq_run << 5) | ((uint32_t)set.tempo << 8) |
             ((uint32_t)set.pattern << 16) | ((uint32_t)set.track << 20) | ((uint32_t)page << 24);
        if (paint_req || st != stamp) {
            int full = paint_req || (st >> 24) != (stamp >> 24);
            if ((st & 0xFF00FFu) != (stamp & 0xFF00FFu)) full = 1;
            paint(full || paint_req);
            paint_req = 0;
            stamp = st;
            stamp_drawn = st;
        } else if (hint_req) {
            paint(0);
        }
        (void)stamp_drawn;

        if (save_req && seq_run) { message("STOP TO SAVE"); save_req = 0; }
        else if ((save_req || dirty) && !seq_run && !sounding && (save_req || fm1_ms - t_change > 2000u)) {
            do_save();
            save_req = 0;
        }
        midi_flush();
    }
}
