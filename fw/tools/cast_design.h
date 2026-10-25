/* SPDX-License-Identifier: GPL-3.0-only
 * Host-side coefficient design. This file uses floating point and must not
 * be linked into the FM-1 image. cast_dac_process() is the firmware path.
 */
#ifndef CAST_DESIGN_H
#define CAST_DESIGN_H

#include "cast_dac.h"

enum {
  CAST_VS = 0,
  CAST_DX7 = 1,
  CAST_SP1200 = 2,
  CAST_EII = 3,
  CAST_S950 = 4,
  CAST_MACHINES = 5
};

/* tone_milli: 0..1000, or -1 for that machine's default.
 * bits: 0 for the machine default. rate_hz: 0 for the machine default.
 */
void cast_design(CastDac *d, int machine, int bits, int rate_hz, int tone_milli, int host_fs);

const char *cast_machine_name(int machine);
int cast_default_bits(int machine);
int cast_default_rate(int machine);

#endif
