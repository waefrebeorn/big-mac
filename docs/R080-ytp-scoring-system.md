# R080 — YTP Edit Recognition & Scoring System

## Overview

To avoid "repetitive trash," we need objective metrics to evaluate edits.
This document defines accuracy scores, creativity scores, and edit classification.

## Edit Classification Taxonomy

### By Technique
| Category | Technique | FFmpeg Implementation |
|----------|-----------|----------------------|
| **Temporal** | Stutter/repeat | concat segment repeat |
| | Speed change | setpts + atempo |
| | Reverse | reverse / areverse |
| | Frame drop | select='not(mod(n,N))' |
| | Time remap | select + setpts |
| **Pitch/Tonal** | Chipmunk | asetrate * 1.5-2.0 |
| | Demon/deep | asetrate * 0.3-0.5 |
| | Robot | asetrate * 0.4 |
| | Vibrato | vibrato filter |
| **Distortion** | Earrape | volume > 3x |
| | Bitcrush | acrusher |
| | Deep fry | eq=contrast+saturation+noise |
| | Datamosh | scale down+up, channel shift |
| **Visual** | Pixelation | scale iw/N:ih/N scale back |
| | VHS | noise + colorchannelmixer + aecho |
| | Scanlines | darken every other row |
| | Chroma key | colorkey + overlay |
| | Kaleidoscope | split+crop+flip+xstack |
| **Compositing** | Green screen | colorkey + overlay |
| | Picture-in-picture | overlay with scaling |
| | Split screen | hstack / vstack |
| | Text overlay | drawtext |
| **Audio** | Meme SFX | amix with sound effects |
| | Bleep censor | volume automation |
| | Vinyl stop | rubberband pitch+tempo |
| | Echo/delay | aecho filter |

### By Complexity Level
| Level | Description | Example |
|-------|-------------|---------|
| 1 - Basic | Single effect | Just pitch shift |
| 2 - Simple | 2-3 effects | Pitch + speed + reverse |
| 3 - Moderate | 4-6 effects | Deep fry + stutter + earrape + VHS |
| 4 - Complex | 7+ effects, multi-source | Chroma key + pitch + speed + effects + composite |
| 5 - Master | Full pipeline, original composition | Tennis-style response edit with 10+ techniques |

## Accuracy Score (0-100)

Measures how well the edit follows YTP conventions and rules.

### Components:
1. **Source integrity** (20 pts): Is the source recognizable?
2. **Transformation** (20 pts): Is it sufficiently transformed?
3. **Technical execution** (20 pts): Clean edits, no artifacts (unless intentional)?
4. **Audio-visual sync** (20 pts): Is audio matched to visuals?
5. **Convention adherence** (20 pts): Does it follow YTP norms?

### Scoring Rubric:
| Score | Rating | Description |
|-------|--------|-------------|
| 90-100 | Excellent | Professional-quality YTP, all conventions met |
| 70-89 | Good | Solid edit, minor issues |
| 50-69 | Average | Basic edit, some conventions missed |
| 30-49 | Below average | Sloppy or off-convention |
| 0-29 | Poor | Barely transformed or broken |

## Creativity Score (0-100)

Measures originality and artistic merit.

### Components:
1. **Novelty** (25 pts): Has this been done before?
2. **Surprise** (25 pts): Unexpected combinations or techniques?
3. **Humor** (25 pts): Is it funny (if aiming for comedy)?
4. **Aesthetic** (25 pts): Is it visually/aurally pleasing (even if absurd)?

### Scoring Rubric:
| Score | Rating | Description |
|-------|--------|-------------|
| 90-100 | Groundbreaking | New technique or never-seen combination |
| 70-89 | Creative | Fresh take on existing techniques |
| 50-69 | Competent | Standard techniques, well executed |
| 30-49 | Derivative | Common combinations, nothing new |
| 0-29 | Trash | Repetitive, no creative thought |

## Detection Algorithms

### Edit Detection (What techniques were used?)
1. **Reverse detection**: Compare frame n with frame N-n. High correlation = reversed.
2. **Speed change detection**: Analyze frame difference patterns. Lower = faster.
3. **Pitch shift detection**: Analyze audio frequency spectrum. Shifted = pitch changed.
4. **Stutter detection**: Find repeated frame sequences.
5. **Effect detection**: Analyze color histogram changes (saturation, contrast).
6. **Composite detection**: Look for hard edges, color key artifacts.

### Implementation Notes:
- Use FFmpeg's `signature` filter for frame comparison
- Use `astats` and `showspectrum` for audio analysis
- Use `signalstats` for visual analysis
- Compare before/after to classify transformations

## Tennis-Specific Scoring

### Response Quality (0-100)
- Did the player respond to the opponent's specific edits?
- Did they build upon or subvert the previous round?
- Did they maintain continuity while adding originality?

### Match Flow (0-100)
- Does the match have a narrative arc?
- Does complexity escalate naturally?
- Are there callbacks to earlier rounds?
- Does the final round feel like a satisfying conclusion?

## Avoiding "Repetitive Trash"

### Warning Signs:
1. Same effect applied to every source
2. No variation in technique across experiments
3. Effects applied randomly without purpose
4. No response to the source material's properties
5. Audio and visuals not coordinated
6. No comedic or artistic intent

### Improvement Strategies:
1. **Study the source**: What makes this source unique? Exploit that.
2. **Match technique to source**: Don't just apply random effects.
3. **Build a narrative**: Even absurdist edits should have internal logic.
4. **Vary techniques**: Don't use the same 3 effects every time.
5. **Study great YTPs**: Analyze what makes them work.
6. **Practice constraints**: Limit yourself to force creativity.
