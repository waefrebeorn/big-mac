# R002 — Real-DAW Architecture Study (Ardour + LMMS source)

**Status:** wired (lessons applied to engine core).
Studied the actual source of two real DAWs, extracted the patterns that make
them correct, and applied them. Sources are local clones in `~/ref/`.

## 7-Hop (grounded in real source, not summaries)

| # | Source | Core finding |
|---|---|---|
| 1 | LMMS `src/core/AudioEngine.cpp` | Notes/samples become **live play-handles** owned by the engine. New ones pushed via a **lock-free list** (`m_newPlayHandles.popList()`). RT thread never allocates. |
| 2 | LMMS `NotePlayHandle.h` | Each play-handle carries its own `m_frames`/`m_totalFramesPlayed`/`m_releaseFramesDone` + `m_pluginData` — a self-contained voice with explicit release lifecycle. |
| 3 | LMMS `AudioEngine.cpp` `renderStage*` | Render pipeline is **staged**: `renderStageNoteSetup` (remove dead handles, swap buffers, create new handles) → `renderStageInstruments` (worker threads) → `renderStageEffects`. |
| 4 | LMMS `AudioEngine.cpp` ctor | **Double-buffered output** (`m_outputBufferRead/Write`, `swapBuffers()`). Engine is device-independent; platform backends (Alsa/Jack/SDL/Pulse/Dummy) plug in. |
| 5 | Ardour `libs/ardour/audioengine.cc` `process_callback` | RT callback takes a **mutex try-lock**; on contention it emits **Xrun (underrun)** and silences output — never blocks the audio thread. |
| 6 | Ardour `libs/ardour/graph.cc` | Processing is a **DAG of routes** run by a worker pool woken by **semaphores** (`_callback_start_sem`/`_callback_done_sem`), topological order. |
| 7 | Ardour `graph.cc` `main_thread` | Realtime discipline: suspend rt-malloc checks, per-thread event pools, sample-count wrap handling. |

## Convergent truth

> **A DAW's audio thread is a staged, device-independent pipeline over a set
> of live, self-contained note/voice objects (play-handles), double-buffered
> and allocation-free. The render callback must never block: it try-locks and
> reports an Xrun instead. New voices arrive via a lock-free queue; the
> graph (routes/effects) is processed in dependency order, optionally on a
> worker pool.**

## Applied to Big Mac DAW (deltas vs R001)

1. **Play-handle model (NEW)** — replace the ad-hoc per-track voice array
   with a real play-handle list owned by the engine. Each handle tracks its
   own position/length/release. New notes are pushed via the existing
   lock-free command queue; the engine spawns handles on note-on and runs
   them to completion (correct polyphony + release).
2. **Staged render (NEW)** — split the block render into
   `stage_schedule` (spawn/retire handles) → `stage_instruments`
   (process each handle) → `stage_effects` (track insert chain). Mirrors
   LMMS's pipeline.
3. **Xrun/underrun detection (NEW)** — render callback uses a try-lock on a
   process mutex; on contention it counts an Xrun, silences output, returns
   immediately. Xrun counter exposed to UI (learn-loss-style metric).
4. **Double-buffered output** — keep two output buffers, swap per block.
5. **Realtime safety audit** — confirm zero malloc/lock/syscall on the RT
   path (preallocate all buffers at session build).

## Wired ledger update

| Capability | Status | Verify |
|---|---|---|
| Play-handle voice model | `wired` | `make test` (polyphony/release) |
| Staged render (schedule→instr→fx) | `wired` | `make test` |
| Xrun detection + counter | `wired` | `make test` |
| Double-buffered output | `wired` | `make test` |
