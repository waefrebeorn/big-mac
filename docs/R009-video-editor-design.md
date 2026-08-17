# R009 — Video editor implementation design (FFmpeg + our audio engine)

**Builds on:** R008 (7-hop convergence: Vegas-style assembly editor on FFmpeg
+ our audio engine; 480p proxy editing; auto-captions via FFmpeg Whisper;
1080p60 export). **Hard constraints:** C11 only, zero 3rd-party libs besides
FFmpeg (which is already installed — `which ffmpeg`); integrate via the C API
(libavformat/libavcodec/libswscale), not CLI subprocesses where possible.
Verified by building + a headless smoke (decode a clip, render a frame, encode
an export).

## 1. Target MVP feature set (CapCut/Clipchamp minimal, per R008 §C)
- [ ] Import video/audio files (ffmpeg demux → extract stream info)
- [ ] Timeline with **video tracks** (above) + **audio tracks** (our engine)
- [ ] Clip trim, split, reorder on timeline
- [ ] B-roll: add a second video track layered over the primary
- [ ] Audio: reuse our existing synth/comp/eq/transport on the audio tracks
- [ ] Auto-captions: ffmpeg whisper filter → SRT → overlay on export
- [ ] Export: 1080p60 mp4 (H.264 + AAC), swapping back to full-res sources
- [ ] 480p proxy generated at import for smooth editing on this Mac

## 2. Architecture (Asset → Clip → Track → Timeline, R008 §D)

### 2.1 Session extension
Keep `wb_session` / `wb_track` / `wb_clip` — add a `kind` field:
- `kind=audio` (existing): handled by our engine
- `kind=video`: handled by FFmpeg; clip stores `source_path`,
  `proxy_path` (480p), `start_in_source` (ffmpeg timestamp), `duration`,
  `timeline_pos` (same units as audio: samples @ 44100, or seconds — decide).

**Decision:** Use **seconds** for video timeline positions (ffmpeg's natural
unit), and convert to/from sample positions at the audio/video boundary. The
transport's timebase stays sample-based for audio; video tracks read their
current frame from the timeline position converted to seconds.

### 2.2 Video clip struct (new, in wbus)
```c
typedef struct wb_video_clip {
    char source_path[512];   /* original full-res file */
    char proxy_path[512];    /* 480p proxy (mp4/h264) */
    double start_in_source;  /* seconds into the source to start playing */
    double duration;         /* seconds to play from source */
    double timeline_pos;     /* seconds on the timeline where this clip starts */
} wb_video_clip;
```

### 2.3 Video track
A track with `kind=video` carries an array of `wb_video_clip` (same pattern as
audio clips/notes). Multiple video tracks = layers (B-roll on top).

### 2.4 Render model
- **Preview (SDL2):** for each visible video track, find the clip active at the
  current timeline position, seek ffmpeg to `start_in_source + (now - timeline_pos)`,
  decode one frame, scale to preview rect via libswscale, blit to SDL2 texture.
- **Export (ffmpeg):** build an ffmpeg filter graph: for each video track, an
  input + trim filter + overlay (for layering); audio inputs from our rendered
  buffer (or from ffmpeg-decoded source audio). Encode to H.264/AAC 1080p60.

## 3. FFmpeg integration points (owned C API, R008 §B)

### 3.1 Decode a video frame (preview)
Use `libavformat` (avformat_open_input, avformat_find_stream_info,
av_find_best_stream, av_read_frame) + `libavcodec` (avcodec_send_packet,
avcodec_receive_frame) + `libswscale` (sws_scale to SDL2 pixel format).
Wrap in a `wb_video_decoder` struct that holds AVFormatContext, AVCodecContext,
SwsContext, and the current seek position.

### 3.2 Generate 480p proxy at import
```c
int wb_video_make_proxy(const char *src, const char *proxy);
```
Implemented via a **private ffmpeg CLI call** (simplest correct path) —
`ffmpeg -i src -vf scale=854:480 -c:v libx264 -preset fast -crf 23 -c:a aac
proxy.mp4`. One CLI call per import, non-blocking via background process or
deferred. We already ship ffmpeg binary; no extra compile.

### 3.3 Auto-captions (export time)
```c
int wb_video_captions_generate(const char *audio_or_video_path, const char *srt_out);
```
Implemented as: `ffmpeg -i path -vn -af "whisper=model=<model>:language=en:
destination=srt_out:format=srt" -f null -`. Then overlay on export via
`-vf subtitles=srt_out`. **Model file:** ship a small whisper.cpp model
(e.g. base.en) in `third_party/` or rely on system ffmpeg 8.0+ with built-in
whisper support. Decision: check what `ffmpeg -filters | grep whisper` says on
this Mac first.

### 3.4 Export (full-res)
Build the filter graph programmatically (libavfilter) OR via a single ffmpeg CLI
call that takes the timeline as input spec. **Decision for MVP:** CLI call is
correct and simplest — we already have the exact ffmpeg command line; building a
full libavfilter graph is a LOT of code for the same result. Use CLI for export,
C API for preview decoding.

## 4. UI (SDL2, one surface, R008 §F)

### 4.1 Layout
- Transport bar (reuse existing): play/stop/seek/BPM→timecode, now showing
  timeline position in seconds.
- **Video preview** (top, large): SDL2 texture showing the composited current
  frame (all video tracks layered).
- **Timeline** (below preview): horizontal tracks, each a row of clips as
  rectangles (width = duration × pixels-per-second). Video tracks labeled
  "V1", "V2"; audio tracks as before.
- **Mixer/audio panel** (right): reuse existing mixer for audio tracks.

### 4.2 Interaction
- Click on timeline → seek to that position.
- Drag a clip rectangle → move its `timeline_pos`.
- Split at cursor → divide a clip into two (insert a new clip, adjust durations).
- Import → file dialog (or drag-drop if SDL2 supports) → generate proxy → add
  clip to V1.

### 4.3 Tabs
Reuse the KEYS/PAD/STEP/SESSION tab concept? For video: maybe **VIDEO / AUDIO /
CAPTIONS / EXPORT** tabs. Keep minimal — the user said "simplest of the simplest."

## 5. Verification plan (R008 §6)
- `tests/test_video_ffmpeg.c`: headless — open a test mp4, decode one frame,
  check dimensions match, check proxy generation (compare durations).
- `make` clean build (FFmpeg linking added to Makefile).
- Preview smoke: open a 1080p clip, seek to 0, decode frame 0, blit — no crash.
- Export smoke: assemble a 2-clip timeline, export 1080p60, verify output file
  plays and duration matches.

## 6. Build order (heavy-first)
1. Add FFmpeg C API link to Makefile (verify `ffmpeg` version + `whisper` filter
   present on this Mac).
2. `wb_video_decoder` + decode-one-frame test.
3. `wb_video_make_proxy` (CLI) + proxy test.
4. Timeline data model (video clip/track structs, session extension).
5. Preview: SDL2 texture from decoded frame, blit in render loop.
6. Timeline UI (clip rectangles, drag, split).
7. Import UI + proxy generation on import.
8. Auto-captions: whisper filter → SRT → overlay on export.
9. Export CLI (full-res 1080p60).
10. Screenshot smoke per view; commit.

## 7. Key decisions
- **CLI for proxy + export; C API for preview decode.** Right balance of effort
  vs. correctness.
- **Seconds for video timeline; samples for audio.** Convert at boundary.
- **Reuses audio engine fully** — the video editor is "Vegas": video on top of
  a working audio DAW, not a rewrite.
- **480p proxy on this Mac** (i5-4260U, 8GB) — 1080p60 export only at the end.
- **FFmpeg whisper filter** for captions — no custom ML compile.
