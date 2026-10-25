/* SPDX-License-Identifier: GPL-3.0-only */
/* Fmdac 0.5. Drums, MIDI tracks, a sequencer, and the converter.
 *
 *   FX     machine, bits, rate, tone. Press FX again: wet, drive, bits, rate.
 *          The law is on the headphones. While a computer streams USB audio,
 *          that stream is mixed in and converted too. USB audio also records
 *          the headphone mix.
 *   HOME   drum levels. Press HOME again to change the drum character:
 *          808, 909, CR78, SDS. These are synthesized, not sample ROMs.
 *          Black keys F# G# A# C# hit the four drums.
 *   GRID   SEQ. White keys are the 16 steps. K4 is the track. PLAY starts.
 *          Press SEQ again for PIANO, again for SONG, again for GRID.
 *   PIANO  keys write the selected MIDI track and move the cursor.
 *   SONG   16 slots. K1 picks the slot, K2 the pattern, K3 the length.
 *          PLAY walks the chain when the length is more than one.
 *   EDIT   tune, decay, snap, drop of a drum. Press EDIT again: the note
 *          of each drum, or the channel of each MIDI track. Press EDIT
 *          again: a kit map. The notes are the published maps of the
 *          instruments OCTACHRON maps to (GM, TR-909, TR-707, TR-727,
 *          TR-08, Volca Beats) and the Hammerhead list its author posted.
 *          This is not every preset stored inside the OCTACHRON app.
 *   ARP    up, down, up-down, random, chord, and as-played. Press ARP again
 *          to latch the held keys.
 *   LFO    MIDI CC, and level or tune on this mix when DEST says so.
 *   ENV    attack, decay, sustain, release of the key tone. Press ENV again:
 *          a MIDI CC envelope on each new note.
 *   GLO    tempo, swing, length, MIDI channel. OCT+ twice clears the pattern.
 *   REC while stopped quantizes the last few seconds into this pattern and
 *   sets the tempo. REC while playing overdubs the playhead. PLAY then plays
 *   that quantized pattern. It is not a free looper.
 *   Tracks: KICK SNARE HAT TOM, then M1 M2 M3 M4. M1-M4 send MIDI and do not
 *   sound here. SELECT and SEL change the track.
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

#ifdef DINGHY_HOST
static int uac_play_on(void) { return 0; }
static int uac_play_frame(int32_t *l, int32_t *r) { *l = 0; *r = 0; return 0; }
#endif

#define SET_VER 5u
enum { PG_FX, PG_HOME, PG_SEQ, PG_EDIT, PG_ARP, PG_LFO, PG_ENV, PG_GLO, PG_N };
enum { VW_GRID, VW_PIANO, VW_SONG };

typedef struct {
    uint8_t ver;
    int8_t oct;
    uint8_t ch, tempo, swing, length, pattern, track, machine, engine;
    uint8_t bits[6], rate[6], tone[6];
    uint8_t wet, drive;
    uint8_t mix[4];
    uint8_t drum[4][4];             /* tune, decay, snap, drop */
    uint8_t atk, dec, sus, rel;
    uint8_t arp_mode, arp_div, arp_gate, arp_oct, arp_latch;
    uint8_t lfo_rate, lfo_depth, lfo_cc, lfo_dest;
    uint8_t env_on, env_cc, env_amt;
    uint8_t gm[4];
    uint8_t mnote[4];
    uint8_t mch[4];
    uint8_t map, bank, dch;         /* kit map, its page, MIDI channel 1..16 */
    uint8_t song[16];
    uint8_t song_len;
    uint8_t pat[8][8][16];          /* drum velocity, or MIDI note (0 = rest) */
    uint8_t pvel[8][4][16];
} Set;
typedef char set_fits[(sizeof(Set) <= 4000u) ? 1 : -1];

static volatile Set set;
static uint8_t page, fx_alt, env_alt, edit_alt, seq_view, rec, hot, save_req, song_slot;
static volatile uint8_t seq_run, audio_ready, sounding, ext_on, fx_bypass, song_i;
static uint8_t idle_ok, in_midi, down_count;
static uint32_t t_change, dirty;
static volatile uint32_t seq_epoch, seq_gen, ext_count, clk_out_n, wet_q8;
static volatile uint8_t seq_pos, seq_vel_out[4], trig_n[4], trig_v[4];
static volatile uint8_t seq_mnote[4], seq_mvel[4];
static volatile uint8_t gain[4];
static volatile int32_t lfo_tune, lfo_lvl = 256;
static CastDac live, live_r;

static const uint32_t INC_C9[12] = {
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};
static const uint32_t DRUM_F[4] = {4480011u, 17530479u, 0u, 9739155u};
static const uint16_t TUNE_Q12[11] = {
    2048, 2340, 2676, 3059, 3497, 4096, 4796, 5616, 6576, 7700, 8192};
/* Original integer characters in the spirit of a modeled 808, 909, CR-78, and SDS.
 * Not those machines' sample ROMs. Pitch and drop are Q8, body and noise are Q7. */
static const uint16_t ENG_PITCH[4] = {240, 280, 176, 360};
static const uint16_t ENG_DROP[4] = {320, 200, 140, 96};
static const uint8_t ENG_CLICK[4] = {2, 7, 1, 4};
static const uint8_t ENG_BODY[4] = {140, 70, 100, 110};
static const uint8_t ENG_NOISE[4] = {90, 170, 60, 150};
static const uint8_t ENG_HAT[4] = {1, 0, 3, 1};
static const int8_t ENG_DEC[4] = {10, -8, 16, -4};
static const uint8_t BITV[5] = {8, 10, 12, 14, 16};
static const char *const MACH[6] = {"VS", "DX7", "SP-1200", "EMU II", "S950", "OFF"};
static const char *const TNAM[5] = {"RAW", "DARK", "OPEN", "BRIGHT", "FULL"};
static const char *const TRK[8] = {"KICK", "SNARE", "HAT", "TOM", "M1", "M2", "M3", "M4"};
static const char *vlab[8];
static const char *const ENG[4] = {"808", "909", "CR78", "SDS"};
static const char *const ENGMSG[4] = {"ENGINE 808", "ENGINE 909", "ENGINE CR78", "ENGINE SDS"};
static const char *const ENGNXT[4] = {
    "PRESS HOME FOR 909", "PRESS HOME FOR CR78", "PRESS HOME FOR SDS", "PRESS HOME FOR 808"};
static const char *const MODE[7] = {"OFF", "UP", "DOWN", "UP-DN", "RAND", "CHORD", "ORDER"};
static const char *const DIVN[4] = {"1/4", "1/8", "1/16", "1/32"};
static const char *const DEST[3] = {"OFF", "TUNE", "LEVEL"};
static const char *const NTN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

/* Published drum-note maps. One page is eight notes: the four sounding
 * drums, then four silent MIDI rows. A second page is eight more notes
 * from the same chart. Channels are the chart's default where it names
 * one, and 1 where the chart leaves the channel to the instrument.
 *
 * GM: MMA General MIDI percussion.
 * 909: Roland TR-909 service notes, June 1984, key assignment. Each voice
 *   answers two note numbers; the first page sends one, the second page
 *   sends the other and the remaining voices.
 * 707 and 727: the same 35-51 assignment, which the TR-707 and TR-727
 *   manuals use. Their extra percussion notes are not split out here.
 * TR-08: Roland TR-08 MIDI implementation chart, August 2017, the notes
 *   the instrument sends. The original TR-808 has no MIDI.
 * Volca Beats: Korg MIDI implementation, notes 36, 38, 39, 42, 43, 46,
 *   49, 50, 67, and 75. Part level is CC 40 through 49.
 * Hammerhead: the eight notes Markus Teufel posted for OCTACHRON. */
static const uint8_t MAP_GM_A[8] = {36, 38, 42, 45, 39, 46, 37, 49};
static const uint8_t MAP_GM_B[8] = {41, 47, 48, 50, 51, 56, 54, 75};
static const uint8_t MAP_909_A[8] = {36, 38, 42, 41, 39, 46, 37, 49};
static const uint8_t MAP_909_B[8] = {45, 48, 51, 35, 40, 43, 47, 50};
static const uint8_t MAP_08_A[8] = {36, 38, 42, 43, 39, 46, 37, 49};
static const uint8_t MAP_08_B[8] = {47, 50, 56, 62, 63, 64, 70, 75};
static const uint8_t MAP_VB_A[8] = {36, 38, 42, 43, 39, 46, 50, 49};
static const uint8_t MAP_VB_B[8] = {36, 38, 42, 50, 39, 46, 67, 75};
static const uint8_t MAP_HH[8] = {36, 38, 42, 45, 39, 46, 48, 50};
static const char *const LAB_GM_A[8] = {"KICK", "SNARE", "HAT", "TOM", "CLAP", "OHAT", "RIM", "CRASH"};
static const char *const LAB_GM_B[8] = {"LTOM", "MTOM", "HTOM", "HT2", "RIDE", "COWB", "TAMB", "CLAV"};
static const char *const LAB_909_A[8] = {"KICK", "SNARE", "HAT", "LTOM", "CLAP", "OHAT", "RIM", "CRASH"};
static const char *const LAB_909_B[8] = {"MTOM", "HTOM", "RIDE", "BD2", "SD2", "LT2", "MT2", "HT2"};
static const char *const LAB_08_A[8] = {"KICK", "SNARE", "HAT", "LTOM", "CLAP", "OHAT", "RIM", "CYM"};
static const char *const LAB_08_B[8] = {"MTOM", "HTOM", "COWB", "HCG", "MCG", "LCG", "MAR", "CLAV"};
static const char *const LAB_VB_A[8] = {"KICK", "SNARE", "HAT", "LTOM", "CLAP", "OHAT", "HTOM", "CRASH"};
static const char *const LAB_VB_B[8] = {"KICK", "SNARE", "HAT", "HTOM", "CLAP", "OHAT", "AGOG", "CLAV"};
static const char *const LAB_HH[8] = {"KICK", "SNARE", "HAT", "RIM", "CLAP", "OHAT", "SNR2", "BELL"};
typedef struct {
    const char *name, *msg, *hint;
    uint8_t ch;
    const uint8_t *a;
    const uint8_t *b;
    const char *const *la;
    const char *const *lb;
} Kit;
static const Kit KITS[] = {
    {"GM", "MAP GM", " GM DRUM NOTES", 10, MAP_GM_A, MAP_GM_B, LAB_GM_A, LAB_GM_B},
    {"909", "MAP 909", " TR-909 NOTES", 10, MAP_909_A, MAP_909_B, LAB_909_A, LAB_909_B},
    {"707", "MAP 707", " TR-707 NOTES 35-51", 10, MAP_909_A, MAP_909_B, LAB_909_A, LAB_909_B},
    {"727", "MAP 727", " TR-727 NOTES 35-51", 10, MAP_909_A, MAP_909_B, LAB_909_A, LAB_909_B},
    {"TR08", "MAP TR08", " TR-08 NOTES", 1, MAP_08_A, MAP_08_B, LAB_08_A, LAB_08_B},
    {"VOLCA", "MAP VOLCA", " VOLCA LEVEL CC 40-49", 1, MAP_VB_A, MAP_VB_B, LAB_VB_A, LAB_VB_B},
    {"HAMR", "MAP HAMR", " HAMMERHEAD NOTES", 1, MAP_HH, 0, LAB_HH, 0},
};
#define NMAP ((uint32_t)(sizeof KITS / sizeof KITS[0]))
static uint32_t map_ix(void) { return set.map < NMAP ? set.map : 0u; }
static const Kit *kit_now(void) { return &KITS[map_ix()]; }
static const char *trk_name(uint32_t tr) {
    if (tr > 7u) tr = 0;
    if (vlab[tr]) return vlab[tr];
    return TRK[tr];
}
static void map_labels(void) {
    const Kit *k = kit_now();
    const char *const *labs;
    uint32_t i;
    if (set.bank && !k->b) set.bank = 0;
    labs = set.bank ? k->lb : k->la;
    for (i = 0; i < 8u; i++) vlab[i] = labs[i];
}
static void apply_map(void) {
    const Kit *k = kit_now();
    const uint8_t *notes;
    uint32_t i;
    if (set.bank && !k->b) set.bank = 0;
    notes = set.bank ? k->b : k->a;
    set.dch = k->ch;
    for (i = 0; i < 4u; i++) {
        set.gm[i] = notes[i];
        set.mnote[i] = notes[4u + i];
        set.mch[i] = k->ch;
    }
    map_labels();
}

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
static void note_text(char *d, uint32_t note) {
    uint32_t n, oct;
    if (!note || note > 127u) { put_str(d, 12, 0, "REST"); return; }
    n = put_str(d, 12, 0, NTN[note % 12u]);
    oct = note / 12u;
    if (oct) put_u(d + n, 12u - n, oct - 1u);
    else put_str(d, 12, n, "0");
}
static uint32_t trk(void) { return set.track > 7u ? 0u : set.track; }
static uint32_t engi(void) { return set.engine > 3u ? 0u : set.engine; }

static int32_t fall(int32_t a, uint32_t sh) {
    int32_t s;
    if (a <= 0) return 0;
    if (sh > 14u) sh = 14u;
    s = a >> sh;
    if (s < 1) s = 1;
    a -= s;
    return a < 20 ? 0 : a;
}

typedef struct {
    uint32_t ph, inc, target;
    int32_t amp, namp, lp;
    uint16_t click;
    uint8_t alive;
} Drum;
static Drum drumv[4];

static void drum_hit(uint32_t d, uint32_t vel) {
    Drum *v;
    uint32_t tune, snap, drop, i, f, mul, base, end, extra, eng;
    if (d > 3u || !vel) return;
    if (vel > 127u) vel = 127u;
    eng = engi();
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
    v->lp = 0;
    if (d == 2u) {
        v->amp = 0;
        v->namp = (int32_t)vel * 140;
        if (v->namp > 20000) v->namp = 20000;
        v->click = 0;
    } else {
        base = (uint32_t)(((uint64_t)DRUM_F[d] * mul) >> 12);
        end = (uint32_t)(((uint64_t)(base ? base : 1u) * ENG_PITCH[eng]) >> 8);
        if (!end) end = 1u;
        v->target = end;
        extra = (uint32_t)(((uint64_t)end * drop) >> 4);
        extra = (uint32_t)(((uint64_t)extra * ENG_DROP[eng]) >> 8);
        v->inc = end + extra;
        v->ph = 0;
        v->amp = (int32_t)vel * 110;
        if (v->amp > 20000) v->amp = 20000;
        v->click = 0;
        v->namp = 0;
        if (d == 1u) {
            v->amp = (int32_t)(((int64_t)v->amp * ENG_BODY[eng]) >> 7);
            v->namp = (int32_t)vel * (int32_t)snap;
            if (v->namp > 18000) v->namp = 18000;
            v->namp = (int32_t)(((int64_t)v->namp * ENG_NOISE[eng]) >> 7);
            if (v->namp > 24000) v->namp = 24000;
            if (v->amp > 24000) v->amp = 24000;
        } else v->click = (uint16_t)(snap * ENG_CLICK[eng]);
    }
    v->alive = 1;
}

typedef struct {
    uint32_t inc, ph, age;
    int32_t amp, peak, sus;
    uint8_t note, stage;
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
        uint8_t note = *(volatile uint8_t *)&set.pat[p][4u + d][step_i];
        uint8_t mv = *(volatile uint8_t *)&set.pvel[p][d][step_i];
        if (v > 127u) v = 127u;
        if (note > 127u) note = 0;
        seq_vel_out[d] = v;
        seq_mnote[d] = note;
        seq_mvel[d] = note ? (mv ? mv : 100u) : 0;
        if (v) drum_hit(d, v);
    }
    seq_pos = step_i;
    seq_gen++;
}
static void advance_step(uint32_t len) {
    uint32_t sl, si;
    step_i++;
    if (step_i >= len) {
        step_i = 0;
        sl = set.song_len;
        if (sl > 1u) {
            if (sl > 16u) sl = 16u;
            si = (uint32_t)song_i + 1u;
            if (si >= sl) si = 0;
            song_i = (uint8_t)si;
            set.pattern = set.song[si];
        }
    }
    render_step();
}

static void app_audio(int32_t *out, uint32_t n) {
    uint32_t i, d, tempo, swing, len, sh[4], tune[4], drop[4], gv[4], eng;
    int32_t lt, lvl, wq;
    uint32_t any = 0;
    int ext;
    if (!audio_ready) return;
    tempo = set.tempo;
    swing = set.swing;
    len = set.length;
    eng = engi();
    if (tempo < 40u) tempo = 40u;
    if (len < 1u) len = 1u;
    if (len > 16u) len = 16u;
    lt = lfo_tune;
    lvl = lfo_lvl;
    if (lvl < 32) lvl = 32;
    if (lvl > 512) lvl = 512;
    wq = (int32_t)wet_q8;
    for (d = 0; d < 4u; d++) {
        int decs = (int)set.drum[d][1] + (int)ENG_DEC[eng];
        gv[d] = gain[d];
        tune[d] = set.drum[d][0];
        drop[d] = set.drum[d][3];
        if (decs < 0) decs = 0;
        if (decs > 100) decs = 100;
        sh[d] = 6u + (uint32_t)decs * 8u / 100u;
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
            advance_step(len);
        }
    } else if (seq_run) {
        uint32_t lim = step_len(step_i, tempo, swing), guard = 0, cl = 110250u / tempo;
        step_acc += n;
        while (step_acc >= lim && guard++ < 4u) {
            step_acc -= lim;
            advance_step(len);
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
            uint32_t ph, inc;
            int32_t amp;
            if (!v->stage && !v->amp) continue;
            any = 1;
            if (v->peak) v->sus = v->peak * (int32_t)sus / 100;
            ph = v->ph;
            amp = v->amp;
            inc = (uint32_t)(((uint64_t)v->inc * bend_q16) >> 16);
            if (lt) {
                int32_t m = 1024 + lt;
                if (m < 256) m = 256;
                inc = (uint32_t)(((int64_t)inc * m) >> 10);
            }
            for (i = 0; i < n; i++) {
                int32_t s;
                if (v->stage == 1u) {
                    amp += (int32_t)as;
                    if (amp >= v->peak) { amp = v->peak; v->stage = 2u; }
                } else if (v->stage == 2u) {
                    if (amp > v->sus) {
                        int32_t sub = (amp - v->sus) >> ds;
                        if (sub < 1) sub = 1;
                        amp -= sub;
                        if (amp < v->sus) amp = v->sus;
                    } else v->stage = 3u;
                } else if (v->stage == 3u) {
                    amp = v->sus;
                } else if (v->stage == 4u || !v->stage) {
                    amp = fall(amp, rs);
                    if (!amp) v->stage = 0;
                }
                ph += inc;
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
        cut = 1u + (tune[d] > 100u ? 0u : (100u - tune[d]) / 25u) + ENG_HAT[eng];
        if (cut < 1u) cut = 1u;
        if (cut > 8u) cut = 8u;
        for (i = 0; i < n; i++) {
            int32_t s = 0;
            uint32_t osc;
            if (d == 2u) {
                int32_t nz = noise(), hp, mixed;
                v->lp += (nz - v->lp) >> cut;
                hp = nz - v->lp;
                mixed = hp + ((v->lp * (int32_t)drop[d]) >> 7);
                s = (mixed * v->namp) >> 15;
                v->namp = fall(v->namp, dsh);
            } else {
                osc = v->inc;
                if (lt) {
                    int32_t m = 1024 + lt;
                    if (m < 256) m = 256;
                    osc = (uint32_t)(((int64_t)osc * m) >> 10);
                }
                s = (sine(v->ph) * v->amp) >> 15;
                v->ph += osc;
                if (v->click) { s += noise() >> 2; v->click--; }
                if (d == 1u) {
                    s += (noise() * v->namp) >> 15;
                    v->namp = fall(v->namp, dsh > 1u ? dsh - 1u : 1u);
                }
                v->amp = fall(v->amp, dsh);
                if (eng == 2u) {
                    v->lp += (s - v->lp) >> 2;
                    s = v->lp;
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
    ext = uac_play_on();
    if (!any && !ext) {
        cast_dac_reset(&live);
        cast_dac_reset(&live_r);
        for (i = 0; i < n; i++) out[2u * i] = out[2u * i + 1u] = 0;
    } else if (!ext) {
        cast_dac_reset(&live_r);
        for (i = 0; i < n; i++) {
            int32_t x = sat(out[2u * i]), y, wet;
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
    } else {
        any = 1;
        for (i = 0; i < n; i++) {
            int32_t mono = out[2u * i], cl = 0, cr = 0, xl, xr, wl, wr;
            uac_play_frame(&cl, &cr);
            xl = sat(mono + cl);
            xr = sat(mono + cr);
            if (!fx_bypass && wq) {
                int32_t wetl = cast_dac_process(&live, xl);
                int32_t wetr = cast_dac_process(&live_r, xr);
                wl = xl + (int32_t)(((int64_t)(wetl - xl) * wq) >> 8);
                wr = xr + (int32_t)(((int64_t)(wetr - xr) * wq) >> 8);
            } else {
                wl = xl;
                wr = xr;
            }
            wl = sat((int32_t)(((int64_t)wl * lvl) >> 8));
            wr = sat((int32_t)(((int64_t)wr * lvl) >> 8));
            out[2u * i] = wl;
            out[2u * i + 1u] = wr;
        }
    }
    sounding = (uint8_t)any;
    (void)clk_len_hold;
}

static void bq_coef(CastBq *d, const CastCoef *s) {
    d->b0 = s->b0; d->b1 = s->b1; d->b2 = s->b2; d->a1 = s->a1; d->a2 = s->a2;
}
static void fx_program(CastDac *dst, int reset_state) {
    uint32_t m, tone, rate, bits, hz, step, i;
    m = set.machine;
    if (m > 5u) m = 5u;
    fx_bypass = (uint8_t)(m >= 5u);
    dst->npre = 0;
    dst->npost = 0;
    dst->dc_block = 0;
    dst->drive_q8 = set.drive ? (int)(256 + (uint32_t)set.drive * 40u) : 0;
    if (m >= 5u) {
        dst->law = CAST_LAW_LINEAR;
        dst->bits = 16;
        dst->step = 65536u;
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
        dst->step = step;
        dst->bits = (m == 1u) ? 12 : (int)BITV[bits];
        if (m == 0u) {
            dst->law = CAST_LAW_LINEAR;
            for (i = 0; i < 2u; i++) bq_coef(&dst->post[i], &CAST_VS_POST[tone][i]);
            dst->npost = 2;
        } else if (m == 1u) {
            dst->law = CAST_LAW_DX7;
            dst->bits = 12;
        } else if (m == 2u) {
            dst->law = CAST_LAW_LINEAR;
            bq_coef(&dst->pre[0], &CAST_SP_PRE[0]);
            bq_coef(&dst->pre[1], &CAST_SP_PRE[1]);
            dst->npre = 2;
            dst->npost = CAST_SP_NPOST[tone];
            for (i = 0; i < dst->npost; i++) bq_coef(&dst->post[i], &CAST_SP_POST[tone][i]);
        } else if (m == 3u) {
            dst->law = CAST_LAW_DPCM;
            dst->dc_block = 1;
            bq_coef(&dst->pre[0], &CAST_EII_PRE[0]);
            dst->npre = 1;
            dst->npost = 2;
            for (i = 0; i < 2u; i++) bq_coef(&dst->post[i], &CAST_EII_POST[tone][i]);
        } else {
            dst->law = CAST_LAW_LINEAR;
            dst->npre = 3;
            dst->npost = 3;
            for (i = 0; i < 3u; i++) {
                bq_coef(&dst->pre[i], &CAST_S950_PRE[rate][i]);
                bq_coef(&dst->post[i], &CAST_S950_POST[rate][i]);
            }
        }
    }
    if (reset_state) cast_dac_reset(dst);
}
static void fx_apply(int reset_state) {
    fm1_irq_off();
    fx_program(&live, reset_state);
    fx_program(&live_r, 1);
    audio_ready = 1;
    fm1_irq_on();
    (void)CAST_TONE_MILLI[0];
}

static uint8_t paint_req = 1, hint_req, skip_keys;
static const char *msg;
static uint32_t msg_until;
static char msgbuf[24];
static char lab[4][20], val[4][12];
static int barv[4], barlo[4], barhi[4];
static uint8_t white_ord[27], black_ord[27], white_at[16];
static uint8_t nwhite, nblack;
static uint8_t cursor;

static void message(const char *s) {
    msg = s;
    msg_until = fm1_ms + 1400u;
    hint_req = 1;
}
static int msg_on(void) { return msg && (int32_t)(fm1_ms - msg_until) < 0; }
static void message_take(uint32_t bpm) {
    uint32_t n = put_str(msgbuf, 24, 0, "TAKE ");
    n = put_u_at(msgbuf, 24, n, bpm);
    put_str(msgbuf, 24, n, " BPM");
    message(msgbuf);
}
static void message_step(uint32_t step, uint32_t note) {
    char nm[12];
    uint32_t n;
    note_text(nm, note);
    n = put_str(msgbuf, 24, 0, "STEP ");
    n = put_u_at(msgbuf, 24, n, step);
    n = put_str(msgbuf, 24, n, " ");
    put_str(msgbuf, 24, n, nm);
    message(msgbuf);
}

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
    uint32_t fill;
    if (hi <= lo) return 0;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    fill = (uint32_t)(v - lo) * w / (uint32_t)(hi - lo);
    if (fill < 6u && w >= 6u) fill = 6u;
    return fill;
}
static void num(char *d, uint32_t v) { put_u(d, 12, v); }
static void ch_text(char *d, uint32_t ch) {
    uint32_t n;
    if (!ch) { put_str(d, 12, 0, "OMNI"); return; }
    n = put_str(d, 12, 0, "CH ");
    put_u(d + n, 12u - n, ch);
}

static void fill_rows(void) {
    char n0[12], n1[12], n2[12], n3[12];
    uint32_t m = set.machine, tr = trk();
    if (m > 5u) m = 5u;
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
        }
    } else if (page == PG_EDIT && edit_alt == 2u) {
        const Kit *k = kit_now();
        uint32_t note = tr < 4u ? set.gm[tr] : set.mnote[tr - 4u];
        note_text(n2, note);
        ch_text(n3, set.dch);
        row_set(0, "K1", "MAP", k->name, (int)map_ix(), 0, (int)NMAP - 1);
        row_set(1, "K2", "PAGE", set.bank ? "2" : "1", (int)set.bank, 0, k->b ? 1 : 0);
        row_set(2, "K3", trk_name(tr), n2, (int)note, 1, 127);
        row_set(3, "K4", "CHANNEL", n3, (int)set.dch, 1, 16);
    } else if (page == PG_EDIT && edit_alt) {
        uint32_t i;
        for (i = 0; i < 4u; i++) {
            char k[3] = {'K', (char)('1' + i), 0};
            if (tr >= 4u) {
                ch_text(n0, set.mch[i]);
                row_set(i, k, trk_name(4u + i), n0, (int)set.mch[i], 1, 16);
            } else {
                note_text(n0, set.gm[i]);
                row_set(i, k, trk_name(i), n0, (int)set.gm[i], 0, 127);
            }
        }
    } else if (page == PG_EDIT && tr >= 4u) {
        uint32_t mt = tr - 4u, len = set.length < 1u ? 1u : set.length;
        uint32_t st = cursor >= len ? 0u : cursor;
        ch_text(n0, set.mch[mt]);
        note_text(n1, set.mnote[mt]);
        note_text(n2, set.pat[set.pattern][tr][st]);
        num(n3, set.pvel[set.pattern][mt][st]);
        row_set(0, "K1", "CHANNEL", n0, (int)set.mch[mt], 1, 16);
        row_set(1, "K2", "BASE", n1, (int)set.mnote[mt], 1, 127);
        row_set(2, "K3", "NOTE", n2, (int)set.pat[set.pattern][tr][st], 0, 127);
        row_set(3, "K4", "VELOCITY", n3, (int)set.pvel[set.pattern][mt][st], 0, 127);
    } else if (page == PG_EDIT) {
        static const char *NM[4] = {"TUNE", "DECAY", "SNAP", "DROP"};
        uint32_t i;
        for (i = 0; i < 4u; i++) {
            char k[3] = {'K', (char)('1' + i), 0};
            num(n0, set.drum[tr][i]);
            row_set(i, k, NM[i], n0, (int)set.drum[tr][i], 0, 100);
        }
    } else if (page == PG_ARP) {
        uint32_t mode = set.arp_mode > 6u ? 0u : set.arp_mode;
        uint32_t div = set.arp_div > 3u ? 2u : set.arp_div;
        num(n2, set.arp_gate);
        num(n3, set.arp_oct);
        row_set(0, "K1", "MODE", MODE[mode], (int)mode, 0, 6);
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
    } else if (page == PG_ENV && env_alt) {
        num(n1, set.env_cc);
        num(n2, set.env_amt);
        row_set(0, "K1", "SEND", set.env_on ? "ON" : "OFF", (int)set.env_on, 0, 1);
        row_set(1, "K2", "CC", n1, (int)set.env_cc, 1, 127);
        row_set(2, "K3", "AMOUNT", n2, (int)set.env_amt, 0, 100);
        row_set(3, "K4", "TRIG", "NOTE", 1, 0, 1);
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
        num(n0, set.tempo);
        num(n1, set.swing);
        num(n2, set.length);
        ch_text(n3, set.ch);
        row_set(0, "K1", "TEMPO", n0, (int)set.tempo, 40, 240);
        row_set(1, "K2", "SWING", n1, (int)set.swing, 50, 75);
        row_set(2, "K3", "LENGTH", n2, (int)set.length, 1, 16);
        row_set(3, "K4", "CHANNEL", n3, (int)set.ch, 0, 16);
    }
}

static void fill_seq(void) {
    char n0[12], n1[12], n2[12];
    uint32_t len = set.length, tr = trk(), vel;
    if (len < 1u) len = 1u;
    if (len > 16u) len = 16u;
    if (cursor >= len) cursor = (uint8_t)(len - 1u);
    if (seq_view == VW_SONG) {
        uint32_t sl = set.song_len < 1u ? 1u : set.song_len;
        uint32_t slot, patn;
        if (sl > 16u) sl = 16u;
        if (song_slot >= sl) song_slot = (uint8_t)(sl - 1u);
        slot = song_slot;
        patn = set.song[slot];
        if (patn > 7u) patn = 0;
        num(n0, slot + 1u);
        num(n1, patn + 1u);
        num(n2, sl);
        row_set(0, "K1", "SLOT", n0, (int)slot, 0, (int)sl - 1);
        row_set(1, "K2", "PATTERN", n1, (int)patn, 0, 7);
        row_set(2, "K3", "LENGTH", n2, (int)sl, 1, 16);
        row_set(3, "K4", "TRACK", trk_name(tr), (int)tr, 0, 7);
        return;
    }
    if (tr >= 4u) {
        note_text(n1, set.pat[set.pattern][tr][cursor]);
        num(n0, (uint32_t)cursor + 1u);
        num(n2, len);
        row_set(0, "K1", "STEP", n0, (int)cursor, 0, (int)len - 1);
        row_set(1, "K2", "NOTE", n1, (int)set.pat[set.pattern][tr][cursor], 0, 127);
        if (seq_view == VW_PIANO) {
            num(n2, set.pvel[set.pattern][tr - 4u][cursor]);
            row_set(2, "K3", "VELOCITY", n2, (int)set.pvel[set.pattern][tr - 4u][cursor], 1, 127);
        } else row_set(2, "K3", "LENGTH", n2, (int)len, 1, 16);
        row_set(3, "K4", "TRACK", trk_name(tr), (int)tr, 0, 7);
        return;
    }
    vel = set.pat[set.pattern][tr][cursor];
    num(n0, (uint32_t)cursor + 1u);
    num(n1, vel);
    num(n2, len);
    row_set(0, "K1", "STEP", n0, (int)cursor, 0, (int)len - 1);
    row_set(1, "K2", "VELOCITY", n1, (int)vel, 0, 127);
    row_set(2, "K3", "LENGTH", n2, (int)len, 1, 16);
    row_set(3, "K4", "TRACK", trk_name(tr), (int)tr, 0, 7);
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
    uint32_t i, x = 0, len = set.length, tr = trk(), pat = set.pattern;
    if (len < 1u) len = 1u;
    if (len > 16u) len = 16u;
    if (pat > 7u) pat = 0;
    cv_begin(240, 48, C_BLACK);
    for (i = 0; i < 16u; i++) {
        uint32_t vel = 0, h = 0;
        uint16_t col = C_DIM;
        if (i < len) {
            if (tr >= 4u) {
                uint32_t note = set.pat[pat][tr][i];
                vel = note ? set.pvel[pat][tr - 4u][i] : 0;
                if (note && !vel) vel = 100;
            } else vel = set.pat[pat][tr][i];
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
static void draw_song(void) {
    uint32_t i, x = 0, sl = set.song_len;
    if (sl < 1u) sl = 1u;
    if (sl > 16u) sl = 16u;
    cv_begin(240, 48, C_BLACK);
    for (i = 0; i < 16u; i++) {
        uint32_t h = 0, p = 0;
        uint16_t col = C_DIM;
        if (i < sl) {
            p = set.song[i];
            if (p > 7u) p = 0;
            h = 8u + p * 3u;
            if (seq_run && i == song_i) col = C_WHITE;
            else if (i == song_slot) col = C_HI;
            else col = C_AMB;
        }
        cv_rect((int32_t)x, 4, 12, 30, C_LINE);
        if (h) cv_rect((int32_t)x, 4 + (int32_t)(30u - h), 12, (int32_t)h, col);
        if (i == song_slot) cv_rect((int32_t)x, 40, 12, 4, C_WHITE);
        x += 15u;
    }
    cv_blit(0, 16);
}
static void header_text(char *d) {
    const char *name = "FX";
    uint32_t n, tr = trk();
    if (page == PG_FX && fx_alt) name = "WET";
    else if (page == PG_HOME) name = "HOME";
    else if (page == PG_SEQ && seq_view == VW_PIANO) name = "PIANO";
    else if (page == PG_SEQ && seq_view == VW_SONG) name = "SONG";
    else if (page == PG_SEQ) name = "GRID";
    else if (page == PG_EDIT && edit_alt == 2u) name = "MAP";
    else if (page == PG_EDIT && edit_alt) name = "NOTE";
    else if (page == PG_EDIT) name = "EDIT";
    else if (page == PG_ARP) name = "ARP";
    else if (page == PG_LFO) name = "LFO";
    else if (page == PG_ENV && env_alt) name = "CC";
    else if (page == PG_ENV) name = "ENV";
    else if (page == PG_GLO) name = "GLO";
    n = put_str(d, 40, 0, " ");
    n = put_str(d, 40, n, name);
    if (page == PG_HOME) {
        n = put_str(d, 40, n, " ");
        n = put_str(d, 40, n, ENG[engi()]);
    } else if (page == PG_EDIT && edit_alt == 2u) {
        n = put_str(d, 40, n, " ");
        n = put_str(d, 40, n, kit_now()->name);
    }
    n = put_str(d, 40, n, "  ");
    n = put_str(d, 40, n, trk_name(tr));
    n = put_str(d, 40, n, "  v");
    put_str(d, 40, n, APP_VER);
}
static void status_text(char *d) {
    uint32_t n, tr = trk();
    n = put_str(d, 40, 0, " ");
    n = put_str(d, 40, n, trk_name(tr));
    n = put_str(d, 40, n, seq_run ? " PLAY " : " ");
    n = put_u_at(d, 40, n, set.tempo);
    if (seq_run && set.song_len > 1u) {
        n = put_str(d, 40, n, " SNG ");
        n = put_u_at(d, 40, n, (uint32_t)song_i + 1u);
    } else {
        n = put_str(d, 40, n, " PAT ");
        n = put_u_at(d, 40, n, (uint32_t)set.pattern + 1u);
    }
    if (seq_run) {
        n = put_str(d, 40, n, " S");
        put_u_at(d, 40, n, (uint32_t)seq_pos + 1u);
    }
}
static const char *hint_text(void) {
    if (msg_on()) return msg;
    if (page == PG_FX && fx_alt) return " PRESS FX FOR THE LAW";
    if (page == PG_FX) return " USB IN GOES TO PHONES";
    if (page == PG_HOME) return ENGNXT[engi()];
    if (page == PG_SEQ && rec && seq_run) return " REC WRITES THE STEP";
    if (page == PG_SEQ && seq_view == VW_PIANO && trk() < 4u) return " SELECT A MIDI TRACK";
    if (page == PG_SEQ && seq_view == VW_PIANO) return " KEYS WRITE A STEP";
    if (page == PG_SEQ && seq_view == VW_SONG) return " PLAY WALKS THE CHAIN";
    if (page == PG_SEQ) return " K4 OR SELECT = TRACK";
    if (page == PG_EDIT && edit_alt == 2u) return kit_now()->hint;
    if (page == PG_EDIT && edit_alt) return " PRESS EDIT FOR THE MAP";
    if (page == PG_EDIT) return " PRESS EDIT FOR NOTES";
    if (page == PG_ARP && set.arp_latch) return " LATCH HOLDS THE KEYS";
    if (page == PG_ARP) return " PRESS ARP TO LATCH";
    if (page == PG_LFO) return " DEPTH IS AUDIBLE";
    if (page == PG_ENV && env_alt) return " CC ON EACH NOTE";
    if (page == PG_ENV) return " PRESS ENV FOR CC";
    return " OCT+ TWICE CLEARS";
}

static uint32_t stamp;
static void paint(int full) {
    char line[40];
    uint32_t i;
    idle_ok = 1;
    if (page == PG_SEQ) fill_seq();
    else fill_rows();
    header_text(line);
#ifdef DINGHY_HOST
    {
        char stat[40];
        status_text(stat);
        printf("%6u ms  UI [%s] [%s] [%s]\n", fm1_ms, line, stat, hint_text());
    }
#endif
    if (full) {
        draw_text_box(0, 0, 240, &FONT_S, line, C_HI, 0);
        if (page == PG_SEQ) {
            if (seq_view == VW_SONG) draw_song();
            else draw_grid();
            for (i = 0; i < 4u; i++) draw_small(i, 64u + i * 36u);
        } else {
            for (i = 0; i < 4u; i++) draw_big(i, 16u + i * 48u);
        }
    } else if (page == PG_SEQ) {
        if (seq_view == VW_SONG) draw_song();
        else draw_grid();
    }
    status_text(line);
    draw_text_box(0, 208, 240, &FONT_S, line, C_AMB, 0);
    draw_text_box(0, 224, 240, &FONT_S, hint_text(), msg_on() ? C_WHITE : C_GRAY, 0);
    idle_ok = 0;
    hint_req = 0;
}

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
static uint32_t midi_ch(uint32_t tr) {
    uint32_t ch;
    if (tr >= 4u) tr = 0;
    ch = set.mch[tr];
    if (ch < 1u || ch > 16u) ch = 1u;
    return ch - 1u;
}
static uint32_t drum_ch(void) {
    uint32_t ch = set.dch;
    if (ch < 1u || ch > 16u) ch = 10u;
    return ch - 1u;
}

static uint8_t menv_st;
static int32_t menv;
static int16_t menv_sent = -1;
static uint32_t menv_ms, menv_until;
static void menv_release(void) {
    if (!down_count && menv_st && menv_st < 4u) menv_st = 4u;
}
static void menv_trig(int held) {
    if (!set.env_on) return;
    menv_st = 1u;
    menv_until = fm1_ms + (held ? 60000u : 180u);
}
static void menv_tick(void) {
    uint32_t dt, cc, amt;
    int32_t step, sus;
    if (!set.env_on) {
        if (menv_sent >= 0) {
            midi_out(0xB0u | out_ch(), set.env_cc ? set.env_cc : 11u, 0);
            menv_sent = -1;
        }
        menv_st = 0;
        menv = 0;
        return;
    }
    dt = fm1_ms - menv_ms;
    menv_ms = fm1_ms;
    if (dt > 40u) dt = 40u;
    if (menv_st && menv_st < 4u && !down_count && (int32_t)(fm1_ms - menv_until) >= 0)
        menv_st = 4u;
    sus = set.sus > 100u ? 100 : (int32_t)set.sus;
    if (menv_st == 1u && dt) {
        step = 1 + (int)(100u - (set.atk > 100u ? 100u : set.atk)) / 4;
        menv += step * (int32_t)dt;
        if (menv >= 100) { menv = 100; menv_st = 2u; }
    } else if (menv_st == 2u && dt) {
        if (menv > sus) {
            menv -= (int32_t)dt;
            if (menv < sus) menv = sus;
        } else menv_st = 3u;
    } else if (menv_st == 3u) menv = sus;
    else if (menv_st == 4u && dt) {
        menv -= (int32_t)dt;
        if (menv <= 0) { menv = 0; menv_st = 0; }
    }
    amt = set.env_amt > 100u ? 100u : set.env_amt;
    cc = (uint32_t)(menv * (int32_t)amt / 100);
    if (cc > 127u) cc = 127u;
    if ((int32_t)cc != menv_sent) {
        uint32_t prev = menv_sent < 0 ? 255u : (uint32_t)menv_sent;
        uint32_t diff = cc > prev ? cc - prev : prev - cc;
        /* The screen redraw can stall the main loop, and the MIDI queue is short. */
        if (diff < 4u && cc != 0u) return;
        midi_out(0xB0u | out_ch(), set.env_cc ? set.env_cc : 11u, cc);
        menv_sent = (int16_t)cc;
    }
}

#define NTAKE 48u
typedef struct { uint16_t dt; uint8_t note, vel; } TakeEv;
static TakeEv takeq[NTAKE];
static uint8_t take_n, take_w;
static uint32_t take_last, take_span;

static void take_push(uint32_t note, uint32_t vel) {
    uint32_t now, dt = 0;
    if (!note || !vel) return;
    if (vel > 127u) vel = 127u;
    if (note > 255u) return;
    now = fm1_ms;
    if (take_n) {
        dt = now - take_last;
        if (dt > 2000u) dt = 2000u;
        while (take_n && take_span + dt > 8000u) {
            uint32_t old = (take_w + NTAKE - take_n) % NTAKE;
            uint32_t next;
            if (take_span >= takeq[old].dt) take_span -= takeq[old].dt;
            else take_span = 0;
            take_n--;
            if (!take_n) break;
            next = (old + 1u) % NTAKE;
            if (take_span >= takeq[next].dt) take_span -= takeq[next].dt;
            else take_span = 0;
            takeq[next].dt = 0;
        }
    }
    takeq[take_w].dt = (uint16_t)dt;
    takeq[take_w].note = (uint8_t)note;
    takeq[take_w].vel = (uint8_t)vel;
    take_w = (uint8_t)((take_w + 1u) % NTAKE);
    if (take_n < NTAKE) take_n++;
    take_span += dt;
    take_last = now;
}

static int drum_ix(uint32_t note) {
    uint32_t i;
    for (i = 0; i < 4u; i++) if (set.gm[i] == note) return (int)i;
    if (note == 35u || note == 36u) return 0;
    if (note == 38u || note == 40u) return 1;
    if (note == 42u || note == 44u || note == 46u) return 2;
    if (note == 41u || note == 43u || note == 45u || note == 47u || note == 48u || note == 50u) return 3;
    return -1;
}

static void request_drum(uint32_t d, uint32_t vel, int record) {
    if (d > 3u || !vel) return;
    if (vel > 127u) vel = 127u;
    trig_v[d] = (uint8_t)vel;
    trig_n[d]++;
    midi_out(0x90u | drum_ch(), set.gm[d], vel);
    note_off_later(drum_ch(), set.gm[d], 70u);
    take_push(128u + d, vel);
    menv_trig(0);
    if (record && rec && seq_run) {
        uint8_t s = seq_pos, len = set.length;
        if (len < 1u) len = 1u;
        if (s < len) {
            set.pat[set.pattern][d][s] = (uint8_t)vel;
            paint_req = 1;
        }
    }
}

static uint32_t take_bpm(void) {
    uint16_t iv[NTAKE];
    uint32_t n = 0, i, j, med, bpm = 0, best = 0, oldest;
    if (take_n < 2u) return 0;
    oldest = (take_w + NTAKE - take_n) % NTAKE;
    for (i = 1; i < take_n; i++) {
        uint16_t dt = takeq[(oldest + i) % NTAKE].dt;
        if (dt >= 40u && dt <= 2000u) iv[n++] = dt;
    }
    if (!n) return 0;
    for (i = 1; i < n; i++) {
        uint16_t x = iv[i];
        j = i;
        while (j && iv[j - 1u] > x) { iv[j] = iv[j - 1u]; j--; }
        iv[j] = x;
    }
    med = iv[n / 2u];
    if (!med) return 0;
    for (i = 1; i <= 4u; i <<= 1) {
        uint32_t b, score;
        if (med * i == 0 || med * i > 60000u) continue;
        b = 60000u / (med * i);
        if (b < 40u || b > 240u) continue;
        score = (b >= 70u && b <= 180u) ? 2u : 1u;
        if (score > best) { best = score; bpm = b; }
    }
    return bpm;
}
static void take_commit(void) {
    uint32_t bpm, i, t, step_ms, len, pat, dest, oldest, mask = 0, melodic = 0;
    bpm = take_bpm();
    if (!bpm) { message("NO TAKE"); return; }
    len = set.length < 1u ? 1u : set.length;
    if (len > 16u) len = 16u;
    pat = set.pattern > 7u ? 0u : set.pattern;
    dest = trk() >= 4u ? trk() : 4u;
    oldest = (take_w + NTAKE - take_n) % NTAKE;
    for (i = 0; i < take_n; i++) {
        uint8_t note = takeq[(oldest + i) % NTAKE].note;
        if (note >= 128u && note < 132u) mask |= 1u << (note - 128u);
        else if (note) melodic = 1u;
    }
    for (i = 0; i < 4u; i++) if (mask & (1u << i)) {
        uint32_t s;
        for (s = 0; s < 16u; s++) set.pat[pat][i][s] = 0;
    }
    if (melodic) {
        uint32_t s;
        for (s = 0; s < 16u; s++) {
            set.pat[pat][dest][s] = 0;
            set.pvel[pat][dest - 4u][s] = 0;
        }
    }
    step_ms = 15000u / bpm;
    if (step_ms < 1u) step_ms = 1u;
    t = 0;
    for (i = 0; i < take_n; i++) {
        TakeEv *e = &takeq[(oldest + i) % NTAKE];
        uint32_t step;
        t += e->dt;
        step = (t + step_ms / 2u) / step_ms % len;
        if (e->note >= 128u && e->note < 132u)
            set.pat[pat][(uint32_t)e->note - 128u][step] = e->vel ? e->vel : 100u;
        else if (melodic && e->note && e->note < 128u) {
            set.pat[pat][dest][step] = e->note;
            set.pvel[pat][dest - 4u][step] = e->vel ? e->vel : 100u;
        }
    }
    set.tempo = (uint8_t)bpm;
    dirty = 1;
    t_change = fm1_ms;
    paint_req = 1;
    message_take(bpm);
}

static uint8_t held[16], nheld, played[16], nplayed;
static uint8_t arp_idx, arp_oct_i, arp_dir = 1, arp_on, arp_notes[8], arp_nn;
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
static void played_add(uint8_t note) {
    uint32_t i;
    for (i = 0; i < nplayed; i++) if (played[i] == note) return;
    if (nplayed < 16u) played[nplayed++] = note;
}
static void played_del(uint8_t note) {
    uint32_t i, j;
    for (i = 0; i < nplayed; i++) if (played[i] == note) {
        for (j = i + 1u; j < nplayed; j++) played[j - 1u] = played[j];
        nplayed--;
        return;
    }
}
static void note_hold(uint8_t note) { held_add(note); played_add(note); }
static void note_drop(uint8_t note) {
    if (set.arp_latch) return;
    held_del(note);
    played_del(note);
}
static void arp_stop(void) {
    uint32_t i;
    for (i = 0; i < arp_nn; i++) {
        tone_stop(arp_notes[i]);
        midi_out(0x80u | out_ch(), arp_notes[i], 0);
    }
    arp_nn = 0;
    arp_on = 0;
}
static void arp_fire(uint32_t note) {
    if (note > 127u || arp_nn >= 8u) return;
    tone_start(note, 108u);
    midi_out(0x90u | out_ch(), note, 108u);
    arp_notes[arp_nn++] = (uint8_t)note;
    arp_on = 1;
}
static void arp_run(void) {
    uint32_t bpm, quarter, iv, mode, gate, note, octs, nsrc;
    uint8_t *src;
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
    mode = set.arp_mode > 6u ? 0u : set.arp_mode;
    octs = set.arp_oct < 1u ? 1u : set.arp_oct;
    if (octs > 3u) octs = 3u;
    gate = set.arp_gate < 10u ? 10u : set.arp_gate;
    if (gate > 100u) gate = 100u;
    if (mode == 5u) {
        uint32_t i, o;
        arp_stop();
        for (o = 0; o < octs; o++)
            for (i = 0; i < nheld && arp_nn < 4u; i++)
                arp_fire((uint32_t)held[i] + 12u * o);
        arp_off = fm1_ms + iv * gate / 100u;
        return;
    }
    src = mode == 6u ? played : held;
    nsrc = mode == 6u ? nplayed : nheld;
    if (!nsrc) { arp_stop(); return; }
    if (mode == 4u) {
        lcg = lcg * 1664525u + 1013904223u;
        arp_idx = (uint8_t)((lcg >> 16) % nsrc);
        arp_oct_i = (uint8_t)((lcg >> 8) % octs);
    } else if (mode == 2u) {
        if (!arp_idx) { arp_idx = (uint8_t)(nsrc - 1u); arp_oct_i = (uint8_t)((arp_oct_i + octs - 1u) % octs); }
        else arp_idx--;
    } else if (mode == 3u) {
        if (arp_dir) {
            if (arp_idx + 1u >= nsrc) arp_dir = 0;
            else arp_idx++;
        } else if (!arp_idx) { arp_dir = 1; arp_oct_i = (uint8_t)((arp_oct_i + 1u) % octs); }
        else arp_idx--;
    } else {
        arp_idx++;
        if (arp_idx >= nsrc) { arp_idx = 0; arp_oct_i = (uint8_t)((arp_oct_i + 1u) % octs); }
    }
    if (arp_idx >= nsrc) arp_idx = 0;
    note = (uint32_t)src[arp_idx] + 12u * arp_oct_i;
    arp_stop();
    arp_fire(note);
    arp_off = fm1_ms + iv * gate / 100u;
}

static void set_zero(void) {
    volatile uint8_t *p = (volatile uint8_t *)&set;
    uint32_t i;
    for (i = 0; i < sizeof set; i++) p[i] = 0;
}
static void pat_put(uint32_t p, uint32_t d, uint32_t s, uint32_t v) { set.pat[p][d][s] = (uint8_t)v; }
static void defaults(void) {
    static const uint8_t K0[16] = {120, 0, 0, 0, 100, 0, 0, 0, 120, 0, 0, 0, 100, 0, 0, 0};
    static const uint8_t S0[16] = {0, 0, 0, 0, 110, 0, 0, 0, 0, 0, 0, 0, 110, 0, 40, 0};
    static const uint8_t H0[16] = {80, 0, 48, 0, 80, 0, 48, 0, 80, 0, 48, 0, 80, 0, 48, 0};
    uint32_t i, m;
    set_zero();
    set.ver = SET_VER;
    set.tempo = 120;
    set.swing = 50;
    set.length = 16;
    set.machine = 2;
    set.engine = 0;
    set.song_len = 1;
    set.song[1] = 1;
    set.song[2] = 2;
    set.song[3] = 3;
    for (m = 0; m < 6u; m++) {
        set.bits[m] = (m == 3u) ? 0u : 2u;
        set.rate[m] = (m == 2u || m == 3u) ? 3u : (m == 4u ? 2u : 0u);
        set.tone[m] = 2;
    }
    set.wet = 100;
    set.mix[0] = 100; set.mix[1] = 92; set.mix[2] = 64; set.mix[3] = 78;
    set.drum[0][0] = 42; set.drum[0][1] = 100; set.drum[0][2] = 28; set.drum[0][3] = 78;
    set.drum[1][0] = 58; set.drum[1][1] = 76; set.drum[1][2] = 55; set.drum[1][3] = 18;
    set.drum[2][0] = 62; set.drum[2][1] = 42; set.drum[2][2] = 100; set.drum[2][3] = 18;
    set.drum[3][0] = 48; set.drum[3][1] = 88; set.drum[3][2] = 20; set.drum[3][3] = 64;
    set.atk = 2; set.dec = 28; set.sus = 100; set.rel = 24;
    set.arp_div = 2; set.arp_gate = 55; set.arp_oct = 1;
    set.lfo_rate = 36; set.lfo_cc = 1; set.lfo_dest = 2;
    set.env_cc = 11; set.env_amt = 100;
    set.map = 0;
    set.bank = 0;
    apply_map();
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
    static const uint8_t GM0[4] = {36, 38, 42, 45};
    static const uint8_t MN0[4] = {60, 64, 67, 72};
    if (set.oct < -2 || set.oct > 2) set.oct = 0;
    if (set.ch > 16u) set.ch = 0;
    if (set.tempo < 40u || set.tempo > 240u) set.tempo = 120;
    if (set.swing < 50u || set.swing > 75u) set.swing = 50;
    if (set.length < 1u || set.length > 16u) set.length = 16;
    if (set.pattern > 7u) set.pattern = 0;
    if (set.track > 7u) set.track = 0;
    if (set.machine > 5u) set.machine = 2;
    if (set.engine > 3u) set.engine = 0;
    if (set.song_len < 1u || set.song_len > 16u) set.song_len = 1;
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
        if (set.gm[i] > 127u) set.gm[i] = GM0[i];
        if (set.mnote[i] < 1u || set.mnote[i] > 127u) set.mnote[i] = MN0[i];
        if (set.mch[i] < 1u || set.mch[i] > 16u) set.mch[i] = 1;
    }
    for (i = 0; i < 16u; i++) if (set.song[i] > 7u) set.song[i] = 0;
    if (set.atk > 100u) set.atk = 2;
    if (set.dec > 100u) set.dec = 28;
    if (set.sus > 100u) set.sus = 100;
    if (set.rel > 100u) set.rel = 24;
    if (set.arp_mode > 6u) set.arp_mode = 0;
    if (set.arp_latch > 1u) set.arp_latch = 0;
    if (set.arp_div > 3u) set.arp_div = 2;
    if (set.arp_gate < 10u || set.arp_gate > 100u) set.arp_gate = 55;
    if (set.arp_oct < 1u || set.arp_oct > 3u) set.arp_oct = 1;
    if (set.lfo_rate > 100u) set.lfo_rate = 36;
    if (set.lfo_depth > 100u) set.lfo_depth = 0;
    if (set.lfo_cc < 1u || set.lfo_cc > 127u) set.lfo_cc = 1;
    if (set.lfo_dest > 2u) set.lfo_dest = 2;
    if (set.env_on > 1u) set.env_on = 0;
    if (set.env_cc < 1u || set.env_cc > 127u) set.env_cc = 11;
    if (set.env_amt > 100u) set.env_amt = 100;
    if (set.map >= NMAP) set.map = 0;
    if (set.bank > 1u) set.bank = 0;
    if (set.dch < 1u || set.dch > 16u) set.dch = 10;
    map_labels();
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
    if (pg == PG_SEQ || pg == PG_EDIT) { tones_release(); arp_stop(); nheld = 0; nplayed = 0; }
    page = (uint8_t)pg;
    fx_alt = 0;
    env_alt = 0;
    edit_alt = 0;
    skip_keys = 1;
    paint_req = 1;
}
static uint32_t clk_seen;
static void transport(int run, int send_midi) {
    fm1_irq_off();
    if (run) {
        if (set.song_len > 1u && set.song_len <= 16u) {
            song_i = 0;
            set.pattern = set.song[0];
        }
        seq_run = 1;
        seq_epoch++;
    } else seq_run = 0;
    clk_seen = clk_out_n;
    fm1_irq_on();
    if (send_midi) midi_out(run ? 0xFAu : 0xFCu, 0, 0);
    paint_req = 1;
}

static int black_note(uint32_t note) { return (0x54Au >> (note % 12u)) & 1; }
static void keys_init(void) {
    uint32_t k, w = 0, b = 0;
    for (k = 0; k < 27u; k++) {
        if (!black_note(53u + k) && w < 16u) { white_at[w] = (uint8_t)k; white_ord[k] = (uint8_t)w++; }
        else white_ord[k] = 255u;
        if (black_note(53u + k)) black_ord[k] = (uint8_t)b++;
        else black_ord[k] = 255u;
    }
    nwhite = (uint8_t)w;
    nblack = (uint8_t)b;
    (void)white_at;
    (void)nwhite;
    (void)nblack;
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
            int d = drum_ix(d1);
            if (ty == 0x90u && d2 && d >= 0) request_drum((uint32_t)d, d2, 1);
            continue;
        }
        if (set.ch && ch != (uint32_t)set.ch - 1u) continue;
        if (ty == 0x90u && d2) {
            take_push(d1, d2);
            menv_trig(1);
            if (set.arp_mode && page != PG_SEQ && page != PG_EDIT) note_hold(d1);
            else tone_start(d1, d2);
        } else if (ty == 0x80u || ty == 0x90u) {
            if (set.arp_mode) note_drop(d1);
            tone_stop(d1);
            menv_release();
        }
    }
    in_midi = 0;
}
static void app_idle(void) { if (idle_ok) midi_in(); }

static void turn_page(uint32_t k, int e) {
    uint32_t m, tr;
    if (!e) return;
    hot = (uint8_t)k;
    dirty = 1;
    t_change = fm1_ms;
    paint_req = 1;
    m = set.machine > 5u ? 5u : set.machine;
    tr = trk();
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
    } else if (page == PG_SEQ && seq_view == VW_SONG) {
        uint32_t sl = set.song_len < 1u ? 1u : set.song_len;
        if (sl > 16u) sl = 16u;
        if (song_slot >= sl) song_slot = 0;
        if (k == 0u) song_slot = (uint8_t)clampu((int)song_slot + e, 0, (int)sl - 1);
        else if (k == 1u) set.song[song_slot] = (uint8_t)clampu((int)set.song[song_slot] + e, 0, 7);
        else if (k == 2u) set.song_len = (uint8_t)clampu((int)set.song_len + e, 1, 16);
        else set.track = (uint8_t)clampu((int)set.track + e, 0, 7);
    } else if (page == PG_SEQ) {
        uint32_t len = set.length < 1u ? 1u : set.length;
        if (k == 0u) cursor = (uint8_t)clampu((int)cursor + e, 0, (int)len - 1);
        else if (k == 1u && tr >= 4u)
            set.pat[set.pattern][tr][cursor] = (uint8_t)clampu((int)set.pat[set.pattern][tr][cursor] + e, 0, 127);
        else if (k == 1u)
            set.pat[set.pattern][tr][cursor] = (uint8_t)clampu((int)set.pat[set.pattern][tr][cursor] + e, 0, 127);
        else if (k == 2u && seq_view == VW_PIANO && tr >= 4u)
            set.pvel[set.pattern][tr - 4u][cursor] = (uint8_t)clampu((int)set.pvel[set.pattern][tr - 4u][cursor] + e, 1, 127);
        else if (k == 2u) set.length = (uint8_t)clampu((int)set.length + e, 1, 16);
        else set.track = (uint8_t)clampu((int)set.track + e, 0, 7);
    } else if (page == PG_EDIT && edit_alt == 2u) {
        if (k == 0u) {
            set.map = (uint8_t)clampu((int)set.map + e, 0, (int)NMAP - 1);
            set.bank = 0;
            apply_map();
            message(kit_now()->msg);
        } else if (k == 1u) {
            if (!kit_now()->b) message("ONE PAGE");
            else {
                set.bank = (uint8_t)clampu((int)set.bank + e, 0, 1);
                apply_map();
                message(set.bank ? "PAGE 2" : "PAGE 1");
            }
        } else if (k == 2u) {
            if (tr < 4u) set.gm[tr] = (uint8_t)clampu((int)set.gm[tr] + e, 1, 127);
            else set.mnote[tr - 4u] = (uint8_t)clampu((int)set.mnote[tr - 4u] + e, 1, 127);
        } else {
            uint32_t i;
            set.dch = (uint8_t)clampu((int)set.dch + e, 1, 16);
            for (i = 0; i < 4u; i++) set.mch[i] = set.dch;
        }
    } else if (page == PG_EDIT && edit_alt) {
        if (tr >= 4u) set.mch[k] = (uint8_t)clampu((int)set.mch[k] + e, 1, 16);
        else set.gm[k] = (uint8_t)clampu((int)set.gm[k] + e, 0, 127);
    } else if (page == PG_EDIT && tr >= 4u) {
        uint32_t mt = tr - 4u, len = set.length < 1u ? 1u : set.length;
        if (cursor >= len) cursor = 0;
        if (k == 0u) set.mch[mt] = (uint8_t)clampu((int)set.mch[mt] + e, 1, 16);
        else if (k == 1u) set.mnote[mt] = (uint8_t)clampu((int)set.mnote[mt] + e, 1, 127);
        else if (k == 2u) set.pat[set.pattern][tr][cursor] = (uint8_t)clampu((int)set.pat[set.pattern][tr][cursor] + e, 0, 127);
        else set.pvel[set.pattern][mt][cursor] = (uint8_t)clampu((int)set.pvel[set.pattern][mt][cursor] + e, 0, 127);
    } else if (page == PG_EDIT) {
        set.drum[tr][k] = (uint8_t)clampu((int)set.drum[tr][k] + e, 0, 100);
    } else if (page == PG_ARP) {
        if (k == 0u) set.arp_mode = (uint8_t)clampu((int)set.arp_mode + e, 0, 6);
        else if (k == 1u) set.arp_div = (uint8_t)clampu((int)set.arp_div + e, 0, 3);
        else if (k == 2u) set.arp_gate = (uint8_t)clampu((int)set.arp_gate + e, 10, 100);
        else set.arp_oct = (uint8_t)clampu((int)set.arp_oct + e, 1, 3);
    } else if (page == PG_LFO) {
        if (k == 0u) set.lfo_rate = (uint8_t)clampu((int)set.lfo_rate + e, 0, 100);
        else if (k == 1u) set.lfo_depth = (uint8_t)clampu((int)set.lfo_depth + e, 0, 100);
        else if (k == 2u) set.lfo_cc = (uint8_t)clampu((int)set.lfo_cc + e, 1, 127);
        else set.lfo_dest = (uint8_t)clampu((int)set.lfo_dest + e, 0, 2);
    } else if (page == PG_ENV && env_alt) {
        if (k == 0u) set.env_on = (uint8_t)clampu((int)set.env_on + e, 0, 1);
        else if (k == 1u) set.env_cc = (uint8_t)clampu((int)set.env_cc + e, 1, 127);
        else if (k == 2u) set.env_amt = (uint8_t)clampu((int)set.env_amt + e, 0, 100);
        else message("TRIG IS A NOTE");
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
    static const uint8_t b[PG_N] = {BTN_FX, BTN_HOME, BTN_SEQ, BTN_EDIT, BTN_ARP, BTN_LFO, BTN_ENV, BTN_GLO};
    return b[pg < PG_N ? pg : 0];
}
static void track_step(int e) {
    int t = (int)set.track + e;
    while (t < 0) t += 8;
    set.track = (uint8_t)(t & 7);
    paint_req = 1;
}
static void overdub_note(uint32_t note, uint32_t vel) {
    uint32_t tr, len, s;
    if (!(rec && seq_run) || !note || note > 127u) return;
    tr = trk() >= 4u ? trk() : 4u;
    len = set.length < 1u ? 1u : set.length;
    s = seq_pos;
    if (s >= len) return;
    set.pat[set.pattern][tr][s] = (uint8_t)note;
    set.pvel[set.pattern][tr - 4u][s] = (uint8_t)(vel > 127u ? 127u : vel);
    paint_req = 1;
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
        menv_tick();
        arp_run();
        midi_in();

        if (seq_gen != seen_gen) {
            uint32_t d, gate_ms = 15000u / (set.tempo < 40u ? 40u : set.tempo);
            seen_gen = seq_gen;
            if (gate_ms < 40u) gate_ms = 40u;
            if (gate_ms > 400u) gate_ms = 400u;
            for (d = 0; d < 4u; d++) {
                if (seq_vel_out[d]) {
                    midi_out(0x90u | drum_ch(), set.gm[d], seq_vel_out[d]);
                    note_off_later(drum_ch(), set.gm[d], 60u);
                    menv_trig(0);
                }
                if (seq_mnote[d]) {
                    uint32_t ch = midi_ch(d);
                    midi_out(0x90u | ch, seq_mnote[d], seq_mvel[d]);
                    note_off_later(ch, seq_mnote[d], gate_ms);
                    menv_trig(0);
                }
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
        base = (uint32_t)(53 + 12 * (int32_t)set.oct);
        for (k = 0; k < 27u; k++) {
            uint32_t vel = 100;
#if BOARD_SMK37
            vel = fm1_in.key_vel[SMK_KEY_F3 + k];
            if (!vel) vel = 100;
#endif
            if (down >> k & 1u) {
                uint32_t note = base + k;
                int piano = page == PG_SEQ && seq_view == VW_PIANO && trk() >= 4u;
                if (note > 127u) note = 127u;
                if (piano) {
                    uint32_t tr = trk(), len = set.length < 1u ? 1u : set.length, stn = cursor, ch;
                    if (len > 16u) len = 16u;
                    if (stn >= len) stn = 0;
                    set.pat[set.pattern][tr][stn] = (uint8_t)note;
                    set.pvel[set.pattern][tr - 4u][stn] = (uint8_t)vel;
                    cursor = (uint8_t)((stn + 1u) % len);
                    ch = midi_ch(tr - 4u);
                    midi_out(0x90u | ch, note, vel);
                    note_off_later(ch, note, 140u);
                    menv_trig(0);
                    dirty = 1;
                    t_change = fm1_ms;
                    paint_req = 1;
                    message_step(stn + 1u, note);
                } else if (page == PG_SEQ && seq_view != VW_SONG && white_ord[k] != 255u && white_ord[k] < set.length) {
                    uint8_t w = white_ord[k];
                    uint32_t tr = trk();
                    uint8_t *cell = (uint8_t *)&set.pat[set.pattern][tr][w];
                    cursor = w;
                    if (tr >= 4u) {
                        if (*cell) { *cell = 0; set.pvel[set.pattern][tr - 4u][w] = 0; }
                        else {
                            uint8_t n = set.mnote[tr - 4u];
                            if (n < 1u) n = 60;
                            *cell = n;
                            set.pvel[set.pattern][tr - 4u][w] = 100;
                        }
                    } else {
                        *cell = *cell ? 0 : 100;
                        if (*cell) request_drum(tr, 100, 0);
                    }
                    dirty = 1;
                    t_change = fm1_ms;
                    paint_req = 1;
                } else if (page == PG_EDIT && trk() < 4u) {
                    request_drum(trk(), vel, 1);
                } else if (black_ord[k] < 4u) {
                    request_drum(black_ord[k], vel, 1);
                } else {
                    down_note[k] = (uint8_t)note;
                    down_count++;
                    take_push(note, vel);
                    menv_trig(1);
                    overdub_note(note, vel);
                    if (set.arp_mode && page != PG_SEQ && page != PG_EDIT) note_hold((uint8_t)note);
                    else { tone_start(note, vel); midi_out(0x90u | out_ch(), note, vel); }
                }
            }
            if ((up >> k & 1u) && down_note[k]) {
                if (down_count) down_count--;
                if (set.arp_mode) note_drop(down_note[k]);
                tone_stop(down_note[k]);
                midi_out(0x80u | out_ch(), down_note[k], 0);
                down_note[k] = 0;
                menv_release();
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
                    take_push(note, vel);
                    menv_trig(1);
                    if (set.arp_mode) note_hold((uint8_t)note);
                    else { tone_start(note, vel); midi_out(0x90u | out_ch(), note, vel); }
                }
            } else if (ext_note[key]) {
                if (set.arp_mode) note_drop(ext_note[key]);
                tone_stop(ext_note[key]);
                midi_out(0x80u | out_ch(), ext_note[key], 0);
                ext_note[key] = 0;
                menv_release();
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
        if (pg == PG_FX && page == PG_FX) { fx_alt ^= 1u; paint_req = 1; }
        else if (pg == PG_ENV && page == PG_ENV) { env_alt ^= 1u; paint_req = 1; }
        else if (pg == PG_EDIT && page == PG_EDIT) {
            edit_alt = (uint8_t)((edit_alt + 1u) % 3u);
            paint_req = 1;
        }
        else if (pg == PG_SEQ && page == PG_SEQ) {
            seq_view = (uint8_t)((seq_view + 1u) % 3u);
            paint_req = 1;
        } else if (pg == PG_HOME && page == PG_HOME) {
            set.engine = (uint8_t)((engi() + 1u) & 3u);
            dirty = 1;
            t_change = fm1_ms;
            paint_req = 1;
            message(ENGMSG[set.engine]);
        } else if (pg == PG_ARP && page == PG_ARP) {
            set.arp_latch ^= 1u;
            if (!set.arp_latch) { nheld = 0; nplayed = 0; arp_stop(); }
            dirty = 1;
            t_change = fm1_ms;
            paint_req = 1;
            message(set.arp_latch ? "LATCH ON" : "LATCH OFF");
        } else if (pg < PG_N) go(pg);
        if (b & (1u << BTN_SEL)) track_step(1);
        if (b & (1u << BTN_PLAY)) transport(!seq_run, 1);
        if (b & (1u << BTN_REC)) {
            if (!seq_run) { take_commit(); paint_req = 1; }
            else { rec ^= 1u; paint_req = 1; }
        }
        if (b & (1u << BTN_SAVE)) save_req = 1;
        if (b & (1u << BTN_OCT_DOWN) && set.oct > -2) { set.oct--; dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_OCT_UP)) {
            if (page == PG_GLO) {
                if (clear_arm && (int32_t)(fm1_ms - clear_arm) < 0) {
                    uint32_t d, s;
                    for (d = 0; d < 8u; d++) for (s = 0; s < 16u; s++) set.pat[set.pattern][d][s] = 0;
                    for (d = 0; d < 4u; d++) for (s = 0; s < 16u; s++) set.pvel[set.pattern][d][s] = 0;
                    dirty = 1; t_change = fm1_ms; paint_req = 1; clear_arm = 0;
                    message("CLEARED");
                } else { clear_arm = fm1_ms + 1600u; message("PRESS OCT+ TO CLEAR"); }
            } else if (set.oct < 2) { set.oct++; dirty = 1; t_change = fm1_ms; }
        }
        if ((e = fm1_enc_take(ENC_KNOB1)) != 0) turn_page(0, e);
        if ((e = fm1_enc_take(ENC_KNOB2)) != 0) turn_page(1, e);
        if ((e = fm1_enc_take(ENC_KNOB3)) != 0) turn_page(2, e);
        if ((e = fm1_enc_take(ENC_KNOB4)) != 0) turn_page(3, e);
        if ((e = fm1_enc_take(ENC_SELECT)) != 0) track_step(e);
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
            if (page == PG_SEQ && seq_view != VW_SONG) {
                uint8_t w = white_ord[k];
                if (w != 255u && w < set.length) {
                    uint32_t tr = trk();
                    uint32_t onv = tr >= 4u ? set.pat[set.pattern][tr][w] : set.pat[set.pattern][tr][w];
                    if (onv) glow = 1;
                    if (seq_run && w == seq_pos) { on = 1; glow = 0; }
                }
            } else if (!(page == PG_SEQ && seq_view == VW_PIANO) && black_ord[k] < 4u) glow = 1;
            fm1_led_key(KEY_LED(k), on || (down_note[k] && voice[0].stage));
            fm1_led_key_glow(KEY_LED(k), glow && !on);
        }
        {
            uint32_t v;
            for (k = 0; k < 27u && !(page == PG_SEQ && seq_view != VW_SONG); k++) {
                int lit = 0;
                for (v = 0; v < NV; v++)
                    if (voice[v].stage && voice[v].stage != 4u && voice[v].note == base + k) lit = 1;
                if (lit) fm1_led_key(KEY_LED(k), 1);
            }
        }
#if BOARD_SMK37
        for (k = 0; k < 16u; k++) padled_set(k, 8, 4, 0);
        padled_flush();
#endif

        if (!msg_on() && msg) { msg = 0; hint_req = 1; }
        st = (uint32_t)seq_pos | ((uint32_t)seq_run << 5) | ((uint32_t)set.tempo << 8) |
             ((uint32_t)set.pattern << 16) | ((uint32_t)trk() << 20) | ((uint32_t)page << 24) |
             ((uint32_t)seq_view << 28);
        if (paint_req || st != stamp) {
            int full = paint_req || (st >> 24) != (stamp >> 24);
            if ((st & 0xFF00FFu) != (stamp & 0xFF00FFu)) full = 1;
            paint(full || paint_req);
            paint_req = 0;
            stamp = st;
        } else if (hint_req) paint(0);

        if (save_req && seq_run) { message("STOP TO SAVE"); save_req = 0; }
        else if ((save_req || dirty) && !seq_run && !sounding && (save_req || fm1_ms - t_change > 2000u)) {
            do_save();
            save_req = 0;
        }
        midi_flush();
    }
}
