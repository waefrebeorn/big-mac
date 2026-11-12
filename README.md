# big-mac

**The host audio output backend for WuBuOS.**

WuBuOS's audio engine (`src/audio/` in the main repo) is a renderer: chip
emulations, a Furnace tracker, an SF2 synth, a DAW mixer, all producing
interleaved float frames. It had no device path -- `wubu_audio_process()` was
called only by its own test, and nothing ever put a frame on a speaker.

big-mac is the missing back half. It takes those frames and writes them to a
real output, choosing the first backend that works:

| backend | how | when |
|---|---|---|
| `pulse` | PulseAudio `pa_simple` | a desktop box; goes through PipeWire transparently |
| `alsa`  | `snd_pcm_writei` on `default` | a bare-metal install |
| `wav`   | 32-bit float PCM to a file | headless / CI -- still proves the pipeline instead of silently producing nothing |

Set `BIGMAC_BACKEND=pulse|alsa|wav` to force one; otherwise it falls through
in that order. `bigmac_out_backend_name()` reports what actually opened.

## Build

```bash
make          # libbigmac_out.a
make test     # build + run the behavior gate
make static   # libbigmac_out_static.a
```

## Use

```c
#include "bigmac_out.h"

bigmac_out_init(48000, 2, "/tmp/wubu_audio.wav");
/* ... each callback: */
bigmac_out_write(frames, nframes);
/* ... at shutdown: */
bigmac_out_close();
```

Frames are interleaved `float` in [-1, 1]. `bigmac_out_write()` returns the
frames accepted, or -1 on a hard error; a device underrun recovers rather than
failing. `bigmac_out_latency_frames()` reports buffered-but-unplayed frames.

## History

This repo previously held an espeak-ng-derived formant TTS engine
(`wb_esynth`), sound effects and ingest tooling. That work was removed
(commit `858ebdb`) when audio editing stopped being active. The repository
was then repurposed for the output backend that the engine was missing.

## License

WaefreBeorn Umbrella License v3.0
