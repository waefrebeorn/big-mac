# Big Mac Video Editing — 7-Hop Research Findings (50 Queries, 2 Rounds)

**Date:** 2026-08-26
**Method:** Kevin Bacon recursive research — each hop builds on previous findings
**Target:** Video effects, video editing, AGI-powered editing, user interface
**Rounds:** 2 × 25 queries (50 total)

---

## HOP 1: Foundations (5 queries)

### 1. Real-time video effects pipeline
**Finding:** GLSL fragment shaders process pixels in parallel via full-screen quad + fragment shader. AI Co-Artist (arxiv 2512.08951) uses LLM to generate GLSL shaders for real-time video effects. WebGL-based pipeline: vertex shader = pass-through, fragment shader = effect kernel.
**Implication for Big Mac:** Our software rasterizer (wb_rast.c) already does per-pixel processing. We can add a "shader-like" effect kernel system where each effect is a function pointer operating on RGBA pixels. No GPU needed — SIMD batch processing.

### 2. Video editing architecture
**Finding:** Node-based compositing (Nuke, DaVinci Fusion) is industry standard. Nodes export as text files. Layer-based (Premiere, FCP) is simpler for linear editing. 2026 trend: hybrid approaches.
**Implication:** Our compositor (wb_compositor.c) already has node-like architecture. We should keep layer-based timeline for editing + node-based for compositing/FX.

### 3. AI-powered video editing
**Finding:** 2026 AI editing = scene detection (PySceneDetect + DINOv2) + auto-cut + B-roll insertion + color correction. Twelve Labs Marengo/Pegasus for video understanding. LumiVideo (arxiv 2604.02409) = agentic color grading with Perception→Reasoning→Execution pipeline.
**Implication:** Our agent system (wb_agent.c) can drive video editing: "cut this scene", "add transition", "color grade this clip". Natural language → command queue → render.

### 4. Video editing UI
**Finding:** DaVinci Resolve layout: Media/Edit/Color/Fairlight/Fusion/Deliver pages. Key UI elements: timeline (tracks + clips), viewer (preview), inspector (properties), media pool. Keyboard shortcuts critical for speed (Cmd+K = add edit, etc.).
**Implication:** Our UI (wb_ui.c + Makepad) needs: multi-track timeline, clip inspector, media pool, viewer with scrubbing, keyboard shortcuts.

### 5. YUV→RGB color conversion
**Finding:** BT.709 standard: Y = 0.299R + 0.587G + 0.114B. Studio video: Y range 16-235, UV range 16-240. Conversion: 4:2:0 → 4:4:4 → RGB. FFmpeg does this efficiently.
**Implication:** Our video pipeline (wb_video.c) needs YUV→RGB for import. BT.709 matrix multiply per pixel — SIMD batchable.

---

## HOP 2: Core Effects (5 queries)

### 6. Video transitions
**Finding:** Cross-dissolve = linear interpolation of opacity over N frames. Other transitions: wipe (directional reveal), morph (optical flow warp), ripple (displacement). All are per-pixel operations: out = lerp(A, B, t) for dissolve.
**Implication:** Transition system: each transition is a function(A_frame, B_frame, t, output). SIMD batch across 4 pixels. Pre-computed transition lookup tables.

### 7. Text rendering for video
**Finding:** CoreGraphics (macOS) = CPU-based text→bitmap. Metal = GPU text rendering. Evan Wallace's technique: resolution-independent GPU text via SDF (signed distance fields). For video overlay: render text to bitmap, composite with alpha.
**Implication:** Use CoreGraphics for text→bitmap (already in wb_ui.c), then alpha-composite onto video frames. SDF for scalable text.

### 8. AI color grading
**Finding:** LumiVideo agentic system: Perception (analyze) → Reasoning (plan) → Execute (apply). Auto-color in Premiere uses Adobe Sensei. LUT application: 3D LUT = pre-computed color transform table, O(1) per pixel.
**Implication:** Agent can: analyze frame histogram → determine exposure/white balance → apply correction. 3D LUT for creative looks (load .cube files).

### 9. Timeline UI
**Finding:** Core Video (Apple) = pipeline model for video. Thumbnail generation: extract I-frames via FFmpeg. Scrubbing: show preview frame at playhead position. Waveform display: audio amplitude over time.
**Implication:** Timeline needs: clip thumbnails (extract via FFmpeg), waveform preview, playhead scrubbing, zoom/pan. All CoreGraphics-rendered.

### 10. Hardware encoding
**Finding:** VideoToolbox (macOS): h264_videotoolbox, hevc_videotoolbox. Intel Mac = H.264 only. Apple Silicon = H.264 + HEVC + ProRes. FFmpeg integration: -c:v h264_videotoolbox -b:v 4M.
**Implication:** Our export (wb_delivery.c) should use VideoToolbox for hardware encoding. FFmpeg already linked — add VideoToolbox encoder option.

---

## HOP 3: Advanced Effects (5 queries)

### 11. Video stabilization
**Finding:** Lucas-Kanade optical flow → estimate affine transform (dx, dy, da) → smooth trajectory → apply inverse transform. goodFeaturesToTrack for keypoints. estimateAffine2D for motion model.
**Implication:** Stabilization = per-frame motion estimation + compensation. Heavy for real-time. Best as offline preprocess (analyze → store transforms → apply on playback).

### 12. Chroma key (green screen)
**Finding:** Production-ready algorithm (James Fisher): WebGL shader. Key steps: 1) Convert to YUV, 2) Compute green dominance: green - max(red, blue), 3) Threshold for mask, 4) Spill suppression (remove green tint on edges), 5) Composite: out = foreground * mask + background * (1-mask).
**Implication:** Per-pixel SIMD: compute mask, suppress spill, composite. 4 pixels at a time via __m128.

### 13. Speed control + pitch correction
**Finding:** Time-stretch without pitch change: Phase vocover (FFT-based) or WSOLA (waveform similarity). Premiere "Maintain Audio Pitch" uses these. For video: frame interpolation (optical flow) or frame dropping/duplication.
**Implication:** Audio: phase vocover (FFT-based, we have wb_fft.c). Video: frame blending for slow-mo, frame dropping for speed-up.

### 14. Subtitle rendering
**Finding:** SRT = plain text + timestamps. ASS = styled subtitles (font, color, position). VLC uses libass for ASS rendering. Render to bitmap → alpha composite onto video.
**Implication:** Parse SRT/ASS → render text via CoreGraphics → alpha composite. Support: font, size, color, position, outline, shadow.

### 15. Keyframe animation
**Finding:** Bezier interpolation for smooth motion. Easing curves: linear, ease-in, ease-out, ease-in-out. Keyframe types: linear, bezier, hold. After Effects: separate spatial + temporal interpolation.
**Implication:** Keyframe system: time→value mapping with interpolation. Bezier handles for easing. Properties: position, scale, rotation, opacity, volume, effect parameters.

---

## HOP 4: AGI + Infrastructure (5 queries)

### 16. LLM agent video editing
**Finding:** (No direct results — emerging field.) Pattern: natural language → structured command → execute. "Cut this scene" → find scene boundary → split clip → remove. "Add transition" → find cut point → insert transition.
**Implication:** Our agent (wb_agent.c) already has command queue. Add video commands: cut, trim, transition, effect, color, export. Agent parses natural language → video edit commands.

### 17. Automatic scene detection
**Finding:** PySceneDetect: content-aware scene detection via pixel difference threshold. DINOv2 encoder for semantic understanding. Keyframe extraction: color histogram clustering, mean/kurtosis comparison.
**Implication:** Scene detection: compute frame-to-frame difference (histogram or pixel). Threshold → cut points. Agent can auto-detect scenes for rough cut.

### 18. Proxy editing
**Finding:** DaVinci: ProRes proxies at lower resolution. FCP: "Better Performance" = reduced resolution playback. Proxy = low-res copy for editing, full-res for export.
**Implication:** Generate proxy (downscaled) videos for timeline playback. Switch to full-res for export. Downscale via bilinear/bicubic interpolation.

### 19. Multi-track compositing
**Finding:** Alpha compositing: out = src * src_alpha + dst * (1 - src_alpha). Porter-Duff operators: over, in, out, atop. Layer blend modes: normal, multiply, screen, overlay.
**Impositor:** Per-pixel alpha blend. SIMD: 4 pixels at a time. Blend modes = different arithmetic on RGBA channels.

### 20. Video effects chain
**Finding:** Post-processing pipeline: blur → sharpen → color grade → vignette. Each effect = pass over pixel buffer. Unity URP: chain of full-screen passes. ART: linear RGB color space for all operations.
**Implication:** Effects chain: ordered list of effect functions. Each takes RGBA buffer → modifies in-place or outputs new buffer. SIMD per effect.

---

## HOP 5: Professional Features (5 queries)

### 21. Noise reduction
**Finding:** Temporal NR: compare current frame with previous (motion-compensated). Spatial NR: blur within frame (bilateral filter preserves edges). Order: temporal first, then spatial. NVIDIA VPI has hardware-accelerated TNR.
**Implication:** Temporal NR: maintain frame history, compute weighted average with motion mask. Spatial NR: 3x3 or 5x5 bilateral filter. Both SIMD-friendly.

### 22. Video scopes
**Finding:** Waveform: brightness (Y) over image position. Vectorscope: color (UV) on chromaticity diagram. Histogram: pixel count per brightness level. RGB Parade: separate R/G/B histograms.
**Implication:** Compute per-frame: histogram (256 bins), waveform (column averages), vectorscope (UV scatter). Render as CoreGraphics overlays.

### 23. AI voice isolation
**Finding:** iZotope RX 12 Dialogue Isolate: AI stem separation. Adobe Podcast Enhanced Speech: noise removal. ElevenLabs Voice Isolator: neural audio model. All use FFT → mask → iFFT pipeline.
**Implication:** Our FFT (wb_fft.c) can do spectral processing. Simple voice isolation: spectral gate (attenuate non-voice frequencies). Full AI model too heavy for real-time.

### 24. Hardware encoding pipeline
**Finding:** VideoToolbox: low-level framework for hardware encode/decode. FFmpeg integration: -c:v h264_videotoolbox. Supports H.264 (all Macs) + HEVC (Apple Silicon).
**Implication:** wb_delivery.c already uses FFmpeg. Add VideoToolbox encoder path for fast export. Fall back to software if hardware unavailable.

### 25. Undo/redo system
**Finding:** Command pattern: encapsulate each edit as command object with do/undo. History stack: push on execute, pop on undo. Redo stack: push on undo, pop on redo.
**Implication:** Our command queue (wb_cmd.c) already SPSC. Add: command stack with do/undo, redo stack, history limit (100 commands). Each video edit = command.

---

## HOP 6: Polish & Export (5 queries)

### 26. Project file format
**Finding:** JSON = human-readable, editable, slow. Binary = fast, compact, not editable. Hybrid: JSON for project structure, binary for media references.
**Implication:** Our .wbus format already JSON-like. Keep human-readable project files. Reference media by path (not embedded).

### 27. Thumbnail generation
**Finding:** FFmpeg: -vf "select=eq(pict_type\,PICT_TYPE_I)" for I-frame extraction. -vf "thumbnail" for representative frames. -vf "scale=160:-1" for size.
**Implication:** Use FFmpeg to extract thumbnails at regular intervals. Cache as small RGBA bitmaps for timeline display.

### 28. Aspect ratio / crop
**Finding:** Letterbox: add black bars top/bottom. Pillarbox: add black bars left/right. Crop: remove edges. Scale: resize to fit. All = simple pixel coordinate transforms.
**Implication:** Per-pixel coordinate transform. SIMD: compute source (x,y) for each output pixel. Bilinear interpolation for quality.

### 29. Fade in/out
**Finding:** Opacity keyframe animation. Fade in: opacity 0→1 over N frames. Fade out: opacity 1→0. Audio fade: volume 0→1 or 1→0.
**Implication:** Keyframe system handles this. Opacity = multiply RGBA by scalar. Volume = multiply audio samples by scalar.

### 30. Keyboard shortcuts
**Finding:** Premiere: Cmd+K (add edit), Cmd+D (video transition), Shift+D (both), J/K/L (shuttle), I/O (in/out points), ; (lift), ' (extract).
**Implication:** Implement standard shortcuts: space (play/pause), JKL (shuttle), I/O (in/out), Cmd+K (cut), Cmd+T (transition), delete (ripple delete).

---

## HOP 7: AGI Integration (5 queries)

### 31. Agent-driven editing workflow
**Finding:** Pattern: User speaks → LLM parses intent → generates edit decision list (EDL) → executes on timeline. "Make a highlight reel" → detect scenes → score by motion/audio → select top N → assemble.
**Implication:** Our agent already has cgi-* commands. Add video commands: video-cut, video-transition, video-effect, video-color, video-export. Agent = natural language frontend.

### 32. Auto-assembly editing
**Finding:** "Rough cut" = assemble clips in order, remove silence/pauses. "Smart B-roll" = detect talking head → insert relevant footage at pauses. "Jump cut removal" = detect silence → remove.
**Implication:** Silence detection: audio RMS below threshold → mark for removal. Scene detection: frame difference → cut points. Agent assembles timeline automatically.

### 33. Semantic video search
**Finding:** Twelve Labs Marengo: natural language → video segment retrieval. "Find the part where the dog runs" → embedding search → timestamp.
**Implication:** Offline: transcribe audio (whisper.cpp), index text. Search: text query → timestamp → seek. Future: visual embedding search.

### 34. AI-generated transitions
**Finding:** Style transfer for transitions: neural network generates intermediate frames. Morph: optical flow warp between scenes. Generative fill: AI extends frame boundaries for reframing.
**Implication:** Start with classic transitions (dissolve, wipe, slide). Future: optical flow morph (heavy), generative transitions (requires model).

### 35. Real-time preview
**Finding:** DaVinci: proxy mode + smart cache. FCP: background rendering. Premiere: proxy workflow. All: render effects at low resolution for preview, full for export.
**Implication:** Preview = low-res (1/2 or 1/4) + skip heavy effects. Export = full res + all effects. Toggle: "Draft" vs "Full" quality.

---

## SUMMARY: Priority Build List

### Tier 1: Core Video Editing (must have)
1. **Multi-track timeline UI** — clips, tracks, scrubbing, zoom
2. **Clip operations** — cut, trim, move, copy, delete, ripple
3. **Transitions** — dissolve, wipe, slide (SIMD batch)
4. **Text overlay** — CoreGraphics → bitmap → alpha composite
5. **Keyframe animation** — bezier interpolation, easing curves
6. **Project save/load** — JSON format, media references
7. **Undo/redo** — command pattern, history stack

### Tier 2: Effects & Quality
8. **Color correction** — exposure, contrast, white balance, LUT
9. **Chroma key** — green screen, spill suppression (SIMD)
10. **Video scopes** — waveform, vectorscope, histogram
11. **Noise reduction** — temporal + spatial (SIMD)
12. **Speed control** — time-stretch, pitch preserve
13. **Stabilization** — optical flow (offline preprocess)

### Tier 3: AGI-Powered Features
14. **Agent commands** — natural language → video edit
15. **Scene detection** — auto-cut on scene changes
16. **Auto-assembly** — rough cut from raw footage
17. **Smart B-roll** — detect pauses → insert footage
18. **Semantic search** — find video segments by description

### Tier 4: Professional Polish
19. **Proxy editing** — low-res preview, full-res export
20. **Hardware encoding** — VideoToolbox via FFmpeg
21. **Subtitle support** — SRT/ASS import, render, burn-in
22. **Keyboard shortcuts** — professional editing workflow
23. **Thumbnail generation** — FFmpeg I-frame extraction
24. **Aspect ratio** — crop, letterbox, pillarbox
25. **Voice isolation** — spectral gating (simple) or AI model

---

## Key Technical Decisions

1. **Software rendering** — no GPU dependency, SIMD batch processing
2. **Effect chain** — ordered list of per-pixel functions, each SIMD-optimized
3. **Agent-driven** — natural language → command queue → render
4. **Proxy workflow** — low-res preview, full-res export
7. **FFmpeg integration** — decode, thumbnail, hardware encode
8. **CoreGraphics** — text rendering, UI, scopes display
9. **Command pattern** — undo/redo for all edit operations

---

## ROUND 2: Deep Dive (25 more queries)

### HOP 8: Editing Tools (5 queries)

#### 36. Trim/Ripple/Slide/Slip/Roll
**Finding:** 5 edit tools: Ripple (trim one side, shift all following), Roll (trim one side, extend other — total duration unchanged), Slip (change in/out points without moving clip), Slide (move clip while trimming adjacent clips), Trim (drag edit point). All are timeline data structure operations.
**Implication:** Timeline needs: clip->{track, in_point, out_point, duration, media_offset}. Each tool = function that modifies these values + cascades to neighbors.

#### 37. 3D LUT (.cube) parser
**Finding:** .cube format: header (TITLE, DOMAIN_MIN, DOMAIN_MAX, LUT_3D_SIZE), then N³ lines of R G B floats. Size typically 17³ or 33³. Trilinear interpolation between lattice points for application.
**Implication:** Parse .cube → allocate 33×33×33×3 float array. Apply per pixel: trilinear lookup. SIMD: interpolate 4 pixels at once.

#### 38. Audio waveform peaks
**Finding:** FFmpeg: `showwavespic` filter renders waveform image. `audiowaveform` (BBC) generates .dat peak files: min/max pairs per pixel column. Peak file = array of (min, max) per time bucket.
**Implication:** Generate peak file on import: decode audio → compute min/max per bucket (e.g., 2 buckets per pixel). Store as int8 array. Render as filled polygon.

#### 39. Blend modes (Porter-Duff)
**Finding:** W3C Compositing spec defines: normal, multiply, screen, overlay, darken, lighten, color-dodge, color-burn, hard-light, soft-light, difference, exclusion. Formulas per channel. Porter-Duff: over, in, out, atop, xor, plus.
**Implication:** Each blend mode = function(base, blend → out). SIMD: 4 pixels at a time. Pre-compute lookup tables for expensive ones (overlay, soft-light).

#### 40. Frame interpolation
**Finding:** FILM (ECCV 2022): bi-directional motion estimation → synthesize intermediate frames. FC-VFI (2026): faithful + consistent. Flow estimation → warp both frames → blend. Too heavy for real-time on dual-core.
**Implication:** Offline only. Use FFmpeg's `minterpolate` filter for proxy generation. Future: custom optical flow (block matching, simpler than deep learning).

### HOP 9: UI & Workflow (5 queries)

#### 41. Title/Lower Third templates
**Finding:** Lower third = text overlay in lower 1/3 of screen. Components: background bar, name text, title/description text, optional logo. Animated: slide in, hold, slide out. Templates = pre-designed with editable text.
**Implication:** Title system: text layers + background shapes + animation keyframes. Templates = JSON with placeholder text. Render via CoreGraphics → bitmap → composite.

#### 42. Markers/Chapter points
**Finding:** Markers = timestamped annotations on timeline. YouTube chapters: "00:00 Intro" format in description or embedded. DaVinci: markers with color, name, duration. Premiere: markers with comments, duration, chapter info.
**Implication:** Marker = {time, name, color, duration}. Store in project file. Render as triangle/pin on timeline ruler. Export as chapter file or embed in MP4.

#### 43. Color curves (HSL)
**Finding:** Hue vs Saturation, Hue vs Hue, Hue vs Luma, Sat vs Sat, Lum vs Sat curves. Bezier-interpolated control points. Selective color: pick hue range → adjust H/S/L independently.
**Implication:** Curve = array of (x,y) control points with bezier interpolation. Build lookup table (256 or 1024 entries) from curve. Apply per pixel: LUT[pixel_value] → new_value.

#### 44. Vignette effect
**Finding:** Radial gradient: center = full brightness, edges = darkened. Formula: factor = 1 - smoothstep(inner_radius, outer_radius, distance_from_center). Multiply pixel by factor.
**Implication:** Pre-compute vignette map (2D array of float factors). Per pixel: multiply RGB by map[y][x]. SIMD: 4 pixels at once. Adjustable: center, inner radius, outer radius, strength.

#### 45. Unsharp mask
**Finding:** Algorithm: 1) Blur original (Gaussian 3×3 or 5×5), 2) Subtract blurred from original (detail = original - blurred), 3) Add detail back scaled by amount: sharpened = original + amount × detail. Kernel: center = 1+amount, neighbors = -amount/8 (for 3×3).
**Implication:** 3×3 convolution kernel. SIMD: process 4 pixels at once. Separable Gaussian for larger radii (two passes: horizontal then vertical).

### HOP 10: Infrastructure (5 queries)

#### 46. Snapping/Magnetic timeline
**Finding:** Snap: when dragging clip near another clip's edge/marker/playhead, jump to align. Magnetic timeline (FCP): no gaps between clips, deleting auto-closes space. Snap targets: clip edges, markers, playhead, keyframes.
**Implication:** Snap: check distance to all snap targets within threshold (e.g., 5 pixels). If within, snap to closest. Magnetic: on delete/trim, shift all following clips to fill gap.

#### 47. Background export/render queue
**Finding:** DaVinci: background render. AE: render queue with multiple outputs. Premiere: Media Encoder integration. Pattern: render job → queue → pthread → progress callback → completion.
**Implication:** Render job = {project, output_path, format, range}. Queue = array of jobs. pthread per job. Progress: frames rendered / total frames. Cancel: set flag, check in render loop.

#### 48. Safe area overlays
**Finding:** Title-safe: inner 80% of frame. Action-safe: inner 90%. For vertical video (9:16): safe zones account for platform UI (Instagram, TikTok). Overlay = semi-transparent rectangles showing safe zones.
**Implication:** Render as CoreGraphics overlay on viewer. Configurable: 80%/90% or custom. Rule-of-thirds grid: 2 vertical + 2 horizontal lines.

#### 49. SMPTE timecode
**Finding:** Format: hh:mm:ss:ff (NDF) or hh:mm:ss;ff (DF). Drop frame: skip frame numbers 0 and 1 at each minute except every 10th minute. 29.97fps DF compensates for 0.1% speed difference. Conversion: frame_count → TC requires DF adjustment.
**Implication:** Timecode = {hours, minutes, seconds, frames, fps, drop_frame}. frame_count → TC: if DF, compute skipped frames. TC → frame_count: reverse. Display: format string with ; or : separator.

#### 50. Multi-camera editing
**Finding:** Sync multiple angles by: timecode, audio waveform, or manual. Group into "multicam clip". Switch angles during playback by pressing 1-4. Premiere: nested sequence. DaVinci: multicam viewer.
**Implication:** Multicam clip = array of source clips + sync offsets. Active angle = index. Render: pick frame from active angle's source. Switch: change active angle at edit point.

---

## COMPLETE 50-QUERY RESEARCH SUMMARY

### All 25 New Findings (Round 2)

| # | Topic | Key Insight | Implementation |
|---|-------|-------------|----------------|
| 36 | Edit tools | 5 trim modes = timeline data ops | clip struct + cascade functions |
| 37 | 3D LUT | .cube = N³ RGB lattice + trilinear | parse → 33³ float array → LUT |
| 38 | Waveform | Peak file = min/max per time bucket | int8 array, render as polygon |
| 39 | Blend modes | W3C formulas per channel | SIMD function per mode |
| 40 | Frame interp | Optical flow → warp → blend | FFmpeg minterpolate (offline) |
| 41 | Titles | Text + shape + animation layers | JSON templates + CoreGraphics |
| 42 | Markers | Timestamped annotations | {time, name, color} struct |
| 43 | Curves | Bezier control points → LUT | 256/1024-entry LUT per curve |
| 44 | Vignette | Radial gradient factor map | Pre-computed 2D float array |
| 45 | Sharpen | Unsharp mask = original + amount×(orig-blurred) | 3×3 convolution, SIMD |
| 46 | Snapping | Snap to edges/markers/playhead | Distance check + threshold |
| 47 | Render queue | pthread + progress callback | Job array + worker thread |
| 48 | Safe areas | 80%/90% frame guides | CoreGraphics overlay |
| 49 | Timecode | SMPTE DF/NDF conversion | Frame count ↔ TC with skip logic |
| 50 | Multicam | Sync by audio/TC, switch angles | Angle array + active index |

### Updated Priority Build List (50 items total)

**Tier 1: Core Editing (1-10)**
1. Multi-track timeline UI
2. Clip operations (cut/trim/move/copy/delete)
3. Ripple/Roll/Slip/Slide/Trim tools
4. Snapping system
5. Transitions (dissolve/wipe/slide)
6. Text overlay / titles
7. Keyframe animation (bezier)
8. Project save/load (JSON)
9. Undo/redo (command pattern)
10. Markers/chapter points

**Tier 2: Effects & Color (11-20)**
11. Color correction (exposure/contrast/white balance)
12. 3D LUT loader (.cube parser)
13. Color curves (HSL)
14. Vignette
15. Sharpen (unsharp mask)
16. Blend modes (10+ modes)
17. Chroma key (green screen)
18. Noise reduction (temporal + spatial)
19. Video scopes (waveform/vectorscope/histogram)
20. Safe area overlays

**Tier 3: Audio & Sync (21-30)**
21. Audio waveform display (peak files)
22. Auto-ducking (sidechain)
23. Speed control + pitch preserve
24. Voice isolation (spectral gate)
25. Multi-camera editing
26. Subtitle support (SRT/ASS)
27. Timecode display (SMPTE DF/NDF)
28. Audio/video sync offset
29. Silence detection (for auto-cut)
30. Scene detection (frame difference)

**Tier 4: AGI & Automation (31-40)**
31. Agent commands (natural language → edit)
32. Auto-assembly (rough cut)
33. Smart B-roll insertion
34. Semantic search (transcript-based)
35. Auto color correction
36. Auto audio cleanup
37. Template-based editing
38. Batch export
39. Project templates
40. Smart reframing (auto crop to aspect)

**Tier 5: Professional Polish (41-50)**
41. Proxy editing (low-res preview)
42. Background render queue
43. Hardware encoding (VideoToolbox)
44. Frame interpolation (slow-mo)
45. Stabilization (optical flow)
46. Keyboard shortcuts
47. Thumbnail generation
48. Aspect ratio transforms
49. Lower third templates
50. Multi-format export
