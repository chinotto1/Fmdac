/* SPDX-License-Identifier: GPL-3.0-only */
/* Fmdac 0.7. Four kit tracks, a sequencer, and the converter.
 *
 *   FX     machine, bits, rate, tone. Press FX again: wet, drive, bits, rate.
 *          The law is on the headphones. While a computer streams USB audio,
 *          that stream is mixed in and converted too. USB audio also records
 *          the headphone mix.
 *   HOME   the level of each kit track, T1 to T4. Press HOME again to
 *          change that track's drum character: 808, 909, CR78, SDS.
 *          These are synthesized, not sample ROMs.
 *   A track is a whole kit, eight lanes, like a Skiff drum track.
 *   SELECT picks the kit. All four kits play together.
 *   GRID   SEQ. White keys are the 16 steps of the lane. A black key
 *          picks a lane and plays it. K4 is the lane. Off the grid,
 *          the keys play the kit: C kick, D snare, F# closed hat.
 *          Press SEQ again for SONG.
 *   SONG   16 slots. K1 picks the slot, K2 the pattern, K3 the length.
 *          PLAY walks the chain when the length is more than one.
 *   EDIT   tune, decay, snap, drop of the lane. Press EDIT again: the
 *          notes of four lanes. Press EDIT again: this kit's map.
 *          K1 keeps GM, TR-909, TR-707, TR-727, TR-08, Volca Beats, and
 *          the grouped Hammerhead list, then the AUv3 app maps from the
 *          OCTACHRON list. The app notes are the public chart that
 *          matches the lists its author posted. A blank lane sends
 *          nothing. OneShot and Tekno have no fixed published chart.
 *          This is not every preset stored inside the OCTACHRON app.
 *   ARP    up, down, up-down, random, chord, and as-played. Press ARP again
 *          to latch the held keys. The keys play the sine only on this page.
 *   LFO    MIDI CC, and level or tune on this mix when DEST says so.
 *   ENV    attack, decay, sustain, release of the key tone. Press ENV again:
 *          a MIDI CC envelope on each new note.
 *   GLO    tempo, swing, length, MIDI channel. OCT+ twice clears this kit.
 *   REC while stopped quantizes the last few seconds of this kit into the
 *   pattern and sets the tempo. REC while playing overdubs the playhead.
 *   PLAY then plays that quantized pattern. It is not a free looper.
 *   Tracks: T1 T2 T3 T4. Each is a whole kit: eight lanes, its own map,
 *   MIDI channel, level, and drum character. SELECT and SEL change the kit.
 *   All four play together. The sounding drums are four voices, one per
 *   kind of hit, shared by the kits. A second kick retriggers the first.
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

#define SET_VER 6u
#define NKIT 4u
#define NLANE 8u
#define NPAT 4u
enum { PG_FX, PG_HOME, PG_SEQ, PG_EDIT, PG_ARP, PG_LFO, PG_ENV, PG_GLO, PG_N };
enum { VW_GRID, VW_SONG };

typedef struct {
    uint8_t map, bank, ch, engine, level;
    uint8_t note[8];
    uint8_t lvl[8];
    uint8_t model[8];               /* 0 kick, 1 snare, 2 hat, 3 tom */
    uint8_t parm[8][4];             /* tune, decay, snap, drop */
} Trk;
typedef struct {
    uint8_t ver;
    int8_t oct;
    uint8_t ch, tempo, swing, length, pattern, track, machine, lane;
    uint8_t bits[6], rate[6], tone[6];
    uint8_t wet, drive;
    uint8_t atk, dec, sus, rel;
    uint8_t arp_mode, arp_div, arp_gate, arp_oct, arp_latch;
    uint8_t lfo_rate, lfo_depth, lfo_cc, lfo_dest;
    uint8_t env_on, env_cc, env_amt;
    uint8_t song[16];
    uint8_t song_len;
    Trk kit[NKIT];
    uint8_t vel[NPAT][NKIT][NLANE][16];
} Set;
typedef char set_fits[(sizeof(Set) <= 4000u) ? 1 : -1];

static volatile Set set;
static uint8_t page, fx_alt, env_alt, edit_alt, seq_view, rec, hot, save_req, song_slot;
static volatile uint8_t seq_run, audio_ready, sounding, ext_on, fx_bypass, song_i;
static uint8_t idle_ok, in_midi, down_count;
static uint32_t t_change, dirty;
static volatile uint32_t seq_epoch, seq_gen, ext_count, clk_out_n, wet_q8;
static volatile uint8_t seq_pos, seq_nev;
static volatile int32_t lfo_tune, lfo_lvl = 256;
typedef struct { uint8_t note, vel, ch; } SeqEv;
static volatile SeqEv seq_ev[16];
/* Main writes a slot, then pend_w. The audio ISR writes pend_r and is the only caller of drum_hit. */
typedef struct { uint8_t model, vel, eng, gain, tune, snap, drop, dec; } Pend;
#define NPEND 16u
static volatile Pend pend[NPEND];
static volatile uint8_t pend_w, pend_r;
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
static const char *const KITN[4] = {"T1", "T2", "T3", "T4"};
static const char *lname[NKIT][NLANE];
/* C kick, C# rim, D snare, D# clap, E tom, F open hat, F# closed hat, G crash. */
static const uint8_t KEY_LANE[12] = {0, 6, 1, 4, 3, 5, 2, 7, 0, 1, 4, 3};
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
 * Hammerhead: the eight notes Markus Teufel posted for OCTACHRON,
 *   grouped here as kick, snare, hat, then the other four. HAMR2 later
 *   in this list is the same eight notes in his row order.
 *
 * Everything after HAMR is an AUv3 app map from the public list that
 * matches the OCTACHRON app screen and the note rows its author posted
 * (Hammerhead, Koala 2x8, Koala 4x4, Splat-to-Clat, Korg Gadget
 * Amsterdam). Screen names are short. The hint names the app. GM8 and
 * GM16 use channel 10. The other app maps use channel 1. A 0 is an
 * empty lane: it does not play and it does not send. OneShot and Tekno
 * are not here; those apps do not publish a fixed note chart. Indices
 * 0 through 6 stay the charts above, so a saved kit keeps its map. */
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
/* Shared pages. FAC, Bilbao, and Recife are pads 36 through 51.
 * London is the first eight of that run. Digistix and NS2 Slate are
 * 48 through 63, with different lane names. */
static const uint8_t N36A[8] = {36, 37, 38, 39, 40, 41, 42, 43};
static const uint8_t N36B[8] = {44, 45, 46, 47, 48, 49, 50, 51};
static const uint8_t N48A[8] = {48, 49, 50, 51, 52, 53, 54, 55};
static const uint8_t N48B[8] = {56, 57, 58, 59, 60, 61, 62, 63};
#define LNONE "----"
static const char *const PAD_A[8] = {"1", "2", "3", "4", "5", "6", "7", "8"};
static const char *const PAD_B[8] = {"9", "10", "11", "12", "13", "14", "15", "16"};
static const uint8_t MAP_AR_A[8] = {36, 37, 38, 39, 40, 41, 42, 43};
static const uint8_t MAP_AR_B[8] = {44, 45, 46, 0, 0, 0, 0, 0};
static const char *const LAB_AR_A[8] = {"KICK", "SNARE", "CLHH", "OPHH", "RIM", "CLAP", "LTOM", "MTOM"};
static const char *const LAB_AR_B[8] = {"HTOM", "RIDE", "CRASH", LNONE, LNONE, LNONE, LNONE, LNONE};
static const uint8_t MAP_AX_A[8] = {36, 37, 38, 39, 40, 41, 42, 0};
static const char *const LAB_AX_A[8] = {"1", "2", "3", "4", "5", "6", "7", LNONE};
/* Blip Box and General MIDI 16 use the same sixteen notes and names. */
static const uint8_t MAP_BL_A[8] = {35, 36, 37, 38, 39, 40, 41, 42};
static const uint8_t MAP_BL_B[8] = {43, 44, 45, 46, 47, 48, 49, 50};
static const char *const LAB_BL_A[8] = {"KICK2", "KICK", "RIM", "SNARE", "CLAP", "SNR2", "LT2", "CLHH"};
static const char *const LAB_BL_B[8] = {"LTOM", "PDHH", "MT2", "OPHH", "MTOM", "HT2", "CRASH", "HTOM"};
static const uint8_t MAP_DA_B[8] = {44, 45, 46, 47, 0, 0, 0, 0};
static const char *const LAB_DA_B[8] = {"9", "10", "11", "12", LNONE, LNONE, LNONE, LNONE};
static const char *const LAB_DI_A[8] = {"KICK", "SNARE", "CLHH", "OPHH", "KICK2", "CLAP", "RIM", "STICK"};
static const char *const LAB_DI_B[8] = {"HTOM", "MTOM", "LTOM", "BUZZ", "CRASH", "SHKR", "RIDE", "TAMB"};
static const uint8_t MAP_DM_A[8] = {36, 38, 42, 46, 39, 37, 56, 43};
static const uint8_t MAP_DM_B[8] = {49, 0, 0, 0, 0, 0, 0, 0};
static const char *const LAB_DM_A[8] = {"KICK", "SNARE", "CLHH", "OPHH", "CLAP", "RIM", "COWB", "TOM"};
static const char *const LAB_DM_B[8] = {"CYM", LNONE, LNONE, LNONE, LNONE, LNONE, LNONE, LNONE};
static const uint8_t MAP_DC_A[8] = {44, 32, 83, 24, 36, 47, 65, 55};
static const uint8_t MAP_606_A[8] = {36, 38, 43, 47, 50, 37, 39, 0};
static const char *const LAB_606_A[8] = {"BASS", "SNARE", "LTOM", "HTOM", "CYM", "OPHH", "CLHH", LNONE};
static const uint8_t MAP_808_A[8] = {36, 38, 43, 47, 50, 37, 39, 56};
static const uint8_t MAP_808_B[8] = {49, 46, 42, 0, 0, 0, 0, 0};
static const char *const LAB_808_A[8] = {"BD", "SD", "LT", "MT", "HT", "RS", "CP", "CB"};
static const char *const LAB_808_B[8] = {"CY", "OH", "CH", LNONE, LNONE, LNONE, LNONE, LNONE};
static const uint8_t MAP_E909_A[8] = {36, 38, 43, 50, 39, 56, 49, 42};
static const char *const LAB_E909_A[8] = {"BASS", "SNARE", "LTOM", "HTOM", "CLAP", "OPHH", "CLHH", "RIDE"};
static const uint8_t MAP_G8_A[8] = {36, 38, 39, 45, 47, 48, 42, 46};
static const char *const LAB_G8_A[8] = {"KICK", "SNARE", "CLAP", "LTOM", "MTOM", "HTOM", "CLHH", "OPHH"};
static const uint8_t MAP_H2_A[8] = {36, 38, 39, 45, 48, 50, 42, 46};
static const char *const LAB_H2_A[8] = {"KICK", "SNARE", "CLAP", "RIM", "SNR2", "COWB", "CLHH", "OPHH"};
static const uint8_t MAP_K28_A[8] = {44, 45, 46, 47, 48, 49, 50, 51};
static const uint8_t MAP_K28_B[8] = {36, 37, 38, 39, 40, 41, 42, 43};
static const uint8_t MAP_K44_A[8] = {48, 49, 50, 51, 44, 45, 46, 47};
static const uint8_t MAP_K44_B[8] = {40, 41, 42, 43, 36, 37, 38, 39};
static const uint8_t MAP_AMS_A[8] = {36, 37, 38, 39, 0, 0, 0, 0};
static const char *const LAB_AMS_A[8] = {"1", "2", "3", "4", LNONE, LNONE, LNONE, LNONE};
static const uint8_t MAP_GL_A[8] = {36, 37, 38, 39, 40, 41, 42, 43};
static const uint8_t MAP_GL_B[8] = {44, 45, 0, 0, 0, 0, 0, 0};
static const char *const LAB_GL_A[8] = {"KICK", "SNARE", "SNR2", "HHCL", "HHOP", "HTOM", "MTOM", "LTOM"};
static const char *const LAB_GL_B[8] = {"CRASH", "RIDE", LNONE, LNONE, LNONE, LNONE, LNONE, LNONE};
static const uint8_t MAP_TK_A[8] = {36, 37, 38, 39, 0, 0, 0, 0};
static const char *const LAB_TK_A[8] = {"BD201", "SD302", "TOM404", "PCS503", LNONE, LNONE, LNONE, LNONE};
static const uint8_t MAP_MV_A[8] = {60, 61, 62, 63, 64, 65, 66, 67};
static const uint8_t MAP_MV_B[8] = {68, 69, 70, 71, 72, 73, 74, 75};
static const char *const LAB_MV_A[8] = {"BD", "SN", "CLP", "LT", "MT", "HT", "RS", "CB"};
static const char *const LAB_MV_B[8] = {"CYM", "OH", "CH", "LC", "MC", "HC", "CLV", "MRC"};
static const char *const LAB_NS_A[8] = {"BD", "SD", "HHCL", "HHOP", "BD2", "CP2", "HHCL2", "HHOP2"};
static const char *const LAB_NS_B[8] = {"CRASH", "CRS2", "RIDE", "RID2", "TOM1", "TOM2", "TOM3", "TOM4"};
static const uint8_t MAP_AC_A[8] = {36, 35, 38, 40, 41, 43, 45, 47};
static const uint8_t MAP_AC_B[8] = {37, 39, 56, 44, 42, 46, 49, 51};
static const char *const LAB_AC_A[8] = {"BD", "BD2", "SD", "SD2", "LT", "HT", "LC", "HC"};
static const char *const LAB_AC_B[8] = {"RIM", "CLP", "CB", "MA", "HH", "OH", "OR", "RIDE"};
static const uint8_t MAP_RU_A[8] = {49, 51, 54, 56, 58, 61, 63, 66};
static const char *const LAB_RU_A[8] = {"KICK", "SNARE", "CLAP", "LTOM", "MTOM", "HTOM", "CLHH", "OPHH"};
static const uint8_t MAP_RF_A[8] = {49, 51, 54, 56, 58, 61, 0, 0};
static const char *const LAB_RF_A[8] = {"KICK", "SNARE", "FX1", "FX2", "FX3", "HAT", LNONE, LNONE};
static const uint8_t MAP_SX_A[8] = {60, 61, 62, 65, 64, 63, 66, 67};
static const uint8_t MAP_SX_B[8] = {68, 0, 0, 0, 0, 0, 0, 0};
static const char *const LAB_SX_A[8] = {"KICK", "SNARE", "CLHH", "OPHH", "LTOM", "HTOM", "CLAP", "TAMB"};
static const char *const LAB_SX_B[8] = {"NOISE", LNONE, LNONE, LNONE, LNONE, LNONE, LNONE, LNONE};
static const char *const LAB_SI_A[8] = {"KICK", "SNARE", "CLHH", "OPHH", "CYM", "LTOM", "MTOM", "HTOM"};
static const char *const LAB_SI_B[8] = {"LCONG", "MCONG", "HCONG", "CLAP", "CLAV", "MAR", "COWB", "RIM"};
static const uint8_t MAP_SP_A[8] = {60, 61, 62, 63, 64, 65, 66, 67};
static const uint8_t MAP_SP_B[8] = {68, 69, 70, 71, 72, 73, 74, 75};
static const char *const LAB_SP_A[8] = {"C3", "C#3", "D3", "D#3", "E3", "F3", "F#3", "G3"};
static const char *const LAB_SP_B[8] = {"G#3", "A3", "A#3", "B3", "C4", "C#4", "D4", "D#4"};
static const uint8_t MAP_VS_A[8] = {36, 38, 39, 41, 43, 44, 42, 46};
static const uint8_t MAP_VS_B[8] = {49, 50, 37, 40, 51, 45, 47, 48};
static const char *const LAB_VS_A[8] = {"KICK", "SNARE", "CLAP", "LTOM", "MTOM", "HTOM", "CLHH", "OPHH"};
static const char *const LAB_VS_B[8] = {"RIDE", "COWB", "RIM", "KICK2", "SNR2", "KICK3", "KICK4", "KICK5"};
static const uint8_t MAP_V8_A[8] = {36, 37, 38, 39, 40, 41, 42, 43};
static const uint8_t MAP_V8_B[8] = {44, 45, 46, 47, 48, 49, 50, 51};
static const char *const LAB_V8_A[8] = {"KICK", "RIM", "SNARE", "CLAP", "CLHH", "OPHH", "LTOM", "MTOM"};
static const char *const LAB_V8_B[8] = {"HTOM", "CYM", "COWB", "LCONG", "MCONG", "HCONG", "MAR", "CLAV"};
static const uint8_t MAP_V9_A[8] = {52, 53, 54, 55, 56, 57, 58, 59};
static const uint8_t MAP_V9_B[8] = {60, 61, 62, 0, 0, 0, 0, 0};
static const char *const LAB_V9_A[8] = {"KICK2", "RIM2", "SNR2", "CLP2", "CLHH2", "OPHH2", "LTOM2", "MTOM2"};
static const char *const LAB_V9_B[8] = {"HTOM2", "CRASH", "RIDE", LNONE, LNONE, LNONE, LNONE, LNONE};
static const uint8_t MAP_VT_A[8] = {24, 27, 30, 33, 36, 39, 42, 45};
typedef struct {
    const char *name, *msg, *hint;
    uint8_t ch;
    const uint8_t *a;
    const uint8_t *b;
    const char *const *la;
    const char *const *lb;
} MapDef;
static const MapDef MAPS[] = {
    {"GM", "MAP GM", " GM DRUM NOTES", 10, MAP_GM_A, MAP_GM_B, LAB_GM_A, LAB_GM_B},
    {"909", "MAP 909", " TR-909 NOTES", 10, MAP_909_A, MAP_909_B, LAB_909_A, LAB_909_B},
    {"707", "MAP 707", " TR-707 NOTES 35-51", 10, MAP_909_A, MAP_909_B, LAB_909_A, LAB_909_B},
    {"727", "MAP 727", " TR-727 NOTES 35-51", 10, MAP_909_A, MAP_909_B, LAB_909_A, LAB_909_B},
    {"TR08", "MAP TR08", " TR-08 NOTES", 1, MAP_08_A, MAP_08_B, LAB_08_A, LAB_08_B},
    {"VOLCA", "MAP VOLCA", " VOLCA LEVEL CC 40-49", 1, MAP_VB_A, MAP_VB_B, LAB_VB_A, LAB_VB_B},
    {"HAMR", "MAP HAMR", " HAMMERHEAD NOTES", 1, MAP_HH, 0, LAB_HH, 0},
    {"AR909", "MAP AR909", " AR-909 APP", 1, MAP_AR_A, MAP_AR_B, LAB_AR_A, LAB_AR_B},
    {"AXON2", "MAP AXON2", " AXON 2 PADS", 1, MAP_AX_A, 0, LAB_AX_A, 0},
    {"BLIP", "MAP BLIP", " BLIP BOX APP", 1, MAP_BL_A, MAP_BL_B, LAB_BL_A, LAB_BL_B},
    {"DADA", "MAP DADA", " DADAMACHINES PADS", 1, N36A, MAP_DA_B, PAD_A, LAB_DA_B},
    {"DIGI", "MAP DIGI", " DIGISTIX APP", 1, N48A, N48B, LAB_DI_A, LAB_DI_B},
    {"DM1", "MAP DM1", " DM1 DRUMLAB APP", 1, MAP_DM_A, MAP_DM_B, LAB_DM_A, LAB_DM_B},
    {"DRUMC", "MAP DRUMC", " DRUMCOMPUTER APP", 1, MAP_DC_A, 0, PAD_A, 0},
    {"E606", "MAP E606", " EGDR 606 APP", 1, MAP_606_A, 0, LAB_606_A, 0},
    {"E808", "MAP E808", " EGDR 808 APP", 1, MAP_808_A, MAP_808_B, LAB_808_A, LAB_808_B},
    {"E909", "MAP E909", " EGDR 909 APP", 1, MAP_E909_A, 0, LAB_E909_A, 0},
    {"FAC", "MAP FAC", " FAC DRUMKIT PADS", 1, N36A, N36B, PAD_A, PAD_B},
    {"GM8", "MAP GM8", " GENERAL MIDI 8", 10, MAP_G8_A, 0, LAB_G8_A, 0},
    {"GM16", "MAP GM16", " GENERAL MIDI 16", 10, MAP_BL_A, MAP_BL_B, LAB_BL_A, LAB_BL_B},
    {"HAMR2", "MAP HAMR2", " HAMMERHEAD APP ORDER", 1, MAP_H2_A, 0, LAB_H2_A, 0},
    {"KO2X8", "MAP KO2X8", " KOALA SAMPLER 2X8", 1, MAP_K28_A, MAP_K28_B, PAD_A, PAD_B},
    {"KO4X4", "MAP KO4X4", " KOALA SAMPLER 4X4", 1, MAP_K44_A, MAP_K44_B, PAD_A, PAD_B},
    {"AMS", "MAP AMS", " KORG GADGET AMSTERDAM", 1, MAP_AMS_A, 0, LAB_AMS_A, 0},
    {"BILBAO", "MAP BILBAO", " KORG GADGET BILBAO", 1, N36A, N36B, PAD_A, PAD_B},
    {"GLAD", "MAP GLAD", " KORG GADGET GLADSTONE", 1, MAP_GL_A, MAP_GL_B, LAB_GL_A, LAB_GL_B},
    {"LONDON", "MAP LONDON", " KORG GADGET LONDON", 1, N36A, 0, PAD_A, 0},
    {"RECIFE", "MAP RECIFE", " KORG GADGET RECIFE", 1, N36A, N36B, PAD_A, PAD_B},
    {"TOKYO", "MAP TOKYO", " KORG GADGET TOKYO", 1, MAP_TK_A, 0, LAB_TK_A, 0},
    {"MV08", "MAP MV08", " MV08 APP", 1, MAP_MV_A, MAP_MV_B, LAB_MV_A, LAB_MV_B},
    {"NS2", "MAP NS2", " NS2 SLATE APP", 1, N48A, N48B, LAB_NS_A, LAB_NS_B},
    {"ACID", "MAP ACID", " PURE ACID DRUMS", 1, MAP_AC_A, MAP_AC_B, LAB_AC_A, LAB_AC_B},
    {"RUIS", "MAP RUIS", " RUISMAKER APP", 1, MAP_RU_A, 0, LAB_RU_A, 0},
    {"RUFM", "MAP RUFM", " RUISMAKER FM APP", 1, MAP_RF_A, 0, LAB_RF_A, 0},
    {"SDSX", "MAP SDSX", " SDS-X APP", 1, MAP_SX_A, MAP_SX_B, LAB_SX_A, LAB_SX_B},
    {"SITALA", "MAP SITALA", " SITALA CHROMATIC", 1, N36A, N36B, LAB_SI_A, LAB_SI_B},
    {"SPLAT", "MAP SPLAT", " SPLAT-TO-CLAT C3 TO D#4", 1, MAP_SP_A, MAP_SP_B, LAB_SP_A, LAB_SP_B},
    {"VASM", "MAP VASM", " VA DRUM SM APP", 1, MAP_VS_A, MAP_VS_B, LAB_VS_A, LAB_VS_B},
    {"VA808", "MAP VA808", " VA DRUM 2 808", 1, MAP_V8_A, MAP_V8_B, LAB_V8_A, LAB_V8_B},
    {"VA909", "MAP VA909", " VA DRUM 2 909", 1, MAP_V9_A, MAP_V9_B, LAB_V9_A, LAB_V9_B},
    {"VATA", "MAP VATA", " VATANATOR PADS", 1, MAP_VT_A, 0, PAD_A, 0},
};
#undef LNONE
#define NMAP ((uint32_t)(sizeof MAPS / sizeof MAPS[0]))
static int prefix(const char *s, const char *p) {
    if (!s) return 0;
    while (*p) {
        if (*s++ != *p++) return 0;
    }
    return 1;
}
static int contains(const char *s, const char *p) {
    if (!s || !p || !*p) return 0;
    for (; *s; s++) if (prefix(s, p)) return 1;
    return 0;
}
/* KICK, BD, and BASS are the kick. Hats, open and closed hats, cymbals,
 * crashes, and rides share the hat. A name containing HH is a hat, so
 * CLHH and HHCL are hats. Toms, including HT2, and the congas share the
 * tom. HT2 starts with HT. HAT starts with HA, so it stays a hat. HTOM
 * does not contain HH. A ---- lane is unused. Everything else is the snare. */
static uint8_t model_of(const char *s) {
    if (!s || s[0] == '-') return 1;
    if (prefix(s, "KICK") || prefix(s, "BD") || prefix(s, "BASS")) return 0;
    if (prefix(s, "HAT") || prefix(s, "OH") || prefix(s, "CH")
        || prefix(s, "CY") || prefix(s, "CR") || prefix(s, "RID")
        || prefix(s, "HIH") || contains(s, "HH")) return 2;
    if (prefix(s, "TOM") || prefix(s, "LT") || prefix(s, "MT") || prefix(s, "HT")
        || prefix(s, "LC") || prefix(s, "MC") || prefix(s, "HC")
        || contains(s, "CONG")) return 3;
    return 1;
}
static const MapDef *map_of(uint32_t ix) { return &MAPS[ix < NMAP ? ix : 0u]; }
static uint32_t trk(void) { return set.track < NKIT ? set.track : 0u; }
static uint32_t lane_now(void) { return set.lane < NLANE ? set.lane : 0u; }
static uint32_t pat_now(void) { return set.pattern < NPAT ? set.pattern : 0u; }
static const char *trk_name(uint32_t tr) { return KITN[tr < NKIT ? tr : 0u]; }
static const char *lane_name(uint32_t ki, uint32_t lane) {
    const char *s;
    if (ki >= NKIT) ki = 0;
    if (lane >= NLANE) lane = 0;
    s = lname[ki][lane];
    return s ? s : "LANE";
}
static void map_labels(uint32_t ki) {
    const MapDef *k;
    const char *const *labs;
    uint32_t i;
    if (ki >= NKIT) return;
    k = map_of(set.kit[ki].map);
    if (set.kit[ki].bank && !k->b) set.kit[ki].bank = 0;
    labs = set.kit[ki].bank ? k->lb : k->la;
    for (i = 0; i < NLANE; i++) lname[ki][i] = labs[i];
}
static void apply_map(uint32_t ki) {
    const MapDef *k;
    const uint8_t *notes;
    const char *const *labs;
    uint32_t i;
    if (ki >= NKIT) return;
    k = map_of(set.kit[ki].map);
    if (set.kit[ki].bank && !k->b) set.kit[ki].bank = 0;
    notes = set.kit[ki].bank ? k->b : k->a;
    labs = set.kit[ki].bank ? k->lb : k->la;
    set.kit[ki].ch = k->ch;
    for (i = 0; i < NLANE; i++) {
        lname[ki][i] = labs[i];
        set.kit[ki].note[i] = notes[i];
        set.kit[ki].model[i] = model_of(labs[i]);
    }
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
    uint8_t alive, model, engc, sh, gain, tune, drop;
} Drum;
static Drum drumv[4];

static uint32_t kit_gain(uint32_t ki, uint32_t lane) {
    uint32_t lv, ll;
    if (ki >= NKIT || lane >= NLANE) return 0;
    lv = set.kit[ki].level;
    ll = set.kit[ki].lvl[lane];
    if (lv > 100u) lv = 100u;
    if (ll > 100u) ll = 100u;
    return lv * ll * 128u / 10000u;
}

/* Called from the audio ISR only. model is the voice: two kits hitting kick share it. */
static void drum_hit(uint32_t model, uint32_t vel, uint32_t eng, uint32_t gain,
                     uint32_t tune, uint32_t snap, uint32_t drop, uint32_t dec) {
    Drum *v;
    uint32_t i, f, mul, base, end, extra;
    int decs;
    if (model > 3u || !vel || !gain) return;
    if (vel > 127u) vel = 127u;
    if (eng > 3u) eng = 0;
    if (gain > 255u) gain = 255u;
    if (tune > 100u) tune = 100u;
    if (snap > 100u) snap = 100u;
    if (drop > 100u) drop = 100u;
    if (dec > 100u) dec = 100u;
    v = &drumv[model];
    decs = (int)dec + (int)ENG_DEC[eng];
    if (decs < 0) decs = 0;
    if (decs > 100) decs = 100;
    v->model = (uint8_t)model;
    v->engc = (uint8_t)eng;
    v->sh = (uint8_t)(6u + (uint32_t)decs * 8u / 100u);
    v->gain = (uint8_t)gain;
    v->tune = (uint8_t)tune;
    v->drop = (uint8_t)drop;
    i = tune / 10u;
    f = tune - i * 10u;
    if (i > 10u) i = 10u;
    mul = TUNE_Q12[i];
    if (i < 10u && f) mul += (uint32_t)(TUNE_Q12[i + 1u] - mul) * f / 10u;
    v->lp = 0;
    if (model == 2u) {
        v->amp = 0;
        v->namp = (int32_t)vel * 140;
        if (v->namp > 20000) v->namp = 20000;
        v->click = 0;
    } else {
        base = (uint32_t)(((uint64_t)DRUM_F[model] * mul) >> 12);
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
        if (model == 1u) {
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

static uint8_t step_i;
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
    uint8_t p = set.pattern, len = set.length;
    uint32_t ki, lane, n = 0;
    if (p >= NPAT) p = 0;
    if (len < 1u) len = 1u;
    if (len > 16u) len = 16u;
    if (step_i >= len) step_i = 0;
    for (ki = 0; ki < NKIT; ki++) {
        uint32_t eng = set.kit[ki].engine, ch = set.kit[ki].ch;
        if (eng > 3u) eng = 0;
        if (ch < 1u || ch > 16u) ch = 10u + ki;
        for (lane = 0; lane < NLANE; lane++) {
            uint8_t v = *(volatile uint8_t *)&set.vel[p][ki][lane][step_i];
            uint32_t note, g, model;
            if (!v) continue;
            if (v > 127u) v = 127u;
            note = set.kit[ki].note[lane];
            if (!note) continue;
            if (note > 127u) note = 36u;
            if (n < 16u) {
                seq_ev[n].note = (uint8_t)note;
                seq_ev[n].vel = v;
                seq_ev[n].ch = (uint8_t)(ch - 1u);
                n++;
            }
            model = set.kit[ki].model[lane];
            if (model > 3u) model = 1u;
            g = kit_gain(ki, lane);
            if (g) drum_hit(model, v, eng, g, set.kit[ki].parm[lane][0], set.kit[ki].parm[lane][2],
                           set.kit[ki].parm[lane][3], set.kit[ki].parm[lane][1]);
        }
    }
    seq_nev = (uint8_t)n;
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
            {
                uint8_t np = set.song[si];
                set.pattern = np < NPAT ? np : 0;
            }
        }
    }
    render_step();
}

static void app_audio(int32_t *out, uint32_t n) {
    uint32_t i, d, tempo, swing, len;
    int32_t lt, lvl, wq;
    uint32_t any = 0;
    int ext;
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
    while (pend_r != pend_w) {
        uint8_t r = pend_r;
        volatile Pend *p = &pend[r % NPEND];
        drum_hit(p->model, p->vel, p->eng, p->gain, p->tune, p->snap, p->drop, p->dec);
        pend_r = (uint8_t)(r + 1u);
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
        uint32_t cut, dsh, model, eng;
        int32_t gs;
        if (!v->alive) continue;
        any = 1;
        gs = (int32_t)v->gain;
        dsh = v->sh;
        model = v->model;
        eng = v->engc;
        if (dsh > 14u) dsh = 14u;
        if (model > 3u) model = 1u;
        if (eng > 3u) eng = 0;
        cut = 1u + (v->tune > 100u ? 0u : (100u - v->tune) / 25u) + ENG_HAT[eng];
        if (cut < 1u) cut = 1u;
        if (cut > 8u) cut = 8u;
        for (i = 0; i < n; i++) {
            int32_t s = 0;
            uint32_t osc;
            if (model == 2u) {
                int32_t nz = noise(), hp, mixed;
                v->lp += (nz - v->lp) >> cut;
                hp = nz - v->lp;
                mixed = hp + ((v->lp * (int32_t)v->drop) >> 7);
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
                if (model == 1u) {
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
        for (i = 0; i < NKIT; i++) {
            char k[3] = {'K', (char)('1' + i), 0};
            num(n0, set.kit[i].level);
            row_set(i, k, KITN[i], n0, (int)set.kit[i].level, 0, 100);
        }
    } else if (page == PG_EDIT && edit_alt == 2u) {
        const MapDef *k = map_of(set.kit[tr].map);
        uint32_t lane = lane_now(), note = set.kit[tr].note[lane];
        note_text(n2, note);
        ch_text(n3, set.kit[tr].ch);
        row_set(0, "K1", "MAP", k->name, (int)(set.kit[tr].map < NMAP ? set.kit[tr].map : 0u), 0, (int)NMAP - 1);
        row_set(1, "K2", "PAGE", set.kit[tr].bank ? "2" : "1", (int)set.kit[tr].bank, 0, k->b ? 1 : 0);
        row_set(2, "K3", lane_name(tr, lane), n2, (int)note, 0, 127);
        row_set(3, "K4", "CHANNEL", n3, (int)set.kit[tr].ch, 1, 16);
    } else if (page == PG_EDIT && edit_alt) {
        uint32_t i, base = lane_now() & 4u;
        for (i = 0; i < 4u; i++) {
            char k[3] = {'K', (char)('1' + i), 0};
            note_text(n0, set.kit[tr].note[base + i]);
            row_set(i, k, lane_name(tr, base + i), n0, (int)set.kit[tr].note[base + i], 0, 127);
        }
    } else if (page == PG_EDIT) {
        static const char *NM[4] = {"TUNE", "DECAY", "SNAP", "DROP"};
        uint32_t i, lane = lane_now();
        for (i = 0; i < 4u; i++) {
            char k[3] = {'K', (char)('1' + i), 0};
            num(n0, set.kit[tr].parm[lane][i]);
            row_set(i, k, NM[i], n0, (int)set.kit[tr].parm[lane][i], 0, 100);
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
    uint32_t len = set.length, tr = trk(), lane = lane_now(), vel;
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
        if (patn >= NPAT) patn = 0;
        num(n0, slot + 1u);
        num(n1, patn + 1u);
        num(n2, sl);
        row_set(0, "K1", "SLOT", n0, (int)slot, 0, (int)sl - 1);
        row_set(1, "K2", "PATTERN", n1, (int)patn, 0, (int)NPAT - 1);
        row_set(2, "K3", "LENGTH", n2, (int)sl, 1, 16);
        row_set(3, "K4", "TRACK", trk_name(tr), (int)tr, 0, (int)NKIT - 1);
        return;
    }
    vel = set.vel[pat_now()][tr][lane][cursor];
    num(n0, (uint32_t)cursor + 1u);
    num(n1, vel);
    num(n2, len);
    row_set(0, "K1", "STEP", n0, (int)cursor, 0, (int)len - 1);
    row_set(1, "K2", "VELOCITY", n1, (int)vel, 0, 127);
    row_set(2, "K3", "LENGTH", n2, (int)len, 1, 16);
    row_set(3, "K4", "LANE", lane_name(tr, lane), (int)lane, 0, (int)NLANE - 1);
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
    uint32_t i, x = 0, len = set.length, tr = trk(), pat = pat_now(), lane = lane_now();
    if (len < 1u) len = 1u;
    if (len > 16u) len = 16u;
    cv_begin(240, 48, C_BLACK);
    for (i = 0; i < 16u; i++) {
        uint32_t vel = 0, h = 0;
        uint16_t col = C_DIM;
        if (i < len) {
            vel = set.vel[pat][tr][lane][i];
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
            if (p >= NPAT) p = 0;
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
        uint32_t eng = set.kit[tr].engine;
        n = put_str(d, 40, n, " ");
        n = put_str(d, 40, n, ENG[eng > 3u ? 0u : eng]);
    } else if (page == PG_EDIT && edit_alt == 2u) {
        n = put_str(d, 40, n, " ");
        n = put_str(d, 40, n, map_of(set.kit[tr].map)->name);
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
    n = put_str(d, 40, n, " ");
    n = put_str(d, 40, n, lane_name(tr, lane_now()));
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
    if (page == PG_HOME) {
        uint32_t eng = set.kit[trk()].engine;
        return ENGNXT[eng > 3u ? 0u : eng];
    }
    if (page == PG_SEQ && rec && seq_run) return " REC WRITES THE STEP";
    if (page == PG_SEQ && seq_view == VW_SONG) return " PLAY WALKS THE CHAIN";
    if (page == PG_SEQ) return " BLACK KEY PICKS LANE";
    if (page == PG_EDIT && edit_alt == 2u) return map_of(set.kit[trk()].map)->hint;
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
static uint8_t off_on[16], off_ch[16], off_note[16];
static uint32_t off_at[16];
static void note_off_later(uint32_t ch, uint32_t note, uint32_t ms) {
    uint32_t i, slot = 0, best = 0xFFFFFFFFu;
    for (i = 0; i < 16u; i++)
        if (off_on[i] && off_ch[i] == ch && off_note[i] == note) { off_at[i] = fm1_ms + ms; return; }
    for (i = 0; i < 16u; i++) if (!off_on[i]) { slot = i; break; }
    if (i == 16u) {
        for (i = 0; i < 16u; i++) if (off_at[i] < best) { best = off_at[i]; slot = i; }
    }
    off_on[slot] = 1;
    off_ch[slot] = (uint8_t)ch;
    off_note[slot] = (uint8_t)note;
    off_at[slot] = fm1_ms + ms;
}
static void offs_flush(void) {
    uint32_t i;
    for (i = 0; i < 16u; i++)
        if (off_on[i] && (int32_t)(fm1_ms - off_at[i]) >= 0) {
            midi_out(0x80u | off_ch[i], off_note[i], 0);
            off_on[i] = 0;
        }
}
static uint32_t out_ch(void) { return set.ch ? (uint32_t)set.ch - 1u : 0u; }
static uint32_t kit_ch(uint32_t ki) {
    uint32_t ch;
    if (ki >= NKIT) ki = 0;
    ch = set.kit[ki].ch;
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

/* Prefer the selected kit when several kits answer the same note. */
static int find_lane(uint32_t midi_ch, uint32_t note, uint32_t *ki_out) {
    uint32_t order[NKIT], n = 0, i, k, lane;
    order[n++] = trk();
    for (k = 0; k < NKIT; k++) if (k != order[0]) order[n++] = k;
    for (i = 0; i < n; i++) {
        k = order[i];
        if (kit_ch(k) != midi_ch) continue;
        for (lane = 0; lane < NLANE; lane++) {
            uint8_t played = set.kit[k].note[lane];
            if (played && played == note) { *ki_out = k; return (int)lane; }
        }
    }
    return -1;
}

static void request_lane(uint32_t ki, uint32_t lane, uint32_t vel, int record) {
    uint32_t note, g, model, eng, ch, w, r;
    if (ki >= NKIT || lane >= NLANE || !vel) return;
    if (vel > 127u) vel = 127u;
    note = set.kit[ki].note[lane];
    if (!note) return;
    if (note > 127u) note = 36u;
    model = set.kit[ki].model[lane];
    if (model > 3u) model = 1u;
    eng = set.kit[ki].engine;
    if (eng > 3u) eng = 0;
    g = kit_gain(ki, lane);
    if (g) {
        w = pend_w;
        r = pend_r;
        if ((uint8_t)(w - r) < NPEND) {
            volatile Pend *p = &pend[w % NPEND];
            p->model = (uint8_t)model;
            p->vel = (uint8_t)vel;
            p->eng = (uint8_t)eng;
            p->gain = (uint8_t)(g > 255u ? 255u : g);
            p->tune = set.kit[ki].parm[lane][0];
            p->snap = set.kit[ki].parm[lane][2];
            p->drop = set.kit[ki].parm[lane][3];
            p->dec = set.kit[ki].parm[lane][1];
            pend_w = (uint8_t)(w + 1u);
        }
    }
    ch = kit_ch(ki);
    midi_out(0x90u | ch, note, vel);
    note_off_later(ch, note, 70u);
    if (ki == trk()) take_push(128u + lane, vel);
    menv_trig(0);
    if (record && rec && seq_run) {
        uint8_t s = seq_pos, len = set.length, p = (uint8_t)pat_now();
        if (len < 1u) len = 1u;
        if (len > 16u) len = 16u;
        if (s < len) {
            set.vel[p][ki][lane][s] = (uint8_t)vel;
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
    uint32_t bpm, i, t, step_ms, len, pat, ki, oldest, mask = 0;
    bpm = take_bpm();
    if (!bpm) { message("NO TAKE"); return; }
    len = set.length < 1u ? 1u : set.length;
    if (len > 16u) len = 16u;
    pat = pat_now();
    ki = trk();
    oldest = (take_w + NTAKE - take_n) % NTAKE;
    for (i = 0; i < take_n; i++) {
        uint8_t note = takeq[(oldest + i) % NTAKE].note;
        if (note >= 128u && note < 128u + NLANE) mask |= 1u << (note - 128u);
    }
    if (!mask) { message("NO TAKE"); return; }
    for (i = 0; i < NLANE; i++) if (mask & (1u << i)) {
        uint32_t s;
        for (s = 0; s < 16u; s++) set.vel[pat][ki][i][s] = 0;
    }
    step_ms = 15000u / bpm;
    if (step_ms < 1u) step_ms = 1u;
    t = 0;
    for (i = 0; i < take_n; i++) {
        TakeEv *e = &takeq[(oldest + i) % NTAKE];
        uint32_t step;
        t += e->dt;
        step = (t + step_ms / 2u) / step_ms % len;
        if (e->note >= 128u && e->note < 128u + NLANE)
            set.vel[pat][ki][(uint32_t)e->note - 128u][step] = e->vel ? e->vel : 100u;
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
static void lane_parms(uint32_t ki) {
    static const uint8_t P[4][4] = {
        {42, 100, 28, 78},
        {58, 76, 55, 18},
        {62, 42, 100, 18},
        {48, 88, 20, 64},
    };
    static const uint8_t L[4] = {100, 92, 64, 78};
    uint32_t lane, m;
    for (lane = 0; lane < NLANE; lane++) {
        m = set.kit[ki].model[lane];
        if (m > 3u) m = 1u;
        set.kit[ki].lvl[lane] = L[m];
        set.kit[ki].parm[lane][0] = P[m][0];
        set.kit[ki].parm[lane][1] = P[m][1];
        set.kit[ki].parm[lane][2] = P[m][2];
        set.kit[ki].parm[lane][3] = P[m][3];
    }
}
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
    set.atk = 2; set.dec = 28; set.sus = 100; set.rel = 24;
    set.arp_div = 2; set.arp_gate = 55; set.arp_oct = 1;
    set.lfo_rate = 36; set.lfo_cc = 1; set.lfo_dest = 2;
    set.env_cc = 11; set.env_amt = 100;
    for (i = 0; i < NKIT; i++) {
        set.kit[i].level = 100;
        set.kit[i].engine = 0;
        set.kit[i].map = 0;
        set.kit[i].bank = 0;
        apply_map(i);
        set.kit[i].ch = (uint8_t)(10u + i);
        lane_parms(i);
    }
    for (i = 0; i < 16u; i++) {
        set.vel[0][0][0][i] = K0[i];
        set.vel[0][0][1][i] = S0[i];
        set.vel[0][0][2][i] = H0[i];
    }
    set.vel[0][0][3][15] = 96;
}
static void clamp_set(void) {
    uint32_t i, m;
    if (set.oct < -2 || set.oct > 2) set.oct = 0;
    if (set.ch > 16u) set.ch = 0;
    if (set.tempo < 40u || set.tempo > 240u) set.tempo = 120;
    if (set.swing < 50u || set.swing > 75u) set.swing = 50;
    if (set.length < 1u || set.length > 16u) set.length = 16;
    if (set.pattern >= NPAT) set.pattern = 0;
    if (set.track >= NKIT) set.track = 0;
    if (set.lane >= NLANE) set.lane = 0;
    if (set.machine > 5u) set.machine = 2;
    if (set.song_len < 1u || set.song_len > 16u) set.song_len = 1;
    for (m = 0; m < 6u; m++) {
        if (set.bits[m] > 4u) set.bits[m] = 2;
        if (set.rate[m] > 4u) set.rate[m] = 0;
        if (set.tone[m] > 4u) set.tone[m] = 2;
    }
    if (set.wet > 100u) set.wet = 100;
    if (set.drive > 8u) set.drive = 0;
    for (i = 0; i < NKIT; i++) {
        uint32_t lane, k;
        if (set.kit[i].map >= NMAP) set.kit[i].map = 0;
        if (set.kit[i].bank > 1u) set.kit[i].bank = 0;
        if (set.kit[i].ch < 1u || set.kit[i].ch > 16u) set.kit[i].ch = (uint8_t)(10u + i);
        if (set.kit[i].engine > 3u) set.kit[i].engine = 0;
        if (set.kit[i].level > 100u) set.kit[i].level = 100;
        map_labels(i);
        for (lane = 0; lane < NLANE; lane++) {
            if (set.kit[i].note[lane] > 127u) set.kit[i].note[lane] = 36;
            if (set.kit[i].lvl[lane] > 100u) set.kit[i].lvl[lane] = 100;
            if (set.kit[i].model[lane] > 3u) set.kit[i].model[lane] = model_of(lname[i][lane]);
            for (k = 0; k < 4u; k++) if (set.kit[i].parm[lane][k] > 100u) set.kit[i].parm[lane][k] = 40;
        }
    }
    for (i = 0; i < 16u; i++) if (set.song[i] >= NPAT) set.song[i] = 0;
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
}
static void audio_params(void) {
    uint32_t w = set.wet;
    if (w > 100u) w = 100u;
    wet_q8 = w * 256u / 100u;
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
            {
                uint8_t np = set.song[0];
                set.pattern = np < NPAT ? np : 0;
            }
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
        {
            uint32_t ki, k, drumch = 0;
            int lane = find_lane(ch, d1, &ki);
            for (k = 0; k < NKIT; k++) if (kit_ch(k) == ch) drumch = 1;
            if (ty == 0x90u && d2 && lane >= 0) request_lane(ki, (uint32_t)lane, d2, 1);
            if (lane >= 0 || drumch) continue;
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
        set.kit[k].level = (uint8_t)clampu((int)set.kit[k].level + e, 0, 100);
    } else if (page == PG_SEQ && seq_view == VW_SONG) {
        uint32_t sl = set.song_len < 1u ? 1u : set.song_len;
        if (sl > 16u) sl = 16u;
        if (song_slot >= sl) song_slot = 0;
        if (k == 0u) song_slot = (uint8_t)clampu((int)song_slot + e, 0, (int)sl - 1);
        else if (k == 1u) set.song[song_slot] = (uint8_t)clampu((int)set.song[song_slot] + e, 0, (int)NPAT - 1);
        else if (k == 2u) set.song_len = (uint8_t)clampu((int)set.song_len + e, 1, 16);
        else set.track = (uint8_t)clampu((int)set.track + e, 0, (int)NKIT - 1);
    } else if (page == PG_SEQ) {
        uint32_t len = set.length < 1u ? 1u : set.length;
        uint32_t lane = lane_now();
        if (k == 0u) cursor = (uint8_t)clampu((int)cursor + e, 0, (int)len - 1);
        else if (k == 1u)
            set.vel[pat_now()][tr][lane][cursor] = (uint8_t)clampu((int)set.vel[pat_now()][tr][lane][cursor] + e, 0, 127);
        else if (k == 2u) set.length = (uint8_t)clampu((int)set.length + e, 1, 16);
        else set.lane = (uint8_t)clampu((int)set.lane + e, 0, (int)NLANE - 1);
    } else if (page == PG_EDIT && edit_alt == 2u) {
        if (k == 0u) {
            set.kit[tr].map = (uint8_t)clampu((int)set.kit[tr].map + e, 0, (int)NMAP - 1);
            set.kit[tr].bank = 0;
            apply_map(tr);
            message(map_of(set.kit[tr].map)->msg);
        } else if (k == 1u) {
            if (!map_of(set.kit[tr].map)->b) message("ONE PAGE");
            else {
                set.kit[tr].bank = (uint8_t)clampu((int)set.kit[tr].bank + e, 0, 1);
                apply_map(tr);
                message(set.kit[tr].bank ? "PAGE 2" : "PAGE 1");
            }
        } else if (k == 2u) {
            set.kit[tr].note[lane_now()] = (uint8_t)clampu((int)set.kit[tr].note[lane_now()] + e, 0, 127);
        } else set.kit[tr].ch = (uint8_t)clampu((int)set.kit[tr].ch + e, 1, 16);
    } else if (page == PG_EDIT && edit_alt) {
        uint32_t ix = (lane_now() & 4u) + k;
        set.kit[tr].note[ix] = (uint8_t)clampu((int)set.kit[tr].note[ix] + e, 0, 127);
    } else if (page == PG_EDIT) {
        set.kit[tr].parm[lane_now()][k] = (uint8_t)clampu((int)set.kit[tr].parm[lane_now()][k] + e, 0, 100);
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
    while (t < 0) t += (int)NKIT;
    set.track = (uint8_t)(t % (int)NKIT);
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
            uint32_t d, n = seq_nev;
            seen_gen = seq_gen;
            if (n > 16u) n = 16u;
            for (d = 0; d < n; d++) {
                midi_out(0x90u | seq_ev[d].ch, seq_ev[d].note, seq_ev[d].vel);
                note_off_later(seq_ev[d].ch, seq_ev[d].note, 60u);
                menv_trig(0);
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
                int grid = page == PG_SEQ && seq_view == VW_GRID;
                if (note > 127u) note = 127u;
                if (page == PG_ARP) {
                    down_note[k] = (uint8_t)note;
                    down_count++;
                    menv_trig(1);
                    if (set.arp_mode) note_hold((uint8_t)note);
                    else { tone_start(note, vel); midi_out(0x90u | out_ch(), note, vel); }
                } else if (grid && white_ord[k] != 255u && white_ord[k] < set.length) {
                    uint8_t w = white_ord[k];
                    uint32_t tr = trk(), lane = lane_now();
                    uint8_t *cell = (uint8_t *)&set.vel[pat_now()][tr][lane][w];
                    cursor = w;
                    *cell = *cell ? 0 : 100;
                    if (*cell) request_lane(tr, lane, 100, 0);
                    dirty = 1;
                    t_change = fm1_ms;
                    paint_req = 1;
                } else if (grid && black_ord[k] < NLANE) {
                    set.lane = black_ord[k];
                    request_lane(trk(), set.lane, vel, 1);
                    dirty = 1;
                    t_change = fm1_ms;
                    paint_req = 1;
                } else if (!grid) {
                    request_lane(trk(), KEY_LANE[note % 12u], vel, 1);
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
                if (page == PG_ARP) {
                    menv_trig(1);
                    if (set.arp_mode) note_hold((uint8_t)note);
                    else { tone_start(note, vel); midi_out(0x90u | out_ch(), note, vel); }
                } else if (!(page == PG_SEQ && seq_view == VW_GRID))
                    request_lane(trk(), KEY_LANE[note % 12u], vel, 1);
            } else if (ext_note[key]) {
                if (page == PG_ARP) {
                    if (set.arp_mode) note_drop(ext_note[key]);
                    tone_stop(ext_note[key]);
                    midi_out(0x80u | out_ch(), ext_note[key], 0);
                    menv_release();
                }
                ext_note[key] = 0;
            }
        }
        while ((e = smk_pad_take()) >= 0) {
            uint32_t pad = ((uint32_t)e >> 8) & 15u, vel = (uint32_t)e & 0x7Fu;
            if (vel) request_lane(trk(), pad & 7u, vel, 1);
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
            seq_view = (uint8_t)(seq_view == VW_GRID ? VW_SONG : VW_GRID);
            paint_req = 1;
        } else if (pg == PG_HOME && page == PG_HOME) {
            uint32_t eng = (set.kit[trk()].engine + 1u) & 3u;
            set.kit[trk()].engine = (uint8_t)eng;
            dirty = 1;
            t_change = fm1_ms;
            paint_req = 1;
            message(ENGMSG[eng]);
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
                    uint32_t d, s, n, ki = trk();
                    for (d = 0; d < NLANE; d++) for (s = 0; s < 16u; s++) set.vel[pat_now()][ki][d][s] = 0;
                    dirty = 1; t_change = fm1_ms; paint_req = 1; clear_arm = 0;
                    n = put_str(msgbuf, 24, 0, trk_name(ki));
                    put_str(msgbuf, 24, n, " CLEARED");
                    message(msgbuf);
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
            while (p < 0) p += (int)NPAT;
            set.pattern = (uint8_t)(p % (int)NPAT);
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
            uint32_t lane = lane_now();
            if (page == PG_SEQ && seq_view == VW_GRID) {
                uint8_t w = white_ord[k];
                if (w != 255u && w < set.length) {
                    if (set.vel[pat_now()][trk()][lane][w]) glow = 1;
                    if (seq_run && w == seq_pos) { on = 1; glow = 0; }
                } else if (black_ord[k] == lane) glow = 1;
            } else if (page != PG_ARP && (base + k) % 12u < 8u) glow = 1;
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
             ((uint32_t)seq_view << 28) | (lane_now() << 29);
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
