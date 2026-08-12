# Big Mac DAW — repo layout

A sample-accurate, C11, zero-third-party digital audio workstation for a
dual-core i5 iMac. Pull-based engine behind CoreAudio; offline export reuses
the same render path.

```
big-mac/
├── Makefile              # build + test gate
├── AGENTS.md             # agent context (this repo)
├── README.md
├── LICENSE               # WaefreBeorn Umbrella v3
├── docs/
│   ├── R001-DAW-architecture.md
│   ├── R002-audio-engine.md      (transport, timeline, graph)
│   ├── R003-plugin-abi.md        (wbus ABI)
│   ├── R004-dsp-units.md         (synth/sampler/FX design)
│   └── INDEX.md                  (wired/open ledger)
├── include/wbus/
│   ├── wbus.h            # public API: engine, transport, session
│   ├── wbus_plugin.h     # plugin ABI (host+plugin structs)
│   ├── wbus_dsp.h        # DSP unit descriptors + process
│   └── wbus_backend.h    # audio backend interface
├── src/
│   ├── wb_core.c         # engine: transport, graph, mixer, cmd queue
│   ├── wb_transport.c    # sample-accurate sequencer/timeline
│   ├── wb_cmd.c          # lock-free SPSC command queue
│   ├── wb_session.c      # session model (clips, tracks, project save/load)
│   ├── wb_dsp.c          # DSP registry + unit lifecycle
│   ├── wb_osc.c          # oscillators (saw/sine/square/noise)
│   ├── wb_env.c          # ADSR envelope
│   ├── wb_filter.c       # biquad EQ / SVF filters
│   ├── wb_comp.c         # compressor/limiter
│   ├── wb_reverb.c       # feedback delay network reverb
│   ├── wb_delay.c        # delay / echo
│   ├── wb_synth.c        # subtractive polysynth instrument
│   ├── wb_sampler.c      # sample playback instrument
│   ├── wb_wav.c          # WAV/AIFF read+write (16-bit PCM / f32)
│   ├── wb_backend.c      # backend dispatch: CoreAudio + offline file
│   ├── wb_backend_coreaudio.c  # AUHAL render callback path (macOS)
│   ├── wb_midi.c         # MIDI note mapping / controller in
│   └── wb_ui.c           # CoreGraphics arrangement+mixer+transport
├── tools/
│   ├── wb_daw.c          # the app: UI + engine + backend main
│   ├── wb_render.c       # CLI offline render: project -> wav
│   └── wb_selftest.c     # headless engine self-test (the gate)
├── tests/
│   ├── test_transport.c
│   ├── test_dsp.c
│   ├── test_wav.c
│   └── test_units.c
└── projects/
    └── demo.wbus         # a demo song (created by tooling)
```
