# R080 YTP Experiments 001-003

**Date:** 2026-08-30
**Goal:** Learn video editing / YTP techniques through hands-on experimentation
**Rule:** Each experiment MUST use different base media

## Source Library

| Category | Count | Size |
|----------|-------|------|
| Full PD episodes | 14 | ~157MB |
| Short clips (10s extracts) | 35 | ~30MB |
| Meme sound effects | 4 | ~1MB |
| Extracted audio tracks | 13 | ~27MB |
| **Total** | **66 files** | **~204MB** |

### Sources Acquired
- **Popeye the Sailor** (Fleischer, 1930s-40s) — 7 episodes
- **Hercules 1960s Cartoon** — 4 episodes (Daedalus series)
- **Scatcrow** (Ub Iwerks)
- **Little Boy Blue** (Ub Iwerks Comicolor, 1936)
- **Sound Effects Archive** (general)
- **Meme SFX:** Vine Boom, Yeet, Taco Bell Bong, Discord Notification

## Experiment 001: Popeye Poop
**Source:** Popeye "A Clean Shaven Man" (public domain)
**Duration:** 71 seconds

| # | Technique | Effect | Size |
|---|-----------|--------|------|
| 1 | Stutter loop | 1s segment repeated 5x | 278K |
| 2 | Chipmunk | Pitch up 1.5x (asetrate) | 986K |
| 3 | Reverse + earrape | Reversed, volume 3x | 981K |
| 4 | Deep fry | Saturation 3x, unsharp, noise | 7.3M |
| 5 | VHS | Chroma shift, tracking noise, echo | 1.3M |
| 6 | Kaleidoscope | 4-way mirror tile | 1.0M |
| 7 | Sentence mix | Pitch down 0.6x (deep voice) | 1.1M |
| 8 | Combo | Contrast + saturation + noise + earrape 2x | 2.1M |

**YTP techniques demonstrated:** stutter, pitch shift, reverse, earrape, deep fry, VHS, kaleidoscope, sentence mix

## Experiment 002: Hercules Poop
**Source:** Hercules "Daedalus and the Evil Unicorn" (public domain)
**Duration:** 111 seconds

| # | Technique | Effect | Size |
|---|-----------|--------|------|
| 1 | Speed up 2x | setpts 0.5x + atempo 2.0 | 308K |
| 2 | Slow motion | setpts 2.0x + atempo 0.5 | 829K |
| 3 | Vine boom overlay | amix with vine_boom.flac | 557K |
| 4 | Boom stutter | Vine boom segment looped 3x | 1.0M |
| 5 | Tape stop | rubberband pitch=0.5 tempo=0.5 | 872K |
| 6 | Databash | Bitcrush 4-bit + downscale/upscale | 1.1M |
| 7 | Robot voice | Pitch down 0.4x | 951K |

**YTP techniques demonstrated:** speed ramp, slow mo, meme sound insertion, tape stop, databash/bitcrush, robot voice

## Experiment 003: Scatcrow Poop
**Source:** Scatcrow (Ub Iwerks, public domain)
**Duration:** 101 seconds

| # | Technique | Effect | Size |
|---|-----------|--------|------|
| 1 | Multi-boom layer | Vine boom + yeet mixed together | 1.1M |
| 2 | Demon voice | Pitch down 0.3x + volume 2x | 1.6M |
| 3 | Hyperfast 4x | atempo 2.0 chained twice | 524K |
| 4 | Invert + earrape | Color negate + volume 5x | 1.2M |
| 5 | Sentence mix | Demon + hyperfast concatenated | 2.0M |
| 6 | Fried + reversed | Reverse + contrast 2x + saturation 5x + noise | 36M |

**YTP techniques demonstrated:** multi-sfx layering, demon voice, hyperfast, color invert, sentence mix, deep fry + reverse combo

## MEGA MIX
**All three experiments concatenated**
- `ytp000_MEGA_MIX_FINAL.mp4` — 17MB, 182 seconds (~3 minutes)
- Popeye → Hercules → Scatcrow

## YTP Techniques Learned & Applied

| Technique | FFmpeg Filter | YTP Use |
|-----------|--------------|---------|
| Stutter loop | concat (segment repeat) | Repeat words/phrases |
| Pitch shift | asetrate + aresample | Chipmunk/demon voices |
| Speed change | setpts + atempo | Fast/slow motion |
| Reverse | reverse / areverse | Backwards speech |
| Earrape | volume=3-5x | Loudness abuse |
| Deep fry | eq=contrast+saturation, unsharp, noise | Visual destruction |
| VHS | colorchannelmixer, noise, echo | Retro degradation |
| Kaleidoscope | split+crop+flip+xstack | Mirror symmetry |
| Databash | acrusher, scale down+up | Digital destruction |
| Meme SFX | amix | Vine boom, yeet, etc. |
| Tape stop | rubberband pitch+tempo | Record slowdown |
| Color invert | negate | Negative image |
| Sentence mix | concat clips | Word salad from different parts |

## Lessons Learned

1. **Always extract clips with audio** — first batch dropped audio streams
2. **Normalize before concat** — different encodings break concat demuxer
3. **amix weights** — use `weights='1 0.8'` to prevent clipping when layering SFX
4. **asetrate trick** — changing sample rate then resampling changes pitch without tempo (pure formant shift)
5. **rubberband** — available in this ffmpeg build, does proper pitch/tempo independent control
6. **Scatcrow is goldmine** — 1930s Ub Iwerks has inherently absurd visuals perfect for YTP

## Next Experiments
- [ ] Datamosh proper (P-frame manipulation, not just bitcrush)
- [ ] Sentence mix from same source (word salad)
- [ ] Taco bell bong insertion
- [ ] Multi-source crossfade (Popeye → Hercules mid-sentence)
- [ ] Audio-reactive visual effects (use Big Mac's wb_audio_color)
- [ ] Formant shift via Big Mac engine (wb_formant.c) for voice character
- [ ] Bleep censor via Big Mac engine (wb_bleep.c)
