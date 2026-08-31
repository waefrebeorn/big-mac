# R080 — YTPMV: YouTube Poop Music Video Research

## What is YTPMV?

YTPMV (YouTube Poop Music Video) is a subgenre where source audio is chopped into phonemes/syllables and rearranged to create music — melodies, beats, basslines, harmonies. It's the intersection of YTP and music production.

## Core Techniques

### 1. Sentence Mixing / Phoneme Chopping
- Isolate individual phonemes (vowels, consonants) from source audio
- Arrange them to form new words or musical notes
- Example: "snooping as usual" → "pi-ngas" (Sonic YTP)

### 2. Vocal Parallel / Vocal Transplant
- Overlay a character's voice onto unrelated visual movements
- Creates absurd lip-sync gags

### 3. Stutter-Bass
- Loop a short phrase (2-4 frames)
- Pitch-shift down to create bass drum effect
- Foundation of YTPMV beats

### 4. Pitch Shifting / Vocoding
- Alter timbre and melody for musical remixes
- Chipmunk (2.0-2.5x) for high melodies
- Demon (0.3-0.4x) for bass lines

### 5. Chroma Key / Screen Replacement
- Swap backgrounds and inserts
- Composite characters onto different scenes

### 6. Visual Distortions
- Swirl / Wave / Spherize for surreal visual gags
- Deep fry (contrast + saturation + noise)

### 7. Mad Dash Cut
- Quick 3-6 clip cuts with increasing tempo
- Finish with freeze-frame gag

## YTPMV Source Tier List (from YTPMV Wiki)

### S-Tier (Most Used)
| Source | Type | Why It's Good |
|--------|------|---------------|
| Billy Mays | Commercials | Loud, energetic, repetitive ("But wait, there's more!") |
| Michael Rosen | Children's poetry | Clear diction, rhythmic, expressive |
| CD-i Mario/Zelda | Games | Bad voice acting = comedy gold |
| SpongeBob | Cartoon | Expressive voices, musical episodes |
| Game Grumps | YouTube | Natural speech, varied emotions |
| Donald McDonald | Japanese ads | Weird dubbing, unique phonemes |

### A-Tier
| Source | Type | Notes |
|--------|------|-------|
| Angry Video Game Nerd | YouTube | Loud, angry, varied vocabulary |
| JonTron | YouTube | Expressive, comedic timing |
| Nostalgia Critic | YouTube | Dramatic, over-the-top |
| Family Guy | TV | Distinct character voices |
| Dragon Ball Z | Anime | Iconic screams, power-ups |
| Rick Astley | Music | "Never Gonna Give You Up" |
| LazyTown | TV | "We Are Number One" |

### B-Tier
| Source | Type | Notes |
|--------|------|-------|
| Old Spice | Commercials | Deep voice, absurd scenarios |
| Mega64 | YouTube | Skit-based, varied |
| Etika | Streaming | Energetic, catchphrases |
| The Room | Movie | Awkward delivery, quotable |
| Mario 64 | Game | "It's-a me, Mario!" |
| Windows | OS | Startup sounds, error dings |

### C-Tier (Niche but usable)
| Source | Type | Notes |
|--------|------|-------|
| Keyboard Crasher | Viral | Repetitive keyboard sounds |
| Trolololo | Music | Eduard Khil song |
| Gangnam Style | Music | PSY song |
| Doot | Music | Skull Trumpet |
| Woah! | Meme | Sound effect |

## YTPMV Production Pipeline

### Step 1: Source Selection
- Choose source with clear vocal audio
- Look for varied phonemes (vowels + consonants)
- Musical sources (singing) = melody material
- Speech sources = rhythm/percussion material

### Step 2: Phoneme Extraction
- Transcribe audio (whisper.cpp)
- Identify individual words/syllables
- Extract each phoneme as separate audio clip
- Catalog by pitch, duration, timbre

### Step 3: Beat Construction
- Use stutter-bass technique for kick drum
- Pitch-shift short clips for snare/hi-hat
- Loop rhythmic phrases for groove

### Step 4: Melody Construction
- Arrange phonemes to match target melody
- Use pitch shifting to hit correct notes
- Layer multiple sources for harmony

### Step 5: Visual Sync
- Match mouth movements to new audio
- Use chroma key for background swaps
- Add visual distortions on beat drops

### Step 6: Mix & Master
- Balance audio levels
- Add reverb/delay for space
- Compress for loudness

## Implementation for Big Mac

### Phoneme Extractor
1. Use whisper.cpp word-level timestamps
2. Segment audio at word boundaries
3. Further segment into phonemes using silence detection
4. Build searchable phoneme database

### Auto-BPM Detector
1. Analyze source audio for tempo
2. Detect beat positions
3. Quantize phoneme placement to grid

### Pitch Corrector
1. Detect original pitch of each phoneme
2. Calculate pitch shift needed for target note
3. Apply formant-preserving pitch shift

### Visual Sync Engine
1. Detect mouth/face in video frames
2. Match mouth shape to vowel phonemes
3. Trigger visual effects on consonant hits

## Tools We Need to Build

1. **Phoneme Extractor** — whisper.cpp → word timestamps → phoneme segmentation
2. **BPM Detector** — analyze source tempo, quantize to grid
3. **Pitch Mapper** — map phonemes to musical notes (C4, D4, etc.)
4. **Beat Sequencer** — arrange phonemes on a musical grid
5. **Visual Sync** — match mouth movements to new audio
6. **YTPMV Renderer** — composite final video with effects

## Sources to Acquire for YTPMV

### Priority 1: Vocal-Heavy Sources
- [ ] Billy Mays OxiClean (original, not Mighty Tape)
- [ ] Game Grumps episodes
- [ ] JonTron episodes
- [ ] Angry Video Game Nerd episodes

### Priority 2: Musical Sources
- [ ] LazyTown "We Are Number One"
- [ ] Rick Astley "Never Gonna Give You Up"
- [ ] SpongeBob musical episodes
- [ ] Mario 64 ("It's-a me!")

### Priority 3: Unique Phonemes
- [ ] Japanese commercials (Donald McDonald)
- [ ] El Chavo del Ocho (Don Ramón)
- [ ] Gachimuchi (internet subculture)
