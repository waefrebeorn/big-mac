# R060 — Agent-First Video Workflow: Shadow Bin, AV Queuing, GUI Sweep
## 7-hop research digest (25+ queries, 8 domains)

## Sources consulted
1. Wikipedia — Edit decision list (EDL/autoconform history)
2. cutconvert.com — CMX3600 EDL explained
3. workflow.frame.io — Common conform issues (8-char name truncation trap)
4. OTIO file-format spec (opentimelineio.readthedocs.io) — Timeline/Stack/Track/Clip schema, RationalTime, metadata bags
5. Premiere version notes — OTIO import/export now standard
6. GSoC 2026 — OpenTimelineIO support in Blender VSE (schema mapping: clip = ExternalReference in Track kind=Audio/Video)
7. frame.io codec guide — proxy/offline-edit + mezzanine concepts
8. thepostflow.com — YouTube export recipes (H.264 High, Rec709 tag)
9. davinciresolve21.com — Resolve export sheets ("Use Proxy Media" render flag)
10. Adobe Premiere docs — auto-ducking (sensitivity/duck amount/fade duration/position), Essential Sound tagging
11. EBU R128 v5 (2023) — -23 LUFS broadcast; streaming uses -14 LUFS convention
12. ffmpeg loudnorm two-pass recipe (print_format=json → measured_* params)
13. oximedia-metering docs — K-weighting pipeline: high-shelf+RLB highpass → 400ms momentary / 3s short-term / gated integrated; LRA p95-p10
14. PySceneDetect — AdaptiveDetector, split_video_ffmpeg, content-detect thresholding
15. ffmpeg scdet/select=gt(scene,t) + metadata=print:file — shot-change timecodes
16. lossless-cut issue #126 — SMART CUT: re-encode only cut-point→next-keyframe, stream-copy the rest
17. videohelp/smart-rendering threads — cuts on arbitrary frames require partial re-encode
18. SonicScoop + mykaraoke.video — A/V sync root causes: VFR footage, sample-rate mismatch, telecine pulldown drift
19. dev.to "You Don't Know Undo/Redo" — Command vs Memento vs branching state-graph; scope of undo; redo-loss anxiety
20. HN thread on undo frameworks — serialize command stack for crash recovery
21. audacity forum + dsp.SX — waveform LOD pyramid: min/max per block (256→64K), log2 zoom-level selection
22. supermegaultragroovy — overview dataset for max zoom kept on heap
23. assistents.ai + foximusic — 7-stage YouTube automation pipeline (research→script→voice→visual→thumbnail→metadata→publish); specialized agents per stage
24. zernio/postproxy — YouTube Data API: videos.insert (quota 1 unit), thumbnails.set, OAuth scope youtube.upload, resumable uploads, retriable 500/502/503/504 backoff
25. loopstack/langchain docs — tool design: actionable errors naming missing/misnamed args; structured JSON results; idempotent writes
26. wireflow/coddykit — headless/API-first editors trend (OpenCut 75k stars)
27. StoryBlender arxiv — storyboard JSON schemas (scene_id/shot_id/camera_instruction)
28. storyflow/ciaro — animatic = timed storyboard + scratch audio

## Triple-devil's-advocate on our current workflow

### Advocate 1: "The agent flow is brittle"
- Agent commands are fire-and-forget strings; no session state query ("what's in my scene?"), no undo of agent actions, no idempotency.
- Errors return -1 with no reason. An LLM can't self-correct from "-1".
- No way to inspect a scene non-destructively before mutating it.

### Advocate 2: "The GUI doesn't match the new engine"
- CGI tier renders a demo cube; there is no UI for cgi scenes, keyframes, or the asset library.
- No JKL scrubbing, no I/O points, no ripple tool keybinds in the DAW UI.
- Audio waveforms draw from raw samples each frame — no LOD cache.

### Advocate 3: "Interchange is half-built"
- We export EDL/FCPXML but have no project sidecar that a big NLE could read AND write back — no round-trip.
- No proxy generation tied to clips (R009 planned proxies but they're per-import only).
- Loudness normalization exists (two-pass polish) but isn't wired into the EXPORT path as a delivery preset.
- No chapter/marker export to YouTube description format.

## The plan (build order this round)
| # | Feature | Closes |
|---|---|---|
| 1 | **Shadow bin** (`wb_shadowbin.c`): JSON sidecar per project — full edit state (clips, trims, fades, lanes, markers, CGI scene refs) in OTIO-compatible shape; write atomic (tmp+rename); read-back restores | interchange |
| 2 | **Agent introspection commands**: `cgi-state`, `cgi-clear`, `agent-undo` via session snapshot, structured error strings `ERR:<code>:<human>` | agent flow |
| 3 | **Auto-duck** (`wb_duck.c`): envelope-follow music under voice using existing comp/gate DSP; writes automation lane | AV queuing |
| 4 | **Delivery presets**: loudnorm two-pass wired into export; YouTube preset (-14 LUFS, Rec709 tag, faststart); chapters from markers → description text | delivery |
| 5 | **Waveform LOD cache** (`wb_wavcache.c`): min/max pyramid per clip, drawn in arrangement instead of raw scan | GUI perf |
| 6 | **JKL + I/O keys** in wb_daw: J/K/L variable-speed scrub, I/O set, GOTO start/end | GUI feel |
