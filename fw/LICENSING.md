# Licensing

Fmdac 0.8 is this Dinghy tree with the name FMDAC, package identity `FM-1_968`, USB name Fmdac, and an update loader that reports `ota-FM-1_968`. The Dinghy logo is not included. Upstream is https://github.com/zvenson/dinghy commit `22acb9165ebc8baa712af3aff8c507e230c1e028`.

Dinghy is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its
complete source under the same licence too. The package `docs/fmdac.fwsc` is built from this directory.

## Who made what

| | |
| --- | --- |
| The platform: `firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `firmware/src/` (`usb.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`, `bootguard.h`, `uac_fir.h`), `tools/` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) |
| The SMK-37 board layer: `firmware/hal/smk_input.h`, `fm1_flash_map.h`, the SMK-37 parts of `fm1_cc.h`, `fm1_flash.h`, `fm1_lcd_hw.h`, `lcd.c`, `ota.c`, `bootguard.h`, `firmware/loader/`, `tools/fm1pkg_make.py`, `tools/fm1_install.py`, `tools/build.py`; `firmware/src/smk_wheel.h`; `tests/ota_test.c`, `ldr_test.c`, `install_test.py`, `smk_wheel_test.c`, `smk_pkg_test.py` | Copyright (C) 2026 M. Geisler (genkigenki/makkuro), from [genkigenki/sloop-fm1](https://github.com/genkigenki/sloop-fm1) branch `smk37` (the reference port of the [fm1-to-smk37](https://github.com/genkigenki/fm1-to-smk37) kit) |
| The TRS MIDI IN `firmware/src/midi_uart.c` | Felucca's, as sloopDX carries it, with the ring read by content from Felucca [Salt] 0.9-salt14 by ChanceTheMaker (found by keremimo) |
| The update loader's screen: `firmware/loader/ldr_screen.c`, `tests/ldr_screen_test.c` | Copyright (C) 2026 zvenson (from OberVave, the Firmware Hub's look); the idea, a loader that draws, its 5 x 7 letters and the order of things from [Skiff](https://github.com/illuminus5/skiff) by illuminus5, GPL-3.0 |
| From OberVave: the storage (blobs, slots), `LCD_IDLE`, the SMK-37 wiring of `platform.c`, `store.h`, the pad event queue in `smk_input.h`, `tests/smk_flash_test.c`, `tests/run_tests.sh`, the CI workflow | Copyright (C) 2026 zvenson |
| The original Dinghy app shape, the wiring `platform.c`, the host mode `tests/`, the docs | zvenson. `firmware/src/dinghy.c` in this tree is the Fmdac 0.8 app |
| Fmdac 0.8: `firmware/src/dinghy.c`, `firmware/src/cast_dac.c`, `firmware/src/cast_dac.h`, `firmware/src/cast_tab.h`, `tools/bake_cast.c`, `tools/cast_design.c`, `tools/cast_design.h`, and the computer-playback endpoint added in `firmware/src/usb.c` | GPL-3.0-only. `tools/cast_design.c` designs the filter tables on a computer and is not linked into the FM-1 image. `cast_tab.h` is the integer tables that image uses. The kit notes in `dinghy.c` are the published MIDI note numbers of those instruments, the Hammerhead list its author posted, and the AUv3 app maps from the public chart that matches the lists he posted. The rest of `usb.c` remains the Felucca and SLOOP driver |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## The logo

`assets/logo.svg` / `logo.png` carry the designburg signet: Copyright (C) designburg, all rights reserved, not under
the GPL. It marks Dinghy itself; a fork uses its own logo.

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE", "FM-1", "SMK-37" and "MKE-P37"
belong to their owners. Dinghy is not made or endorsed by them.
