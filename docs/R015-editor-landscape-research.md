# R015 — Editor Landscape Research (Kevin Bacon expansion)

**Date:** 2026-08-18
**Scope:** Broaden the editor study beyond Fusion/Natron (R013) to learn from the
widest possible set of audio + video + AI editors, find the features that make
each "amazing," and converge on what makes OUR editor the best. Builds on
R008/R009 (video editor MVP) and R013 (node-compositor sauce).

Method: 7-hop Kevin Bacon across ≥7 sources / ≥3 domains, plus real-source
study of the cloneable open editors. Each lesson cites a concrete source.

---

## 0. Sources acquired (local clones in `~/ref/`, outside the repo)
- `~/ref/olive/` — Olive video editor `master` (shallow, 50M). C++/Qt, **node-based
  compositing** + timeline. Directly relevant to R013's node direction.
- `~/ref/opencut/` — OpenCut `main` (shallow, 1.4M). MIT, CapCut twin (Rust/web
  stack — feature-set lesson, not C-source; do not SLERM its code).
- (prior) `~/ref/natron`, `~/ref/openfx`, `~/ref/ardour`, `~/ref/lmms`.

---

## 1. 7-hop Kevin Bacon — sources, core findings, ONE convergent truth

### Domain A — Node-based video editors (the Fusion/Resolve shape)
1. **Olive `app/node/`** — full node graph: `node.cpp`, `block/`, `input/`,
   `traverser.cpp`. `NodeTraverser::GenerateTable/GenerateRow/ProcessInput`
   recursively pull each input's value for a `TimeRange` — a **pull-based graph
   evaluator** (confirms R013 P3). Nodes are typed (footage/color/distort/filter/
   generator/keying/math/output).
2. **Olive `app/render/framehashcache.h`** — `FrameHashCache` keys a cached frame
   by `(uuid, time, timebase)`. **Per-time frame cache** — the exact shape of our
   R013 D3 tile cache, validated. Also `audioplaybackcache.h`, `precachetask.h`
   (proactive precache of upcoming frames).
3. **Olive `app/render/renderer.h` + `rendermodes.h`** — render is a job graph
   (`footagejob`, `shaderjob`, `cachejob`, `colortransformjob`); GPU (OpenGL)
   path optional. Mirrors our "C API decode + optional GPU" decision (R009 §3.1).
4. **Olive README / gappsy review** — "node-based compositing lets users build
   effects and transitions as a graph of connected nodes instead of a fixed
   linear chain." The differentiator vs Premiere-style fixed chains.

### Domain B — Native / no-import / multi-format timeline (the friction killer)
5. **Shotcut** (opensourcealternatives 2026) — edits **natively**: no import or
   format conversion step; the multi-format timeline mixes phone + camera +
   screencast footage at different resolutions/framerates in one project. This is
   a UX principle: **don't force a transcode before edit**. We already do this
   (FFmpeg decodes source directly; proxy is optional) — keep it.
6. **Kdenlive** — **proxy editing** (low-res working copies, full-res on export)
   + **VST audio** + customizable workspace. Validates R008 §E (480p proxy) and
   shows VST-on-video-audio is expected (we own the audio engine — bonus).
7. **LosslessCut** (mifi/lossless-cut) — flagship: **byte-level stream copy**
   (`ffmpeg -c copy`), cut on real keyframes, no re-encode, perfect quality. Also
   auto-split, segment removal. **Lesson:** a "lossless trim" path that only
   remuxes (no decode) is a first-class feature we can add trivially on top of
   our FFmpeg backbone — instant, zero-quality cuts.

### Domain C — Transcript / text-based editing (the AI-era differentiator)
8. **Descript "Edit like a doc"** — delete a word from the transcript → that word
   is removed from the audio/video; bulk filler-word removal ("um/uh");
   remove-from-transcript (keep media, hide text). **This is our R012 captions
   pipeline's natural extension:** once we have a transcript (whisper), editing
   the transcript edits the timeline. The convergent "amazing" feature for
   2026 editors is **text-as-the-timeline**.
9. **Riverside / Opus Clip / CapCut AI** — auto-show-notes, chapter breakdown,
   **auto-clip long video into social shorts**, auto-captions, **reframe/resize**
   (e.g. 16:9→9:16 with subject tracking), background removal. These are
   *pipeline* features, not engine features — they sit on top of transcribe +
   detect-highlights + reframe. We can own all three (whisper = done-adjacent).

### Domain D — Audio editors with unique DNA
10. **Hindenburg** — built for voice: **auto-level / auto-duck**, loudness
    targeting, voice-tailored EQ presets. Lesson: a "make voice sound right"
    one-click is a killer feature for our audio engine — we have comp/eq/gain;
    add a `wb_voice_polish` preset chain (gate → de-esser → comp → EQ → limiter,
    loudness-normalized). Cheap to build, huge perceived quality.
11. **Reaper** — **routing matrix** (any track → any track, unlimited sends,
    sidechain, parallel chains), and deep **scripting/action** customization
    (user-defined actions, per-project config). Lesson: expose a **routing
    matrix** UI (our P8 "user owns the graph") and a **scriptable action layer**
    so power users extend. Our node graph (R013 D1) is the routing matrix for
    video; audio should get the same.
12. **Bitwig** — **modular "The Grid"** + linear/non-linear hybrid, container
    devices, CV/MIDI. Lesson: modularity and containers (group nodes) are the
    modern expectation — our `NodeGroup` concept (Natron has it too) should be
    first-class.

### Domain E — OpenCut (the CapCut twin, the user's named reference)
13. **OpenCut** (opencut-app/opencut, MIT, 78k★) — "open-source CapCut
    alternative," web/desktop/mobile, self-hostable, privacy-first. Feature set
    to learn from: multi-layer editing, transitions, **keyframes**, **4K export**,
    auto-captions, text-based/subtitle editing, templates. The lesson is product
    scope: a CapCut-class MVP = trim/split/multilayer + transitions + keyframes +
    auto-captions + text edit + 4K export, all **local-first / private**. That is
    precisely our R008/R009/R012 target — OpenCut proves the scope is right.

### Domain F — Render/perf principles (carryover)
14. **Shotcut hardware-accel decode (NVENC/QSV/VCE)**, **Kdenlive proxy** — smooth
    4K needs either proxy OR HW decode. On this Mac (i5-4260U, 8GB) proxy is the
    only path; our 480p proxy decision (R008) is correct and matches the field.

### Domain G — Why some editors "feel best" (HCI, carryover from R006/R014)
15. **Convergent UX thread across Olive (nodes), Shotcut (native), Descript
    (text), OpenCut (simple layers):** the best editors **remove a translation
    step** between the user's intent and the result — nodes remove "fixed chain"
    thinking, native timeline removes "import/convert," text-editing removes
    "scrub-to-find," auto-captions remove "type subtitles." **Every "amazing"
    feature is the elimination of a manual translation step.** That is the design
    target.

---

## Convergent truth (one sentence)

> **The editors that win eliminate a manual translation step between intent and
> result: node graphs remove fixed-chain thinking (Olive/Natron), native
> multi-format timelines remove import/convert (Shotcut), transcript editing
> removes scrub-to-find (Descript), auto-captions/auto-clip remove typing/triage
> (OpenCut/CapCut/Opus), and lossless keyframe-trim removes re-encode (LosslessCut).
> Big Mac's path to "the best" is to own ALL of these on top of the one engine
> we have: a pull-based (node + timeline) compositor (R013) where the transcript
> is an editable view of the timeline (R012), voice-polish and a routing matrix
> are first-class (Hindenburg/Reaper), and a lossless-trim path is free (FFmpeg
> -c copy). One engine, every translation step removed.**

---

## 2. Feature matrix — what to learn / adopt

| Feature | Source(s) | Adopt into Big Mac? | How (build on existing) |
|---|---|---|---|
| Pull-based node graph | Olive, Natron (R013) | YES (R013 D1/D6) | `wb_video_node_pull` + `wb_ofx_host` |
| Per-time frame hash cache | Olive `FrameHashCache` | YES (R013 D3) | `wb_video_cache_key{time,scale,rect}` |
| Proactive precache of next frames | Olive `precachetask` | YES | background decode-ahead on proxy |
| Native multi-format timeline (no import) | Shotcut | DONE (FFmpeg direct decode) | keep; don't force transcode |
| 480p proxy + full-res export | Kdenlive, R008 | DONE (R009) | keep |
| Lossless keyframe trim (-c copy) | LosslessCut | YES (cheap win) | `wb_video_lossless_trim` remux path |
| Transcript = editable timeline | Descript, OpenCut | YES (R012 extension) | whisper transcript → clip edit ops |
| Auto-captions + text/subtitle edit | OpenCut, CapCut, R012 | DONE-direction (R012) | SRT overlay on export |
| Auto-clip to shorts + reframe | Opus Clip, Riverside, CapCut | LATER | detect highlights + 9:16 reframe |
| Voice auto-level/duck/polish | Hindenburg | YES (cheap win) | `wb_voice_polish` preset chain |
| Routing matrix (any→any sends) | Reaper | YES (P8) | node graph is the matrix; audio too |
| Modular containers / Grid | Bitwig, Natron groups | YES | `NodeGroup` first-class |
| Keyframes on any param | OpenCut, Olive, all | YES | param animation track (extend R009) |
| Scriptable/extensible actions | Reaper | LATER | action layer over engine API |

---

## 3. "Make ours the best" — prioritized adoption (verify each)

**Tier 1 — cheap, high-perceived-quality, build on what we have:**
1. **Lossless keyframe trim** (`-c copy` remux, cut on keyframes) — free, zero
   decode, instant. Test: trim a clip, diff is byte-identical stream.
2. **Voice polish preset** (`wb_voice_polish`: gate→deesser→comp→EQ→limiter,
   loudness-norm) — reuses our DSP. Test: noisy voice in → normalized loudness out.
3. **Transcript-editable timeline** (R012 captions → edit transcript removes
   words from audio/video) — the Descript differentiator. Test: delete a
   transcript word → corresponding media silent/removed.

**Tier 2 — the architecture bets (R013):**
4. Pull-based node graph + per-time frame cache (D1/D3/D6). Test: 2-node chain,
   cache hit on re-eval, RoI pull only needed slice.
5. Routing matrix / NodeGroup for audio too (P8). Test: arbitrary track→track send.

**Tier 3 — AI pipeline on top (once T1/T2 land):**
6. Auto-clip to shorts + 9:16 reframe (subject-track + crop). Test: long video →
   N shorts with captions.
7. `wb_ofx_host` to load Fusion/Resolve/Nuke OFX plugins (R013 D4). Test: external
   plugin node renders.

---

## 4. Ledger delta
- New refs: `~/ref/olive` (node video editor), `~/ref/opencut` (CapCut twin, MIT).
- New doc: R015 (this file).
- Validates R013 D3 (Olive `FrameHashCache`) and D6 (Olive `NodeTraverser` pull).
- Adds adoption matrix + 3-tier "best editor" roadmap.
- See `R014-design-principles.md` P1–P12 for the rules these adoptions obey.
