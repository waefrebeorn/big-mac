# Big Mac DAW — Wired/Open Ledger

## Cloned vendored dependency
- `third_party/SDL2-2.32.10/` — SDL2 2.32.10 built from source as a static
  library. **The only third-party; everything else is ours.** Chosen for
  portable windowing + portable audio across macOS/Linux/Windows.

## Wired (verified by `make test`)
| Capability | Verify |
|---|---|
| Engine boot + transport (sample-accurate) | `make test` |
| Lock-free SPSC command queue (UI→engine) | `make test` |
| Session model (2-track, clips, notes, save/load) | `make test` |
| DSP graph + mixer (pan, gain, mute/solo) | `make test` |
| Subtractive synth (osc/env/filter, 16 voices) | `make test` |
| Compressor, delay, reverb, sampler | `make test` |
| WAV write (16-bit PCM + 32-bit float) | `make test` |
| Offline render (oracle parity with live) | `make test` |
| Recursive learn/fix feedback loop | `make test` (tuner converges) |
| SDL2 realtime audio backend | `make all && ./build/wb_daw` |
| SDL2 window + transport + arrangement + mixer | `build/wb_daw` |

## Open (next)
| Capability |
|---|
| MIDI input from hardware controller |
| Plugin scan + third-party plugin hosting |
| Project save/load (.wbus file format) |
| Automation envelopes (per-parameter curves) |
| Waveform display (audio clips on arrangement) |
| Font-based text rendering (UI labels/numbers) |
| Recording (audio/MIDI capture) |
| Additional synth/FX units |
| Undo/redo |
