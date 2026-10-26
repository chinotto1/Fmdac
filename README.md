# Fmdac

Browser installer for Fmdac on an M-VAVE FM-1.

The page lives in [`docs/`](docs/index.html). In Chrome or Edge, on a computer, plug the FM-1 in with a USB data cable and install from that page. The package stays in the browser. Web MIDI needs a secure page, so open the published site or serve `docs/` yourself:

```sh
python3 -m http.server -d docs 8765
```

Then open `http://127.0.0.1:8765`.

The page loads [`docs/fmdac.fwsc`](docs/fmdac.fwsc) by itself. That file is Fmdac 0.9, identity `FM-1_969`. Four kit tracks, T1 to T4. A track is eight lanes, and all four kits play together. SELECT picks the kit. On HOME the selected kit is marked, and pressing HOME again chooses MIDI or a drum machine for that kit. A MIDI kit is silent locally and still sends MIDI. SEL on a drum kit opens 808, 909, CR-78, and SDS. SEL on a MIDI kit opens the map list: SELECT moves, SEL picks, and HOME closes it. On the grid, a black key picks a lane and K4 is that lane. Off the grid, C is the kick, D the snare, and F# the closed hat. A MIDI kit can play a scale from the keys. On a drum kit, EDIT is tune, decay, tone, and drop, then the lane levels. On a MIDI kit, EDIT is the scale, then the notes, then the map. K1 starts with GM, TR-909, TR-707, TR-727, TR-08, Volca Beats, and Hammerhead, and K2 opens the next page of that chart. Those notes come from the instruments' published MIDI charts. Hammerhead there is the grouped list. HAMR2 is the same eight notes in the order the OCTACHRON author posted. The list and K1 then continue through the AUv3 app maps from the OCTACHRON list. The names on the screen are short, and the bottom line names the app. A lane the chart leaves empty stays silent. OneShot and Tekno are not included, because those apps do not publish a fixed note chart. The file does not contain every preset stored in the OCTACHRON app. SEQ is the step grid, then a song chain of four patterns. REC while stopped quantizes a take of one, two, or four bars across at most four patterns and sets the BPM. A song saved by 0.8 loads as the factory kits. HOME sets each kit's level. The four sounding voices are shared. While a computer streams audio to the FM-1, that audio is converted on the way to the headphones. Playback at 48 kHz is resampled to the headphone clock. When nothing is streaming, the headphones hear the FM-1's own mix, and the computer records that mix. There are no sample ROMs, and the take is not a looper. Installing it replaces the firmware on the FM-1. The page checks the file against `product`, `size`, and `sha256` in [`docs/releases.json`](docs/releases.json).

The source of that image is in [`fw/`](fw/). It is Dinghy at commit `22acb9165ebc8baa712af3aff8c507e230c1e028`, with the Fmdac 0.9 app, the `FM-1_969` identity, and a loader that reports `ota-FM-1_969`. Rebuild it with the JieLi toolchain and the AC79 SDK, which are not in this repository:

```sh
export JIELI_TOOLCHAIN="$HOME/.jieli/toolchain"
export AC79_SDK="$HOME/fw-AC79_AIoT_SDK"
cd fw && ./build.sh --release 0.9
```

The other tab installs three published packages after the same check: Skiff 1.3.2 (`FM-1_713`), SLOOP 2.7.1 (`FM-1_900`), and the M-VAVE `FM-1.fwsc` whose identity was `FM-1_016` on October 25, 2026. Those files are downloaded from their publishers. They are not stored in this repository.

GitHub Pages publishes `docs/` from the Pages workflow. In the repository settings, set Pages to **GitHub Actions**. The site is then [chinotto1.github.io/Fmdac](https://chinotto1.github.io/Fmdac/).

The update protocol in `docs/vendor/` is adapted from Skiff and Felucca by Leo Kuroshita, Hügelton Instruments, and is GPL-3.0-only. See `docs/vendor/NOTICE.md`. Installing firmware can leave an FM-1 unable to start until it is recovered. There is no warranty.

```sh
node --test docs/test.mjs
```
