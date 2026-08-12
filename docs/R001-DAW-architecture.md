# R001 — DAW Architecture: Convergent Truth

**Status:** wired after engine boots; the doc converges the 7-hop research.

## 7-Hop Research (Kevin-Bacon)

| # | Source / field | Core finding |
|---|---|---|
| 1 | KVR DSP forum — DAW/sequencer design | Sequencer MUST run in the audio (RT) thread for sample-accurate timing; audio processing is a graph of nodes connected by wires |
| 2 | Ross Bencina — RT audio engineering | UI and audio thread communicate via lock-free FIFOs / double-buffered command queues; NO allocations, locks, or syscalls on the audio thread |
| 3 | CLAP plugin ABI (free-audio/clap) | A clean C ABI with a single event queue carrying note/param/timing events is the modern plugin contract; proven threading model |
| 4 | bastibe — CoreAudio internals | macOS native path = an AudioUnit with a render callback; output written into a buffer the callback is handed; CoreAudio is the lowest-latency, most-documented-in-C path |
| 5 | Ardour / Ardour's pbd (open-source DAW) | A serious open DAW keeps an editable "session" model (undoable, offline-renderable) separate from the realtime graph |
| 6 | Steinberg VST3 C++ ABI problems | C++ ABIs are a portability trap; a C ABI (CLAP-style) avoids RTTI/name-mangling fragility |
| 7 | Ableton Live / Bitwig model | Session+Arrangement duality: clips are the source material, the arrangement is the linear timeline; soft CPU budget, buffer-aware scheduling |

## Convergent truth

> **A DAW is a sample-accurate audio graph pulled by a hardware render
> callback. All musical state (transport, clips, events) lives in the
> realtime thread. The UI holds its own editable copy of the session and
> pushes changes to the engine through a lock-free command queue. DSP is a
> C11 plugin ABI (CLAP-style: one event queue, `process()` in/out buffers).**
> On weak hardware the engine must be pull-based, allocation-free on the RT
> path, and buffer-sized to the CPU — degrade gracefully instead of glitching.

## Our architecture (SLERM of the above — every byte ours)

```
┌────────────── UI thread ──────────────┐
│ Session model (editable, undoable)     │
│ Arrangement view / mixer / transport   │  (CoreGraphics/Cocoa host)
└───────────────┬────────────────────────┘
        lock-free command queue (SPSC ring)
┌───────────────▼────────────────────────┐
│ Engine (RT thread)                     │
│  Transport (sample-accurate)           │
│  Timeline -> clips -> notes/audio      │
│  DSP graph (nodes + wires)             │
│  Mixer (tracks -> groups -> master)    │
└───────────────┬────────────────────────┘
        render callback (pull)
┌───────────────▼────────────────────────┐
│ AudioBackend: CoreAudio AudioUnit      │
│   + WAV/AIFF offline export (verify)   │
└────────────────────────────────────────┘
```

## Design decisions

1. **Plugin ABI — "wbus" (WuBu Studio):** our own C11 ABI modeled on CLAP's
   proven shape. `wbus_plugin` = {host, `process()`, param get/set, event
   queue}. Built-in DSP implements it; external plugins could later too.
2. **Transport in RT thread.** The sequencer advances by sample; every
   event (note-on/off, clip start, automation point) is scheduled at an
   exact sample within the render block. No wall-clock guessing.
3. **Lock-free command queue.** UI→engine is an SPSC ring of fixed-size
   commands (`set_param`, `note`, `play`, `seek`, `edit`). RT thread never
   blocks, never allocates (arena per render block).
4. **Pull-based render.** Engine only produces audio when the backend asks
   for a block. Offline export pulls the same path — so what you hear IS
   what you export (oracle parity).
5. **Pure C11, zero third-party.** CoreAudio + CoreGraphics are the OS's
   own C libraries (the platform, not a dependency) — permitted. Everything
   else is ours.
6. **Weak-hardware discipline:** 44.1kHz stereo, double-buffered, CPU load
   meter, auto quality scaling; allocations all upfront at graph build.

## Wired ledger (scanner-owned)

| Capability | Status | Verify command |
|---|---|---|
| Engine boot + transport | `open` → `wired` | `make test && build/wb_daw --selftest` |
| DSP graph + mixer | `open` | `make test_dsp` |
| WAV/AIFF export | `open` | `build/wb_daw render demo.wav` |
| CoreAudio live output | `open` | `build/wb_daw play` |
| Synth / sampler / FX | `open` | `make test_units` |
| GUI (arrangement+mixer) | `open` | `build/wb_daw` |
