# Fmdac

Browser installer for Fmdac on an M-VAVE FM-1.

The page lives in [`docs/`](docs/index.html). In Chrome or Edge, on a computer, plug the FM-1 in with a USB data cable and install from that page. The package stays in the browser. Web MIDI needs a secure page, so open the published site or serve `docs/` yourself:

```sh
python3 -m http.server -d docs 8765
```

Then open `http://127.0.0.1:8765`.

The page loads [`docs/fmdac.fwsc`](docs/fmdac.fwsc) by itself. That file is Fmdac 0.4, identity `FM-1_964`. Eight tracks: KICK, SNARE, HAT, TOM, and silent MIDI tracks M1–M4. SELECT, SEL, or K4 changes the track. SEQ is a step grid, a keys-and-cursor piano roll, and a song chain. REC while stopped quantizes a short take into the pattern and sets the BPM. HOME cycles synthesized 808, 909, CR-78, and SDS characters. While a computer streams audio to the FM-1, that audio is converted on the way to the headphones. When nothing is streaming, the headphones hear the FM-1's own mix, and the computer records that mix. There are no sample ROMs and no OCTACHRON factory map. Installing it replaces the firmware on the FM-1. The page checks the file against `product`, `size`, and `sha256` in [`docs/releases.json`](docs/releases.json).

The source of that image is in [`fw/`](fw/). It is Dinghy at commit `22acb9165ebc8baa712af3aff8c507e230c1e028`, with the Fmdac 0.4 app, the `FM-1_964` identity, and a loader that reports `ota-FM-1_964`. Rebuild it with the JieLi toolchain and the AC79 SDK, which are not in this repository:

```sh
export JIELI_TOOLCHAIN="$HOME/.jieli/toolchain"
export AC79_SDK="$HOME/fw-AC79_AIoT_SDK"
cd fw && ./build.sh --release 0.4
```

The other tab installs three published packages after the same check: Skiff 1.3.2 (`FM-1_713`), SLOOP 2.7.1 (`FM-1_900`), and the M-VAVE `FM-1.fwsc` whose identity was `FM-1_016` on October 25, 2026. Those files are downloaded from their publishers. They are not stored in this repository.

GitHub Pages publishes `docs/` from the Pages workflow. In the repository settings, set Pages to **GitHub Actions**. The site is then [chinotto1.github.io/Fmdac](https://chinotto1.github.io/Fmdac/).

The update protocol in `docs/vendor/` is adapted from Skiff and Felucca by Leo Kuroshita, Hügelton Instruments, and is GPL-3.0-only. See `docs/vendor/NOTICE.md`. Installing firmware can leave an FM-1 unable to start until it is recovered. There is no warranty.

```sh
node --test docs/test.mjs
```
