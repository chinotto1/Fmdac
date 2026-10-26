/* SPDX-License-Identifier: GPL-3.0-only */
#include "cast_design.h"

#include <math.h>
#include <string.h>

static int q14(double c) {
  double s = c * 16384.0;
  if (s > 32767.0) {
    s = 32767.0;
  }
  if (s < -32768.0) {
    s = -32768.0;
  }
  return (int)lrint(s);
}

/* RBJ low-pass, unity gain at DC, coefficients stored for dac.c's DF2T
 * (a1, a2 kept with their cookbook sign, and the recurrence subtracts them).
 */
static void lpf(CastBq *b, double fs, double fc, double q) {
  if (fc < 40.0) {
    fc = 40.0;
  }
  if (fc > fs * 0.47) {
    fc = fs * 0.47;
  }
  double w0 = 2.0 * 3.141592653589793 * fc / fs;
  double cw = cos(w0);
  double sw = sin(w0);
  double alpha = sw / (2.0 * q);
  double b0 = (1.0 - cw) / 2.0;
  double b1 = 1.0 - cw;
  double b2 = (1.0 - cw) / 2.0;
  double a0 = 1.0 + alpha;
  double a1 = -2.0 * cw;
  double a2 = 1.0 - alpha;
  memset(b, 0, sizeof(*b));
  b->b0 = q14(b0 / a0);
  b->b1 = q14(b1 / a0);
  b->b2 = q14(b2 / a0);
  b->a1 = q14(a1 / a0);
  b->a2 = q14(a2 / a0);
}

static const double Q4[2] = {0.54119610, 1.30656296};
static const double Q6[3] = {0.51763809, 0.70710678, 1.93185165};

static int add_butterworth(CastBq *dst, int n, int maxn, double fs, double fc, int order) {
  if (order <= 2) {
    if (n < maxn) {
      lpf(&dst[n++], fs, fc, 0.70710678);
    }
    return n;
  }
  const double *q = order >= 6 ? Q6 : Q4;
  int sections = order >= 6 ? 3 : 2;
  for (int i = 0; i < sections && n < maxn; i++) {
    lpf(&dst[n++], fs, fc, q[i]);
  }
  return n;
}

const char *cast_machine_name(int machine) {
  switch (machine) {
  case CAST_VS:
    return "Prophet VS";
  case CAST_DX7:
    return "DX7";
  case CAST_SP1200:
    return "SP-1200";
  case CAST_EII:
    return "Emulator II";
  case CAST_S950:
    return "S950";
  default:
    return "?";
  }
}

int cast_default_bits(int machine) {
  switch (machine) {
  case CAST_EII:
    return 8;
  case CAST_DX7:
    return 12;
  default:
    return 12;
  }
}

int cast_default_rate(int machine) {
  switch (machine) {
  case CAST_DX7:
    return 49096;
  case CAST_SP1200:
    return 26040;
  case CAST_EII:
    return 27777;
  case CAST_S950:
    return 22050; /* crunch start; the sampler itself goes to 48 kHz */
  default:
    return 44100; /* Prophet VS: no fixed low rate is documented */
  }
}

void cast_design(CastDac *d, int machine, int bits, int rate_hz, int tone_milli, int host_fs) {
  memset(d, 0, sizeof(*d));
  if (bits <= 0) {
    bits = cast_default_bits(machine);
  }
  if (rate_hz <= 0) {
    rate_hz = cast_default_rate(machine);
  }
  if (tone_milli < 0) {
    tone_milli = 500;
  }
  if (tone_milli > 1000) {
    tone_milli = 1000;
  }
  d->bits = bits;
  d->step = ((uint32_t)rate_hz << 16) / (uint32_t)host_fs;
  if (d->step < 256u) {
    d->step = 256u;
  }

  double tone = tone_milli / 1000.0;
  /* 400 Hz .. 16 kHz, exponential, so the middle of the knob is musically useful. */
  double tone_fc = 400.0 * pow(16000.0 / 400.0, tone);

  switch (machine) {
  case CAST_VS:
    d->law = CAST_LAW_LINEAR;
    /* No fixed oscillator sample rate is documented. The default tone is a
     * 4-pole at 8 kHz: a patch standing in for the CEM3379, not a factory
     * measurement. Any other tone position uses the exponential curve.
     */
    if (tone_milli == 500) {
      tone_fc = 8000.0;
    }
    d->npost = add_butterworth(d->post, 0, CAST_BQ_MAX, host_fs, tone_fc, 4);
    break;
  case CAST_DX7:
    d->law = CAST_LAW_DX7;
    d->bits = 12; /* the scaler is the resolution; the bits knob does not retune it */
    break;
  case CAST_SP1200:
    d->law = CAST_LAW_LINEAR;
    /* Yeh's SP-12 paper: the input anti-alias is real, and the playback
     * that makes the machine is a zero-order hold with no reconstruction
     * on the bright outputs. 11070 Hz is 0.85 of the 13.02 kHz Nyquist,
     * the estimate in the SP-1200 service notes. Tone 0 and the default
     * tone leave the output as that hold. The other tones bring in a
     * 4-pole, standing in for the filtered channels.
     */
    d->npre = add_butterworth(d->pre, 0, CAST_BQ_MAX, host_fs, 11070.0, 4);
    if (tone_milli > 0 && tone_milli != 500) {
      d->npost = add_butterworth(d->post, 0, CAST_BQ_MAX, host_fs, tone_fc, 4);
    }
    break;
  case CAST_EII:
    d->law = CAST_LAW_DPCM; /* Rossum: mu-law COMDAC on the sample difference */
    d->dc_block = 1;
    d->npre = add_butterworth(d->pre, 0, CAST_BQ_MAX, host_fs, 12000.0, 2);
    if (tone_milli == 500) {
      tone_fc = 12000.0;
    }
    d->npost = add_butterworth(d->post, 0, CAST_BQ_MAX, host_fs, tone_fc, 4);
    break;
  case CAST_S950:
    d->law = CAST_LAW_LINEAR;
    /* Input trim starts at unity. Drive is there for the knob; the default
     * measurement is the converter and the filter, not the soft clip.
     */
    d->drive_q8 = 0;
    {
      double bw = 0.40 * (double)rate_hz;
      if (bw > 19000.0) {
        bw = 19000.0;
      }
      d->npre = add_butterworth(d->pre, 0, CAST_BQ_MAX, host_fs, bw, 6);
      d->npost = add_butterworth(d->post, 0, CAST_BQ_MAX, host_fs, bw, 6);
      /* Service manual: MF6CN-50, 6 poles, clocked from the voice so the
       * cutoff follows the sample rate, after a 2-pole Sallen-Key near
       * 21 kHz. The 6-pole already uses every biquad, so the fixed stage
       * is left out and the tracking filter is the one that is modeled.
       */
      (void)tone_fc;
    }
    break;
  default:
    d->law = CAST_LAW_LINEAR;
    d->bits = 16;
    d->step = 65536u;
    break;
  }
  cast_dac_reset(d);
}
