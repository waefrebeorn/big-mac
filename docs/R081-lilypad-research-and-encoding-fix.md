# R081 — YTP Pipeline Overhaul + Lily Pad Research

## Problem Statement (User Feedback)
- All existing YTPs are random slowdowns/speedups/rehearsals with one random blast section
- Edit pattern is identical between all of them — no plot, no recognition, no context
- Videos are 360p (not 480p), 10MB+ for 18-second clips — massive waste
- Need: context-aware editing, proper encoding, real creative content

## Phase 1: Cleanup — DONE
- Deleted ALL 130+ experiment videos from `assets/ytp_experiments/`
- Freed ~778MB of wasted space
- Kept only `ytp001_popeye_poop.sh` as reference

## Phase 2: Encoding Fix — CRITICAL
### Current Problems:
1. **Resolution**: Videos are 320x240 (not even 480p!) — source clips are low-res
2. **CRF**: Using `-crf 23` which is too high quality for YTP meme content
3. **Preset**: Using `-preset fast` — should use `-preset ultrafast` or `-preset superfast` for speed
4. **FPS**: Encoding at 30fps, source is 23.16fps — should match source
5. **Audio**: Using `-c:a aac` without bitrate cap — should be `-b:a 64k` for YTP
6. **No resolution enforcement**: Not scaling to 480p (854x480)

### Correct Encoding Command for YTP:
```bash
ffmpeg -y -i input.mp4 \
  -vf "scale=854:480:force_original_aspect_ratio=decrease,pad=854:480:(ow-iw)/2:(oh-ih)/2" \
  -c:v libx264 -preset ultrafast -crf 28 -r 24 \
  -c:a aac -b:a 64k \
  -movflags +faststart \
  output.mp4
```

### Expected Size Reduction:
- Current: 10MB for 18s @ 320x240 → ~1.3 MB/s
- Fixed: ~2MB for 18s @ 854x480 → ~0.11 MB/s (5x smaller despite higher res!)

## Phase 3: Lily Pad Research — COMPREHENSIVE

### LILY PAD 1: Diffusion Studio (Primary)
**URL**: https://github.com/diffusionstudio/editor
**Stars**: 2k | **Language**: TypeScript | **License**: MPL-2.0

#### Architecture (STUDY THIS):
1. **Composition-as-Code**: JSX-based video editing (like React for video)
   - `<stage>` → root canvas with camera
   - `<scene>` → clipped, playable frame with timeline
   - `<video>`, `<text>`, `<rect>`, `<image>`, `<audio>` → elements
   - `<animation>` → preset in/out animations
   - `<sequence>` → sequential placement
   - `<captions>` → auto-transcription + burn-in

2. **Pipeline**: Stamp → Compile → Evaluate → Mount → Resolve → Live
   - IDs minted per element
   - esbuild + babel-preset-solid compilation
   - Direct ECS mount (no DOM)
   - Background asset resolution
   - Reactive graph stays live

3. **Key Packages**:
   - `@diffusionstudio/jsx` — authoring API (elements, generate.*)
   - `@diffusionstudio/reconciler` — Solid-based renderer → ECS
   - `@diffusionstudio/encoder` — offline encoding (mediabunny)
   - `@diffusionstudio/assets` — asset library with content hashing
   - `@diffusionstudio/cli` — `dapi` CLI for agent-driven editing

4. **dapi CLI Commands**:
   - `dapi media probe/grab/filmstrip/waveform/transcribe/listen`
   - `dapi check` — self-eval, finds structural mistakes
   - `dapi context` — reports generation status
   - `dapi fonts` — font management

5. **Motion System** (recently added):
   - Keyframe interpolation (animejs-based)
   - Text animations: reveal words, scramble
   - Shader paints (WGSL fragment shaders)
   - Surface paints (canvas ref, sampled every frame)

#### What We Steal for C11:
- **Composition-as-code model**: Our EDL (Edit Decision List) should be code, not just data
- **Reactive asset resolution**: Background loading with placeholder states
- **Self-eval**: `dapi check` equivalent — validate our edits before rendering
- **Keyframe system**: Our wb_video_edit.c has scene detection but no keyframe interpolation
- **Shader paints**: WGSL-like fragment shader support in our compositor
- **Caption auto-transcription**: We have whisper.cpp, should auto-transcribe on import

### LILY PAD 2: CMU Motion Capture Database
**URL**: https://mocap.cs.cmu.edu/
**Data**: 2605 trials, 6 categories, 23 subcategories
**License**: Free for research and commercial use (cannot resell data directly)

#### Categories:
1. Human Interaction (two subjects)
2. Interaction with Environment (playground, uneven terrain)
3. Locomotion (running, walking)
4. Physical Activities & Sports (basketball, dance)
5. Situations & Scenarios (common behaviors, pantomime)
6. Test Motions

#### Download Options:
- `allasfamc.zip` — all ASF/AMC files
- `allc3d_*.zip` — all C3D files (4GB+ total)
- `allavi.zip` — all AVI reference movies
- `allmpg/` — all MPG rendered movies

#### Tools Available:
- C++ ASF/AMC viewer (Linux/Mac): `amc_viewer.r1806.tar.gz`
- mocapPlayer (cross-platform): `mocapPlayer.zip`
- AMC to Matrix (Matlab)
- C3D loading/saving (Matlab)
- Maya plugins for ASF/AMC and Vicon V format

### LILY PAD 3: amc2bvh (C Code — DIRECTLY USABLE)
**URL**: https://github.com/thcopeland/amc2bvh
**Stars**: 28 | **Language**: Pure C | **License**: MIT

#### What It Does:
- Converts ASF/AMC → BVH format
- Single C file (`amc2bvh.c` + `amc2bvh.h`)
- No external dependencies
- Includes `render_amc_animation.c` — renders AMC to a visual!
- Cross-platform Makefile

#### Why This Matters:
- **We can compile this directly into our pipeline**
- BVH is the standard format for skeletal animation
- Can convert CMU mocap → BVH → render as 2D skeleton overlay on video
- Perfect for YTP: overlay dancing skeletons on SpongeBob!

### LILY PAD 4: bvh11 (C++11 BVH Parser)
**URL**: https://github.com/yuki-koyama/bvh11
**Stars**: 43 | **Language**: C++11 | **License**: MIT

#### What It Does:
- Read AND write BVH files
- Clean API: `BvhObject(path)` → parse, `WriteBvhFile(path)` → export
- Dependencies: Eigen 3 (header-only math library)
- Includes demo with visualization

#### What We Steal:
- BVH parsing logic (can port to C11)
- Eigen is header-only, could use just the math routines
- Clean skeleton extraction API

### LILY PAD 5: AMCParser (C++ ASF/AMC Parser)
**URL**: https://github.com/FZzzz/AMCParser
**Language**: C++11 | **License**: MIT

#### What It Does:
- Parses ASF (skeleton) + AMC (motion) into BoneName-Keyframes map
- Self-contained, just add .h/.cpp to project
- Dependency: GLM (math)

#### What We Steal:
- ASF/AMC parsing logic for our C11 implementation
- Bone hierarchy data structure

### LILY PAD 6: ytpai (AI Sentence Mixing)
**URL**: https://github.com/stephenswetonic/ytpai
**Stars**: 24 | **Language**: Python/Svelte

#### What It Does:
- WhisperX for word-level transcription
- Drag-and-drop word mixing interface
- Generates YTP-style sentence mixes automatically
- Supports multiple languages

#### What We Steal:
- **Word-level mixing concept**: Our whisper.cpp gives us word timestamps — we can build sentence mixing in C11
- **UI pattern**: Drag words → rearrange → generate. Our CLI could accept word lists.

### LILY PAD 7: YTP-Editor (Python YTP Framework)
**URL**: https://github.com/Lulu2009-YTP/YTP-Editor
**Language**: Python | **License**: MIT

#### Effect Library (STEAL THESE):
- Supercut, Video Remixed/Meme, Parody, Vidding, Dance
- Stutter Loop, Ear-Rape, Bleep Censors
- Scrambling/Random Chopping, Stare Down/Zoom, SpaDinner
- Text overlays, Color shifts

#### What We Steal:
- Effect catalog — many of these we haven't implemented
- Bleep censor generation
- Scrambling/random chopping algorithm
- Stare-down zoom effect

### LILY PAD 8: SAM3DBody-cpp (Video → BVH)
**URL**: https://github.com/AmmarkoV/SAM3DBody-cpp
**Language**: C++

#### What It Does:
- Takes video → outputs BVH motion capture
- Uses YOLO + ONNX + GGUF models
- Can export per-person BVH files
- Includes Butterworth filtering for smooth output

#### Why This Matters:
- Could convert our source videos → BVH → apply CMU mocap on top
- "Dance like Subject #41" — retarget mocap onto characters

## Phase 4: Integration Plan

### Immediate (This Session):
1. Fix encoding settings in all YTP scripts
2. Download CMU mocap database (allasfamc.zip)
3. Clone amc2bvh, compile it, test conversion
4. Build BVH parser in C11 for our engine

### Short Term (Next 5 hops):
5. Implement BVH → 2D skeleton renderer (overlay on video)
6. Add sentence mixing using whisper.cpp word timestamps
7. Port bvh11 parsing logic to C11 (wb_bvh.c)
8. Add keyframe interpolation system (from Diffusion Studio's motion system)
9. Implement effect catalog from YTP-Editor

### Long Term (Vision):
10. Composition-as-code: video edits defined as data structures, not just CLI calls
11. Self-eval: validate edits before rendering (like dapi check)
12. Mocap retargeting: apply CMU dances to source characters
13. Full YTP studio: context-aware editing with plot/structure

## Phase 5: Encoding Fix Specification

### Current (WASTEFUL):
```
-c:v libx264 -preset fast -crf 23 -c:a aac
```
Result: 10MB for 18s @ 320x240

### Target (EFFICIENT):
```
-vf "scale=854:480:force_original_aspect_ratio=decrease,pad=854:480:(ow-iw)/2:(oh-ih)/2" \
-c:v libx264 -preset ultrafast -crf 28 -r 24 \
-c:a aac -b:a 64k -movflags +faststart
```
Expected: ~2MB for 18s @ 854x480 (5x smaller, higher res)

### For Source Clips (archival):
```
-vf "scale=854:480" -c:v libx264 -preset medium -crf 20 -c:a aac -b:a 128k
```
Keep quality for source material, still 480p.

## Key Insight: The Real Problem
The user is right — the current YTPs are just random effects with no structure.
What we need is:
1. **Context-aware editing**: Know what's happening in the video (scene detection ✓, transcript ✓)
2. **Plot/structure**: Beginning → Middle → End, not just random cuts
3. **Sentence mixing**: Rearrange words to create new meanings (ytpai approach)
4. **Mocap overlays**: Dancing skeletons, visual interest beyond source footage
5. **Proper encoding**: 480p at reasonable file sizes

The combination of:
- Big Mac's whisper.cpp (transcription + word timestamps)
- CMU mocap (2605 free animations)
- amc2bvh (C code, compiles anywhere)
- Diffusion Studio's architecture (composition-as-code, keyframes, self-eval)
- Our existing engine (audio render, video decode, compositor)

...creates a unique capability: **context-aware YTP editing on a 2012 iMac**.
