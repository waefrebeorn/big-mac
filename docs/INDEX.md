# Big Mac DAW — Wired/Open Ledger

## Cloned vendored dependency
- `third_party/SDL2-2.32.10/` — SDL2 2.32.10 built from source as a static
  library. **The only third-party; everything else is ours.** Chosen for
  portable windowing + portable audio across macOS/Linux/Windows.

## Reference sources studied (R002)
- `~/ref/ardour/` — Ardour (`libs/ardour/audioengine.cc`, `graph.cc`)
- `~/ref/lmms/` — LMMS (`src/core/AudioEngine.cpp`, `midi/MidiApple.cpp`)
Lessons applied: staged render (schedule→instruments→effects), Xrun
try-lock, DAG worker model.

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
| Staged render pipeline (schedule→instr→fx) | `make test` |
| Xrun detection + counter (try-lock, never blocks) | `make test` |
|| Insert FX chain (comp/reverb/delay per track) | `make test` |
|| Per-slot bypass toggle + wet/dry mix on every insert slot (track & bus) | `make test` (test_bypass_wet + test_compressor_sidechain verify per-slot state + parallel wet signals + key-input ducking) |
| CoreMIDI input (enumerate + open by name) | launched, opened "Launchpad MK2" |
| Text UI (5×7 bitmap font: labels/numbers) | `build/wb_daw` shows time/BPM/dB |

## Open (next)
| Capability |
|---|
| Sends/returns/aux (pre/post-fader sends from any track to any bus; parallel FX return tracks; sidechain-capable compressor key input) |
| Per-insert wet/dry mix + per-slot bypass toggles (bus + track) |
| Per-insert on/off bypass (track + bus insert slots) |
| **Launchpad Mk2 driver (our own, C11)** — see R006/R007. Current `wb_launchpad_*` uses the *classic* `row*16+col` + velocity-color map, which is WRONG for Mk2 (Mk2 = `11+col+row*10` + RGB SysEx `F0 00 20 29 02 18 0B n r g b F7`). Need `wb_lp_mk2_*` + `wb_midi_send_sysex` + `wb_scale_*`. |
| **Tabbed UI** — KEYS / PAD (Mk2 piano roll) / STEP / SESSION as one editable clip object (R006 convergence: kill the Session-vs-Arrangement metaphor gap). |
| **Scale lock** — pick root+type once, both rolls snap in-key; Mk2 lights in-scale pads (FL/Ableton "scale highlighting" owned). |
| **Color = state** on Mk2 grid (playhead=white, loop=amber, clip=green, sel=blue, off-scale=dim). |

## Research docs
- `R006-launchpad-ux-research.md` — 7-hop Kevin Bacon (25 sources, 8 domains): convergent truth = one owned surface, scale-locked, color-coded, no menu-diving.
- `R007-launchpad-mk2-driver-and-tabs.md` — applied spec: Mk2 byte-exact protocol, C11 driver API, tabbed UI, scale lock, verification plan, build order.

Pinned: R002 wiring (staged render/Xrun/double-buffer/DAG-worker model) is done — next is the mixing topology above, not more RT-pattern work. Research R006/R007 scopes the Launchpad Mk2 + tabbed-UI push.
