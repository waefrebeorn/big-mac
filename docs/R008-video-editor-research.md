# R008 — Video editor pivot: FFmpeg-backed assembly editor (7-hop)

**Date:** 2026-08-16
**Scope:** Sony Vegas / CapCut-style video editor built on Big Mac's audio engine.
*Target:* 1080p60 export, **480p proxy editing**, automatic captions, no 3rd-party
compile burden.

## 7-hop Kevin Bacon research — ≥7 sources, ≥3 domains, ONE convergent truth

### Domain A — Historical model (Sony Vegas origin)
1. Sound Forge began 1992 as $25 shareware; Vegas was built as a multitrack
   audio editor that later gained a video track — "originally a multi-tracked
   soundforge, when Vegas came out it just had video added on top of the audio
   engine" (community/foundry history + orangutan YouTube history recap).
2. **Convergent lesson:** Vegas succeeded because the audio editor was already
   solid and the video track was *non-destructive* on top — the video was a
   layer over a working audio timeline, not a rewrite.

### Domain B — FFmpeg as the codec/container backbone (owned, no compile needed)
3. FFmpeg/libav: libavcodec = decode/encode, libavformat = demux/mux; the
   C API is what every desktop editor uses under the hood (ffmpeg.org docs,
   leandromoreira/ffmpeg-lib-tutorial, trac.ffmpeg.org/wiki/Using-libav).
4. **FFmpeg 8.0 ships an OpenAI Whisper filter** — automatic speech-to-text
   / captions from a single ffmpeg command, writing SRT/VTT/JSON; optional GPU
   accel; needs whisper.cpp library present at build time (phoronix.com,
   ffmpeg.org filter docs, medium vpalmisano tutorial). **This is the captions
   feature for free.**
5. **FFmpeg already installed on this Mac** (`ffmpeg` binary present) — so we
   have decode/encode/SRT/subtitle overlay today with zero compile.

### Domain C — CapCut / Clipchamp minimal feature set (the target UX)
6. CapCut core: drag clips to timeline, trim/split/rearrange, add text/filters/
   transitions/effects/BGM, **auto-captions in 10+ languages**, animated text,
   exporting (capcut.com beginner tutorial, Content Creators in-depth tutorial).
7. Clipchamp (Microsoft): trim, split, merge, stock library, basic text/audio;
   positioned as "simple assembly editor" (ngram CapCut-vs-Clipchamp, swellai).
8. **Convergent lesson:** minimal viable editor = clip import + timeline trim/
   split + B-roll + audio + **auto-captions** + export. Everything else (color
   grading, keyframing, AutoCut) is upside we can add later.

### Domain D — Timeline data model (architecture)
9. Standard model: **Asset → Clip → Track → Timeline** (videodb.io
   timeline-architecture docs, img.ly mobile timeline design). Clip wraps an
   asset + in/out/duration/offset; track sequences clips; timeline is the canvas.
10. For a C11 editor: each "clip" is a (source_path, start_in_source, duration,
    timeline_pos) tuple; the render pass walks tracks in time order and
    decodes/renders the visible slice. **This mirrors Big Mac's session/clip/
    track model exactly.**

### Domain E — Proxy editing (480p edit, 1080p export)
11. Industry standard: edit on low-res proxy, export full-res masters — Premiere
    ingest/proxy workflow, Kdenlive 4K multi-track with proxy editing
    (helpx.adobe.com, opensourcealternatives.to).
12. **Simplest implementation:** at import, use ffmpeg to create a 480p proxy
    beside the source; edit references proxy paths; on export, ffmpeg switches
    back to the original. The proxy is just another file path in the clip tuple.

### Domain F — Why beginners struggle with DAWs/Launchpads (carryover lesson)
13. HCI cognitive-load theory: extraneous load from physically separated menus
    kills learnability (HCI source from R006). One surface, visible state, no
    menu-diving. Carry this into the video editor: one timeline, tabs minimal,
    color = state.

### Domain G — Open-source editor SDK landscape (integration options)
14. Open-source video editor SDKs 2025/2026 roundup (img.ly): FFmpeg (codec/
    container, no GUI), MLT/OpenShot library (C++/Python timeline), CE.SDK
    (full GUI, LGPL). **We own the GUI (SDL2) and the audio engine; FFmpeg is
    the only external lib we need for the video side.**

## Convergent truth (one sentence)
> **The right move is a Sony-Vegas-style assembly editor layered on Big Mac's
> existing audio engine + FFmpeg for video decode/encode/captions: a single
> timeline with video tracks (480p proxy) over audio tracks, clip trim/split/
> B-roll, auto-captions via the FFmpeg 8.0 Whisper filter (no compilation of
> any ML stack), and 1080p60 export that swaps back to full-res sources. We do
> NOT build a from-scratch video codec or a web-editor — we integrate FFmpeg
> as the video backbone and keep the audio engine we already own.**

## Implications / decisions
- **VIDEO = FFmpeg, AUDIO = our engine.** The render pass: for each visible
  video track, ffmpeg decodes the proxy (480p) into an SDL2 texture for the
  preview; on export, ffmpeg encodes the timeline from the full-res sources.
  Audio keeps going through our DSP chain (synth/comp/eq/etc.) — the video
  editor inherits the whole audio feature set for free.
- **Captions = FFmpeg whisper filter + SRT overlay.** One ffmpeg call per clip
  produces an SRT; overlay via `-vf subtitles=`. No whisper.cpp compile (we
  ship a prebuilt model if needed, or rely on system ffmpeg 8.0+ with
  --enable-whisper).
- **Proxy = 480p mp4 beside source at import.** Edit stays fast on this Mac
  (i5-4260U, 8GB).
- **Timecode model = same as audio session:** sample-accurate timeline, just
  with a video track layer. Reuse `wb_session`/`wb_track`/`wb_clip` schema
  with a `kind=video` track type.

## Concrete next steps (design doc → code)
See `docs/R009-video-editor-design.md` for the implementation spec.
