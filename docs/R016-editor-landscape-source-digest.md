# R016 — Full Editor Landscape: Source Acquisition + Sauce Digest

**Date:** 2026-08-18
**Scope:** Acquire the entire editor landscape as local source (language-agnostic —
Rust/JS/Python/C++ all fair game; we SLERM to C11), then extract the real
architectural "sauce" from each via source reading. Builds on R015 (feature
landscape) by going from *features* to *implementation*. The user's directive:
"acquire and pull all of the projects, I don't care if it is rust, we make it c."

All clones live in `~/ref/` (OUTSIDE the big-mac repo — no pollution). Shallow
(`--depth 1`) to keep them lean, matching the R002 `~/ref/ardour` + `~/ref/lmms`
convention. We study and SLERM; we never link or vendor their code.

---

## 0. Acquired source (in `~/ref/`)

| Project | Lang/Stack | Why acquire | Source-read? |
|---|---|---|---|
| `ardour/` | C++ | R002 DAW RT pipeline | ✅ (R002) |
| `lmms/` | C++ | R002 play-handle model | ✅ (R002) |
| `natron/` | C++ | R013 Fusion-equivalent node compositor | ✅ (R013) |
| `openfx/` | C | R013 OpenFX host/plugin contract | ✅ (R013) |
| `olive/` | C++/Qt | node graph + keyframes + frame cache | ✅ (this doc) |
| `opencut/` | Rust/web | CapCut twin; plugin-first + MCP | ✅ README/struct (this doc) |
| `lossless-cut/` | TS/Electron | `-c copy` trim, scene/black/silence detect | ✅ (this doc) |
| `shotcut/` | C++/MLT | native multi-format timeline | acquired; read pending |
| `kdenlive/` | C++/MLT | proxy + VST audio | acquired; read pending |
| `openshot-qt/` | Python/Qt | beginner NLE structure | acquired; read pending |
| `flowblade/` | Python | Linux NLE, G'MIC effects | acquired; read pending |
| `vidcutter/` | Python/mpv | lossless cut GUI | acquired; read pending |
| `moviepy/` | Python | programmatic edit API | acquired; read pending |
| `editly/` | Node | declarative NLE (ffmpeg) | acquired; read pending |
| `FFCreator/` | Node | short-video factory | acquired; read pending |
| `openshorts/` | Python | auto-shorts from long video | acquired; read pending |
| `audacity/` | C++ | label tracks / waveform edit | acquired; read pending |
| `tenacity/` | C++ | audacity fork (multi-track) | acquired; read pending |
| `pyJianYingDraft/` | Python | CapCut draft format interop | acquired; read pending |

> "read pending" = cloned, not yet source-read in this pass. The sauce below is
> from projects already read; the rest are queued for R017 if you want the full
> per-project digest. The feature-level lessons from all of them are already in
> R015.

---

## 1. Sauce extracted (from source, not summaries)

### S1 — Olive: pull-based node graph + typed inputs (validates R013 D1/D6)
`app/node/node.h`:
- `Node::AddInput(id, NodeValue::Type, default, flags)` — every node input is
  **typed** (kColor, kFootage, kNumber, kText, kBoolean, ...). Type-checked
  connections = the graph can't wire incompatible plugs. Our `wb_video_node`
  should carry a typed-input table too.
- `Node::GetValueAtTime(input, time)` — a node is evaluated by pulling each
  input's value at a time; the traverser (`app/node/traverser.cpp`) does the
  recursive pull (`GenerateTable` → `GenerateRow` → `ProcessInput`). Same pull
  model as Natron (R013). **Two independent open impls agree → it's the standard.**
- `ValueHint` — an input can hint "I want a color from tag X," enabling UI
  auto-wiring. Nice-to-have for our node UI.

### S2 — Olive: keyframe model (our "keyframes on any param", R015 Tier)
`app/node/keyframe.h` `NodeKeyframe`:
- Per-input keyframe **tracks** (`NodeKeyframeTrack = QVector<NodeKeyframe*>`).
- `enum Type { kLinear, kHold, kBezier }` + `BezierType { kInHandle, kOutHandle }`
  with `bezier_control_in/out()` QPointF handles.
- **Correctness detail we must copy:** `valid_bezier_control_in/out()` clamps
  the handle so X (time) is monotonic — prevents a bezier that loops in time.
  This is the kind of bug that silently corrupts animation; Olive solved it.
- Interpolation resolved in `Node::GetValueAtTime` by walking
  `previous()/next()` keyframes on the track. Our C11 `wb_param_track` should
  mirror: typed value + keyframe list + linear/hold/bezier(2 handles) + valid-clamp.

### S3 — Olive: frame hash cache (validates R013 D3)
`app/render/framehashcache.h` `FrameHashCache : PlaybackCache`:
- Cached frame keyed by `(uuid, time, timebase)`; `IsFrameCached(time)`,
  `GetValidCacheFilename`, `SaveCacheFrame/LoadCacheFrame`. This is EXACTLY our
  R013 D3 `wb_video_cache_key{time,scale,rect}` — validated by a second impl.
- `precachetask.h` proactively caches upcoming frames → our "decode-ahead on
  proxy" idea (R015 Tier 2) is standard practice.

### S4 — LosslessCut: lossless trim via concat demuxer + `-c copy`
`src/main/ffmpeg.ts`:
- `runFfmpegConcat({ffmpegArgs, concatTxt, ...})` pipes a **concat demuxer**
  script (`ffconcat` text) into ffmpeg stdin with `-c copy` → multi-segment
  lossless export. This is the exact command shape for our **Tier-1 lossless
  keyframe trim** (`wb_video_lossless_trim`): build a concat list of
  `[inpoint,outpoint]` segments, remux, zero re-encode.
- Scene/black/silence detection: `detectSceneChanges` (ffmpeg `select=gt(scene,..)`
  filter), `blackDetect` (`blackdetect` filter), and a silent-detect path. These
  are the **building blocks of auto-cut / auto-clip-to-shorts** (R015 Tier 3) —
  pure ffmpeg filters, no ML. We can own all three on our FFmpeg backbone.

### S5 — OpenCut: plugin-first + Editor API + MCP server (the "amazing" meta-feature)
`README` (rewrite): "first-class third party plugins (plugin-first
architecture), desktop/mobile/browser from one Rust core, **MCP server (for AI
agents)**, headless mode (automation, batch rendering), a scripting tab."
- Lesson: the differentiator for 2026 is **AI-agent/scriptable + plugin-first**.
  Our engine should expose a stable C API (it already does — `wb_*` functions)
  that an agent (Hermes!) or a scripting tab can drive. The `wb_ofx_host` (R013
  D4) is the plugin layer; a thin command/automation layer on top makes us
  "agent-editable." This is higher-leverage than any single effect.

### S6 — OpenFX (R013 recap, the Fusion/Resolve bridge)
The plugin action contract (`kOfxActionLoad`/`Describe`/`Render(RoI)`) is what
lets Fusion/Resolve/Nuke OFX plugins load in Big Mac. The single most
"standards-lifting" move. (Full digest in R013.)

---

## 2. Convergent truth (extends R015)

> **Source-reading two independent node editors (Natron + Olive) confirms the
> pull-based, typed-input, RoI/RoD, frame-hash-cache model is THE standard for
> compositing. LosslessCut proves lossless multi-segment export is just a concat
> demuxer + `-c copy`, and that scene/black/silence detection (pure ffmpeg
> filters) is the cheap path to auto-cut. OpenCut proves the 2026 differentiator
> is plugin-first + AI-agent (MCP) + headless automation. So the C11 Big Mac
> target is: a pull-based typed node graph (S1/S2/S3) for video+audio, a lossless
> trim + detect-based auto-cut layer (S4) on our FFmpeg backbone, and a stable
> engine API that an agent/script drives (S5) — all SLERM'd from these sources,
> none linked.**

---

## 3. What we SLERM from each (concrete C11 targets)

| Sauce | Source | C11 target in Big Mac |
|---|---|---|
| Typed-input pull node graph | Olive S1, Natron R013 | `wb_video_node` with typed input table + `wb_video_node_pull()` |
| Keyframe tracks (lin/hold/bezier + valid-clamp) | Olive S2 | `wb_param_track` (per-param animation) |
| Frame hash cache (uuid,time,scale) | Olive S3, Natron R013 | `wb_video_cache_key` + mem/disk cache |
| Lossless concat `-c copy` trim | LosslessCut S4 | `wb_video_lossless_trim()` |
| Scene/black/silence detect → auto-cut | LosslessCut S4 | `wb_video_autocut()` (ffmpeg filters) |
| Plugin-first + agent/headless API | OpenCut S5 | engine C API + (later) `wb_ofx_host` |
| OFX host (Fusion/Resolve plugins) | OpenFX R013 | `wb_ofx_host` (R013 D4) |

---

## 4. Ledger delta
- New refs in `~/ref/`: olive ✅(read), opencut ✅(read), lossless-cut ✅(read),
  + 13 more acquired (shotcut/kdenlive/openshot-qt/flowblade/vidcutter/moviepy/
  editly/FFCreator/openshorts/audacity/tenacity/pyJianYingDraft) — read pending.
- New doc: R016 (this file).
- Validates R013 D1/D3/D6 with a second independent source (Olive).
- Adds concrete C11 SLERM targets table.
- See R014 P1–P12 for the rules these targets obey; R015 for the feature matrix.

## 5. Next
- R017 (optional): source-read the "read pending" repos (especially
  shotcut/kdenlive proxy+MLT, audacity label-tracks for transcript edit,
  openshorts auto-clip) and append per-project sauce.
- Then implement Tier-1 C11 targets (lossless trim + keyframe tracks) against
  `src/wb_video.c` with headless tests.
