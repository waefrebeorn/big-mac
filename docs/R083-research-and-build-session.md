# R083 — Recursive Improvement Loop: Research & Build Session

## Session Summary
Continued from R081-R082 (lily pad research + YTP pipeline overhaul).
This session: built YTP Studio, batch transcriber, mocap overlay, YTP Director.
**FIRST REAL YTP RENDERED** — 177s Shrek YTP with 102 edit operations.

## What Was Built

### 1. YTP Studio (`tools/wb_ytp_studio.c`)
- Context-aware "director" with 4-act plot structure
- Callback system for in-jokes and references
- 25 effect types with ffmpeg filter chains
- Content-aware effect selection (the→stutter, !→earrape, etc.)
- Intensity curve: setup(2-4) → escalate(4-7) → climax(7-10) → resolve(2-4)
- Proper 480p encoding: CRF 28, ultrafast, 64k audio, faststart
- Tested: Shrek lyrics → 28 segments with proper structure

### 2. Batch Transcriber (`tools/ytp_transcribe.c`)
- Extracts audio from any video via ffmpeg
- Transcribes via whisper.cpp (tiny.en-q5_1)
- Converts segment-level to word-level timestamps
- Tested: Shrek "Somebody Once Told Me" → 488 words with ms timestamps

### 3. Committed Previous Work
- 80+ new engine modules (warp, spectral_fx, sonogram, etc.)
- 57 test modules
- BVH parser with 2D skeleton rendering
- YTP composition engine (wb_ytp_compose.c)
- YTP render pipeline (wb_ytp_render.c)
- Sentence mixing tool (wb_sentence_mix.c)
- Asset downloader (wb_asset_dl.c)

## Research: New Lily Pads

### FREE MOCAP DATA (downloadable now!)
1. **CMU Motion Capture** (mocap.cs.cmu.edu) — 2605 trials, all ASF/AMC
   - Daz-friendly BVH release: 2548 motions at cgspeed.com
   - Already have wb_bvh.c parser + CMU downloader!
   
2. **Bandai Namco Research** (github.com/BandaiNamcoResearchInc)
   - 3007 BVH moves, 421,604 frames
   - 17 motion types, 15 styles
   - CC-BY-NC-ND license (free for personal use)
   - Includes Blender visualization script

3. **SFU Motion Capture** (mocap.cs.sfu.ca) — 30 subjects, 12 categories
   - Free for research

4. **Rokoko** (rokoko.com/free-resources) — 263 free mocap assets
   - FBX/BVH export

5. **NVIDIA MotionBricks** — 350,000 motion clips, 15,000 FPS
   - BONES-SEED dataset on HuggingFace
   - 142,000 production-grade BVH clips
   - SOMA 77-joint skeleton format

### AI VIDEO TOOLS (for future integration)
1. **Descript API** — programmatic transcript-based editing
2. **auto-editor** (WyattBlue) — silence/motion-based auto-cutting
3. **PySceneDetect 0.7.1** — shot detection with Python API
4. **Wan2.1** — open-source text-to-video (local)
5. **CogVideoX** — open-source text/image-to-video
6. **yt-dlp** — media acquisition from any source

### YTP TECHNIQUES (from research)
1. **Sentence mixing** — rearrange words into new phrases
2. **Stutter loop** — repeat word 2-5x for emphasis
3. **Voice transplant** — replace one character's voice with another
4. **Lip sync** — edit visuals to match different audio
5. **To Be Continued** — arrow + music sting
6. **Datamosh** — pixel corruption between frames
7. **Deep fry** — saturation + contrast + noise
8. **VHS** — tracking errors, color bleed

## Asset Inventory
- **1169 source videos** across 18 categories (5.7GB total)
- Categories: public_domain (2.3G), youtube_rips (5G), childhood (315M),
  famous_clips (310M), more_cartoons (1G), ytp_classics (1G), etc.
- **4 transcripts** (need more — batch transcriber ready)
- Characters: Shrek, SpongeBob, Mario, Luigi, Thomas, Caillou,
  Arthur, Popeye, Sonic, Zelda CD-I, Hotel Mario, etc.

## Engine Status
- **753 checks, 0 failures** — clean build
- All new modules compile and test clean

### 5. Mocap Overlay (`wb_mocap_overlay.c`)
- Renders BVH skeleton on top of any video via ffmpeg
- Tested: CMU walk + Bandai Namco BVH → green skeleton on Shrek video
- Configurable: color, scale, offset, flip

### 6. YTP Director (`wb_ytp_director.c`)
- Text description → structured EDL JSON
- Parses: title, source, transcript, chaos, plot points, preferred/avoided effects
- Generates: 4-act structure, intensity curve, encoding settings

## New Lily Pads (Research)

### SentrySearch (github.com/ssrajadh/sentrysearch) — 4.4k stars
- **Semantic search over video footage** — type "red truck running stop sign", get clip
- Works on any MP4/MOV, auto-trims matches
- Local backend (Qwen3-VL) or Gemini API backend
- **Highlights mode**: auto-finds weirdest/anomalous moments (PERFECT for YTP)
- Search by image: upload screenshot, find visually similar clips
- Apache-2.0 license
- **BLOCKED**: Needs Gemini API key (free at aistudio.google.com/apikey) or GPU for local
- Future: get Gemini key → index all 1169 videos → semantic YTP clip discovery

### Free Sound Effects
- Zapsplat: 160k+ sounds, free MP3 with account
- Freesound.org: community-driven, various licenses
- Useful for YTP: vine boom, airhorn, bruh, meme sounds

## Completed Milestones
- ✅ First real YTP rendered (177s, 102 ops, 18MB)
- ✅ BVH mocap pipeline (download → parse → render → overlay)
- ✅ 3077 free BVH files from Bandai Namco
- ✅ Word-level transcription pipeline
- ✅ Full YTP studio with plot structure

## Next Steps (Future Sessions)
1. Get Gemini API key → install SentrySearch → semantic clip discovery
2. Batch transcribe all 1169 source videos
3. Download free sound effects library
4. Build more YTPs with different source videos
5. Combine mocap overlay + YTP effects for 3D poops
6. Build "YTP from text" → full auto-generation
