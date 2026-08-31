# R081-R082 — YTP Pipeline Overhaul: Complete System Summary

## What Was Built

### 1. BVH Motion Capture Parser (`src/wb_bvh.c`)
- Parses BVH (Biovision Hierarchy) files — the standard mocap format
- Recursive descent parser handles arbitrary joint hierarchies
- Forward kinematics computes 2D joint positions from rotation data
- Skeleton renderer draws bones+joints onto RGBA buffers
- CMU mocap downloader built in
- **Tested**: 54 BVH files from CMU subjects 01-25, all parse correctly
- 38 joints per skeleton, up to 5594 frames per motion

### 2. amc2bvh Converter (cloned to `ref/amc2bvh/`)
- Pure C, no dependencies, compiles on this iMac
- Converts CMU ASF/AMC → BVH format
- **Tested**: All 54 conversions successful

### 3. Sentence Mixing Engine (`tools/wb_sentence_mix.c`)
- Takes whisper.cpp word-level transcripts
- 8 mixing strategies: shuffle, stutter, word salad, reverse, emphasize, build phrase, interleave, every-nth
- Exports EDL (Edit Decision List) for ffmpeg
- Exports human-readable scripts
- SRT parser loads transcripts directly

### 4. YTP Composition Engine (`tools/wb_ytp_compose.c`)
- **The brain**: takes a transcript + chaos level → produces structured YTP
- **Plot structure**: INTRO → DIALOGUE (escalating) → OUTRO
- **Context-aware effects**: NOT random — chosen based on content:
  - "the" → stutter (YTP staple)
  - questions → reverse
  - exclamations → earrape
  - short phrases → vine boom
  - sentence position → intensity
- **Pacing**: segments of 5-8 words, split on punctuation/pauses
- **Chaos levels**: 1-10, controls effect density
- Exports: human-readable script + ffmpeg EDL

### 5. Character Asset System (`tools/wb_asset_dl.c` + `docs/R082-`)
- Master list of 85+ characters across 2 tiers
- Tier 1: 52 essential 2D sprites (Mario, Sonic, Sans, etc.)
- Tier 2: 33 3D models (SM64, OoT, Crash, etc.)
- Library organized by console/dimension
- All from spriters-resource.com / models-resource.com (free)

### 6. Video Encoding Fix (`src/wb_video.c`)
- Was: 320x240 @ 10MB for 18s clips
- Now: 854x480 (true 480p) @ ~2MB for 18s clips
- Settings: `-preset veryfast -crf 23 -r 30 -b:a 128k`
- Proxy generation also fixed with proper aspect ratio padding

### 7. Diffusion Studio Research (`docs/R081-`)
- Architecture reference: composition-as-code, keyframes, self-eval
- Key steal: JSX-like composition model, reactive asset resolution
- Can't run on this iMac (Node 20+), but patterns are documented

## What Makes This Different From Before

### Before (Bad):
- Random slowdowns + speedups
- One random blast section
- Same pattern every time
- No plot, no recognition, no context
- 360p, 10MB for 18 seconds

### After (Good):
- **Structured composition** with plot arcs
- **Context-aware effects** chosen by content analysis
- **Sentence mixing** rearranges words into new meanings
- **Mocap overlays** add visual interest beyond source footage
- **Proper 480p encoding** at 5x smaller file sizes
- **Character library** of 85+ recognizable characters
- **Multiple chaos levels** for different energy

## File Inventory

| File | Purpose | Status |
|------|---------|--------|
| `src/wb_bvh.c` | BVH parser + 2D skeleton renderer | ✅ Built, tested |
| `include/wbus/wbus_bvh.h` | BVH API header | ✅ Done |
| `tools/test_bvh.c` | BVH test program | ✅ All pass |
| `tools/wb_sentence_mix.c` | Sentence mixing engine | ✅ Built, tested |
| `tools/wb_ytp_compose.c` | YTP composition engine | ✅ Built, tested |
| `tools/wb_asset_dl.c` | Character asset downloader | ✅ Built, tested |
| `ref/amc2bvh/` | ASF/AMC → BVH converter | ✅ Cloned, compiled |
| `assets/characters/mocap/cmu/*.bvh` | 54 mocap animations | ✅ Downloaded |
| `docs/R081-lilypad-research-and-encoding-fix.md` | Research doc | ✅ Written |
| `docs/R082-character-asset-master-list.md` | Character database | ✅ Written |

## Next Steps (When Ready to Make Videos)

1. Download sprite sheets for Tier 1 characters
2. Download 3D models for Tier 2 characters
3. Build ffmpeg render pipeline that uses the composition EDL
4. Add mocap skeleton overlay as a compositor node
5. Add sentence mixing audio pipeline (pitch shift, stutter, reverse)
6. Make a YTP that actually has plot, pacing, and payoff
