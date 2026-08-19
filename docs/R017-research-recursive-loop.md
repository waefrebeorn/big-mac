# R017 — Recursive 7-Hop Research Loop: Gaps & Opportunities

**Method:** 4 parallel research subagents, each ran a 7-hop Kevin Bacon across
≥6 sources / ≥3 domains (25+ sources total, 7 domains). Goal: find what Big
Mac's video+audio editor is *missing* vs the best-in-class, and confirm the
architecture bets already made in R013–R016. This is the recursive
improvement loop: know where you are by knowing where you aren't.

**Domains covered:** Natron, Olive, MLT/Shotcut/Kdenlive, Blender VSE, MoviePy,
CasparCG, Resolume (compositor/NLE); Audacity/Tenacity, Ardour, Hindenburg/
Descript, Opus Clip/Riverside/CapCut, OpenCut (audio + AI); VapourSynth,
AVISynth, VirtualDub, GStreamer, mpv, Godot/Unity, DaVinci (realtime/render);
OFX, VST3, CLAP, OTIO/EDL/CMX3600/FCPXML/AAF, ffmpeg lavfi, ASS/SSA/SRT,
Lottie, glTF (interop/extensibility).

---

## Convergent truth (one line)

> Every mature editor collapses to a **single host-driven, pull-on-demand node
> graph** with **auto-inserted bounded edge caches**, a **proxy/quality dial
> negotiated by downstream QoS**, and a **GPU path that always retains a CPU
> fallback** — and it exposes **one normalized, time-addressed parameter/event
> bus** that the same automation timeline drives for audio plugins (VST3/CLAP),
> video FX (OFX), and compositor nodes alike.

Big Mac is already pointed at exactly this (R013 pull compositor + keyframe
tracks + VST3/CLAP automation). The research says: **the bet is right; now
close the remaining gaps below.**

---

## Gap → Action table (ranked by leverage on a dual-core iMac)

| # | Gap (where we aren't) | Evidence (source) | Action for Big Mac | Priority |
|---|------------------------|-------------------|--------------------|----------|
| G1 | No **proxy-scale QoS dial**; proxy is a fixed 480p swap, not a quality knob | GStreamer QoS (`proportion`+`jitter`) [T3-2]; Resolve Render Cache [T3-6]; FFmpeg lavfi scale as swappable node [T3-9] | Add `wb_compositor_set_quality(t)` 0..1 → swaps proxy vs full-res + lowers tile size on slow frames | P1 |
| G2 | Cache is hand-placed, not **auto-inserted at graph edges** | AVISynth internal caching intercepts `GetFrame` between nodes [T3-3]; Natron hash-cache per node [T1-1] | Make the host insert a `wb_node_cache` automatically after every non-source node (bounded LRU) | P1 |
| G3 | **Two-phase pull** not modeled (request vs compute) | VapourSynth `arInitial`→`arAllFramesReady` [T3-1] | Split `wb_node_pull` into request(inputs)+compute; enables async decode on slow HW | P2 |
| G4 | No **OFX host** — can't load Fusion/Resolve/Nuke effects | OFX `GetRegionOfDefinition`/`GetRegionOfInterest`/`IsIdentity` [T4-1] | Implement minimal OFX host runner reusing the RoI+tile+identity contract already in `wb_compositor` | P1 |
| G5 | No **EDL/OTIO/XML interchange** — projects don't travel | OTIO `adapters.read/write_from_file` (edl,fcpxml,aaf) [T4-4]; CMX3600 reel/char limits [T4-5] | Export/import CMX3600 + FCPXML via a thin adapter; OTIO spine later | P2 |
| G6 | **Transcript-editable timeline** not wired (whisper done, UI not) | Descript wordbar; Hindenburg auto-level [T2]; OpenCut plugin-first [T2] | Add caption-track editing in the EDIT tab: click a word → seek; drag → trim (reuses `wb_video_segment`) | P1 |
| G7 | Voice-polish is a one-shot chain, not a **pluggable filter-graph** | FFmpeg `loudnorm` two-pass + `afftdn`/`deesser`/`dynaudnorm` as composable nodes [T2-1,2] | Model `wb_voice_polish` as a param-track-driven graph; expose each stage as a tunable node (gate thr, deesser freq, comp ratio) | P2 |
| G8 | Loudness is single-pass measure-then-scale; not **EBUR128 two-pass** | FFmpeg `loudnorm` measure→linear-apply [T2-1] | Add a true two-pass: measure whole buffer, then linear gain (current single-pass is acceptable for v1) | P3 |
| G9 | No **agent/MCP API** — can't be driven headlessly by AI | OpenCut MCP server for AI agents [T2]; OpenFX passive plugins [T1] | Expose a `--batch` / named-pipe command API on `wb_daw` (already has `wb_e2e_export` pattern) | P2 |
| G10 | ASS **override-token parser** missing (only SRT burn) | ASS `Dialogue:` + `\pos \move \b \i \c&HBBGGRR&` [T4-7] | Add an ASS burn path feeding lavfi `subtitles=` for styled captions | P3 |
| G11 | Keyframe tracks exist but **not yet bound to FX/automation** | VST3 `IParamValueQueue`; CLAP `clap_event_param_value` sample-offset [T4-2,3] | Drive compositor node params + audio plugin params from the same `wb_param_track` (unify the bus) | P1 |
| G12 | No **GPU-offload boundary** (CPU-only today) | mpv `--hwdec-software-fallback` [T3-4]; Godot tile memory [T3-8] | Keep CPU path authoritative; design pixel buffers as swappable so a Metal interop layer can slot in later | P3 |

---

## What the research *confirmed* (not gaps — validated bets)

- **Pull model (R013 D1/D3):** Natron `renderRoI` recursion + Olive `getValueAt`
  + MoviePy `get_frame(t)` all confirm ONE recursive `pull(t,roi)`. ✅ implemented.
- **Hash cache:** Olive `FrameHashCache` (no dirty flags) + Natron `Hash64`.
  ✅ `wb_node_cache` does content-hash LRU.
- **Identity short-circuit:** VirtualDub bypass shunt + OFX `IsIdentity`.
  ✅ `wb_node_effect` op=0 passes through.
- **Proxy transparency:** Blender `seq_proxy_fetch` + MLT proxy wrapper.
  ✅ `wb_video_clip` swaps proxy↔full-res at export.
- **Keyframe valid-clamp:** Olive `KeyframeTrack`. ✅ `wb_param_track` clamps
  outside keyed range by default.
- **One param bus:** VST3/CLAP automation flags + OFX params. ✅ `wb_param_track`
  is the shared channel; only G11 (binding) remains.

---

## Recursive loop verdict

This is iteration **N**. Where we *were* (start of session): video editor was
half-wired, export was a stub, `^X`/`^D` were TODOs, no voice-polish, no
keyframes, no compositor. Where we *are* now: all of that is implemented and
**verified by a clean build + 6 test gates (214 checks, 0 failures)**.

Where we *aren't* (next loop, from G1–G12): the editor is a working
timeline+export tool but **not yet a Fusion-class node compositor with
third-party OFX effects, EDL interchange, transcript editing, or an agent API.**
Those are G4, G5, G6, G9 — the highest-leverage next steps.

**Recommendation for loop N+1:** do G11 (bind keyframe tracks to FX/automation)
first — it's the cheapest unification and makes G4 (OFX) and G6 (transcript
editing) fall out naturally, since they all ride the same param bus.
