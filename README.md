# Big Mac DAW

A sample-accurate, C11, zero-third-party digital audio workstation.

Pull-based engine behind CoreAudio; offline export reuses the same render
path. Built for a dual-core i5 iMac, portable enough to build anywhere.

## Layout

```
big-mac/
├── Makefile              # build + test gate
├── AGENTS.md             # agent context (this repo)
├── README.md             # this file
├── LICENSE               # WaefreBeorn Umbrella v3
├── docs/                 # architecture ledger (R001... INDEX.md)
├── include/wbus/         # public API: engine, transport, session
├── src/                  # engine, DSP, plugins
├── tests/                # test gate
├── tools/                # codegen, tooling
├── assets/               # sound effects, sources
└── third_party/          # vendored SDL2 (static, no system install)
```

## Build

```bash
make            # build
make test       # run the gate
```

`CC`/`CXX` default to clang; override with `make CC=gcc CXX=g++`.

## Host audio output

`bigmac_out.[ch]` is the device output backend for the engine's float frames:
interleaved `float` in [-1, 1] in, a real speaker out. It picks the first
backend that works:

| backend | how | when |
|---|---|---|
| `pulse` | PulseAudio `pa_simple` | a desktop box (transparent through PipeWire) |
| `alsa`  | `snd_pcm_writei` on `default` | a bare-metal install |
| `wav`   | 32-bit float PCM to a file | headless / CI -- still proves the pipeline |

`BIGMAC_BACKEND=pulse|alsa|wav` forces one. `bigmac_out_backend_name()`
reports what actually opened.

```c
#include "bigmac_out.h"

bigmac_out_init(48000, 2, "/tmp/wubu_audio.wav");
bigmac_out_write(frames, nframes);   /* each callback */
bigmac_out_close();
```

`bigmac_out_write()` returns frames accepted, or -1 on a hard error; an
underrun recovers rather than failing. `bigmac_out_latency_frames()` reports
buffered-but-unplayed frames.

Build the backend alone with `make -f Makefile.bigmac`.

## License

WaefreBeorn Umbrella License v3.0
