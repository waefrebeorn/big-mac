# R080 YTP Experiments — Full Log

**Date:** 2026-08-30
**Goal:** Learn video editing / YTP techniques through hands-on experimentation
**Rule:** Each experiment MUST use different base media

---

## Source Library (Final)

| Category | Count | Size |
|----------|-------|------|
| Full PD episodes | 41 | ~2.3GB |
| Short clips (10s extracts) | 95 | ~120MB |
| Meme sound effects | 4 | ~1MB |
| Extracted audio tracks | 13 | ~27MB |
| **Total** | **153+ files** | **~2.5GB** |

### Collections Acquired
1. **Popeye the Sailor** (Fleischer, 1933-40s) — 8 episodes
2. **Hercules 1960s Cartoon** (Daedalus series) — 4 episodes
3. **Ub Iwerks Comicolor** — Balloon Land, Don Quixote, Ali Baba, Mary's Little Lamb, Summertime, Valiant Tailor, Jack and the Beanstalk, Tom Thumb, Simple Simon, Headless Horseman, Little Red Hen, Three Bears, Boy Blue, Scatcrow, Brementown Musicians, Happy Days, Skeleton Frolics, Humpty Dumpty, Old Mother Hubbard, Summer VS Winter
4. **Fleischer Color Classics** — Greedy Humpty Dumpty, Hawaiian Birds, Little Stranger, Little Lambkins, Somewhere in Dreamland, Gabby King for a Day
5. **Classic Cartoons** — Betty Boop, Bugs Bunny, Bimbo's Auto, Ali Baba Bound, Porky's Cafe, Sailor, Henpecked Duck, Toyland, WhoZoo, A Coy Decoy
6. **1956 Public Domain Animation** — 90 Day Wondering, Adventures of *, Date with Dizzy, Destination Earth, Living Circle, Popeye for President, Hooked Bear, Out to Punch, Assault and Flattery, Insect to Injury
7. **Jerky Turkey** (1945)
8. **Little Lulu** (Cad and Caddy)
9. **Once Upon a Time**
10. **Meme SFX:** Vine Boom, Yeet, Taco Bell Bong, Discord Notification

---

## Experiment Results

### YTP 001: Popeye Poop
**Source:** Popeye "A Clean Shaven Man"
**Output:** `ytp001_popeye_poop_FINAL.mp4` (13MB, 72s)
**Techniques:** stutter loop, chipmunk pitch, reverse+earrape, deep fry, VHS, kaleidoscope, sentence mix, combo

### YTP 002: Hercules Poop
**Source:** Hercules "Daedalus and the Evil Unicorn"
**Output:** `ytp002_hercules_poop_FINAL.mp4` (6.1MB, 111s)
**Techniques:** speed up 2x, slow mo, vine boom overlay, boom stutter, tape stop, databash, robot voice

### YTP 003: Scatcrow Poop
**Source:** Scatcrow (Ub Iwerks)
**Output:** `ytp003_scatcrow_poop_FINAL.mp4` (28MB, 102s)
**Techniques:** multi-boom layer, demon voice, hyperfast, invert+earrape, sentence mix, fried+reversed

### YTP 004: Betty Boop + Bugs + Toyland
**Source:** Betty Boop, Bugs Bunny, Toyland
**Output:** 6 individual effects
**Techniques:** chipmunk, demon reverse, fry+invert, cross-source sentence mix, VHS earrape, databash

### YTP 005: Fleischer Color Classics
**Source:** Greedy Humpty Dumpty, Ali Baba Bound, Little Lambkins
**Output:** `ytp005_fleischer_poop_FINAL.mp4` (7.7MB, 94s)
**Techniques:** pitch wobble (vibrato), slow-mo demon, deep fry stutter, kaleidoscope, triple-source mix, reverse+hflip+earrape

### YTP 000: MEGA MIX
**Sources:** All of experiments 001-003
**Output:** `ytp000_MEGA_MIX_FINAL.mp4` (17MB, 183s)

---

## YTP Techniques Master Catalog

| # | Technique | FFmpeg Implementation | First Used |
|---|-----------|----------------------|------------|
| 1 | Stutter loop | `concat` segment repeat (5x) | YTP001 |
| 2 | Pitch shift (chipmunk) | `asetrate=44100*1.5,aresample=44100` | YTP001 |
| 3 | Pitch shift (demon) | `asetrate=44100*0.3-0.4,aresample=44100` | YTP003/004 |
| 4 | Reverse | `reverse` / `areverse` | YTP001 |
| 5 | Earrape | `volume=3-6x` | YTP001 |
| 6 | Deep fry | `eq=contrast+saturation,unsharp,noise` | YTP001 |
| 7 | VHS | `colorchannelmixer,noise,aecho` | YTP001 |
| 8 | Kaleidoscope | `split+crop+flip+xstack` (4-way) | YTP001 |
| 9 | Sentence mix | `concat` different source clips | YTP001 |
| 10 | Speed change | `setpts=PTS*0.25~3.0` + `atempo` | YTP002/003 |
| 11 | Meme SFX layer | `amix=inputs=2:weights='1 0.8'` | YTP002 |
| 12 | Tape stop | `rubberband=pitch=0.5:tempo=0.5` | YTP002 |
| 13 | Databash | `acrusher=bits=3-4:mix=0.5-0.7` | YTP002 |
| 14 | Multi-source mix | `concat=n=3` with scale normalize | YTP005 |
| 15 | Pitch wobble | `vibrato=f=8:d=0.5` | YTP005 |
| 16 | Color invert | `negate` | YTP004 |
| 17 | Horizontal flip | `hflip` | YTP005 |
| 18 | Combo/chain | Multiple filters in one pass | YTP001 |

---

## Key Learnings

1. **Quote escaping matters** — filenames with `'` break shell `python3 -c '...'` — use Python scripts with proper args
2. **Normalize before concat** — different resolutions/encodings break concat; always `scale=W:H,setsar=1` first
3. **asetrate trick** — changing sample rate then resampling changes pitch without tempo change (pure formant shift)
4. **vibrato filter** — available in this ffmpeg build, great for pitch wobble effects
5. **rubberband** — available for proper pitch/tempo independent control
6. **amix weights** — prevents clipping when layering SFX: `weights='1 0.7'`
7. **1956 PD animations** — huge files (397MB for 7min) but great quality
8. **Fleischer Color Classics** — inherently surreal visuals, perfect for YTP
9. **Archive.org rate limits** — parallel downloads from same IP get throttled; sequential with delays works better
10. **yt-dlp install** — running in background (proc_64c7669a1ec6), needed for future YouTube source downloads

## Next Steps
- [ ] Wait for yt-dlp install → download YouTube-available YTP sources (SpongeBob, CD-i, Mario)
- [ ] Experiment 006: 1956 Popeye for President (different era Popeye)
- [ ] Experiment 007: Skeleton Frolics (inherently spooky → YTP gold)
- [ ] Build YTP preset system: chainable effect presets in a config file
- [ ] Integrate Big Mac C11 modules (wb_formant, wb_bleep, wb_kaleidoscope) into video pipeline
- [ ] Datamosh proper: P-frame manipulation using ffmpeg's mpeg4-RAW
