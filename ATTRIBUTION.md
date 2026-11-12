# Attribution — `old` branch

This branch is the archive of Big Mac's earlier vocal-synthesis work. It was
removed from the mainline in commit `858ebdb` ("cleanup: remove obsolete
audio-engine content") and restored here so it stays reachable and credited.

## What is here

| file | what | origin |
|---|---|---|
| `src/wb_glottis.c`, `include/wb_glottis.h` | LF glottal-flow source (vocal folds) | Liljencrants-Fant model (1985) |
| `src/wb_esynth.c`, `include/wb_esynth.h` | additive formant synthesis engine | faithful port of **espeak-ng** |

## espeak-ng

`wb_esynth` is a port of espeak-ng's additive formant synthesis engine.

- espeak-ng is Copyright (C) 2006–2016 by the espeak-ng authors.
- License: **GNU General Public License v3.0**.
- Project: <https://github.com/espeak-ng/espeak-ng>

The port was made at R015 (commit `f097cf1`, "continuous formant-trajectory
engine (`wb_esynth_phrase`)"), building on `3899d41` ("fresh start: `wb_esynth`
— faithful port of espeak-ng's additive formant engine").

Because this code is a derived work of a GPL-3.0 program, it carries that
license. It lives on this archive branch, separate from the mainline, so the
licensing boundary is explicit rather than mixed into the DAW.

## Liljencrants-Fant

The glottal source implements the LF model of Liljencrants & Fant (1985), the
standard parameterisation of glottal flow derivative used in speech and
singing synthesis. It is a published scientific model, not a copyrighted
implementation.

## Related work

The mainline branch carries the DAW and the host audio output backend
(`bigmac_out.[ch]`). WuBuOS links that backend for device output.
