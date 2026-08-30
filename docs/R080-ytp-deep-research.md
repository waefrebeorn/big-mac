# R080 — YouTube Poop (YTP) Deep Research & Technique Catalog

## 1. History & Origins

### Timeline
- **1938** — Proto-YTP: "Daffy Duck in Hollywood" (Warner Bros) — reversing clips, overdubbing
- **1968** — Nixon attack ad vs Hubert Humphrey — YTP-like editing in politics
- **2004-12-22** — First true YTP: SuperYoshi's "The Adventures of Super Mario Bros. 3 REMIXED!!!" (SheezyArt, before YouTube existed). Used Windows Movie Maker, "Recycled Koopa" episode
- **2005** — First YTPs on YouTube: "I'D SAY HE'S HOT ON OUR TAIL", "Dr. Light's Guidance System Show", "Robotnik's Sodomy Fest of 2005"
- **2006** — Term "YouTube Poop" coined by Yaminomalex ("uploading poop to YouTube")
- **2007-06-04** — YouChewPoop (later YouChew) forum opens — main YTP community hub
- **Mid-to-late 2000s** — "Golden Age" — shared memes, in-jokes, community
- **2010** — "jonathan swift returns from the dead to eat a cheese sandwich" — moved YTP toward artistry
- **Late 2010s** — Decline in mainstream popularity; fragmentation into sub-communities
- **2020s** — Niche revival, sophisticated editing, references-within-references

### Key Creators
- **SuperYoshi** — originator (Mario Bros 3 cartoon)
- **RetroJape** — early collaborator (Mega Man, Sonic)
- **Stegblob** — "Robotnik Has a Viagra Overdose" (created "pingas" meme)
- **EmpLemon** — documented Golden Age, later film analysis
- **Schaffrillas Productions** — massive YTP collabs, later film analysis
- **Yaminomalex** — coined the term "YouTube Poop"

### Cultural Lineage
- William Burroughs cut-up technique → hip-hop sampling → MTV frenetic 80s editing → vidding/AMV → YTP
- Compared to: Dada art movement, noise music, heavy metal, plunderphonics, détournement

## 2. Poopisms (Complete Technique Catalog)

### Core Techniques (the "Poopisms")
| # | Technique | Description | BigMac Status |
|---|-----------|-------------|---------------|
| 1 | **Stutter Loop** | Short clip repeated over+over. Oldest poopism (SuperYoshi). | ✅ wb_stutter.c (10 types) |
| 2 | **Stutter Loop Plus** | Different video effect per loop iteration | ✅ variations in wb_stutter |
| 3 | **Stutter Loop Minus** | Remove video, keep audio (implied sex scene) | ❌ MISSING |
| 4 | **Buzzing Stutter Loop** | Stutter + buzzing noise (fake computer crash) | ❌ MISSING |
| 5 | **Scrambling/Random Chopping** | Chop into frames, random order = gibberish | ✅ wb_beat_slicer partial |
| 6 | **Sentence Mixing** | Cut syllables/words, rearrange into new dialogue | ✅ wb_ytp.c sentence_mix |
| 7 | **Reverse** | Play clip backwards | ✅ wb_ytp.c reverse |
| 8 | **Ear-Rape** | Sudden extreme volume shock | ✅ wb_ytp.c earrape |
| 9 | **Bleep Censors** | Tone replaces word (implies swearing) | ❌ MISSING |
| 10 | **Meme Replacements** | Replace word with meme audio | ❌ MISSING |
| 11 | **Stare Down / Mysterious Zoom** | Freeze frame + zoom into face | ✅ wb_speed_ramp partial |
| 12 | **Panning** | Move clip around screen | ✅ wb_keyframes |
| 13 | **Paint Jobs** | MS Paint edits over frames | ❌ MISSING |
| 14 | **Tech Text** | Split-second on-screen text commentary | ✅ wb_text_animate |
| 15 | **SpaDinner** | Famous quotes as soundbites ("Dinner", "Lotsa Spaghetti") | ✅ wb_meme_sounds |
| 16 | **Sex-O-Phone** | Sexual tension + saxophone + visual FX | ❌ MISSING |
| 17 | **Dance Rave** | Character dancing + visual FX + techno/dubstep | ✅ wb_beat_sync |
| 18 | **Sequence Mixing** | Words reordered to form new (profane) sentences | ✅ wb_ytp.c sentence_mix |
| 19 | **Pitch Shifting** | Higher/lower audio | ✅ wb_pitch_bend.c |
| 20 | **Time Stretch** | Speed change without pitch | ✅ wb_timestretch.c |
| 21 | **Freeze Frame** | Single frame held extended | ✅ wb_keyframes |
| 22 | **Voice Transplant** | Replace character voice with another | ❌ MISSING |
| 23 | **Lip Syncing** | Visuals edited to match different audio | ❌ MISSING |
| 24 | **Datamosh** | Compression artifacts as aesthetic | ✅ wb_ytp.c datamosh |
| 25 | **Deep Fry** | Sharpen+saturate+contrast+noise | ✅ wb_deep_fry.c |
| 26 | **VHS Effect** | Tracking errors, chroma noise, scanlines | ✅ wb_vhs_effect.c |

### Advanced / Modern Techniques
| # | Technique | Description | BigMac Status |
|---|-----------|-------------|---------------|
| 27 | **YTPMV** | Music video from remixed clips | ✅ partial (lyric_video) |
| 28 | **YTP Tennis** | Back-and-forth remix chain | N/A (community format) |
| 29 | **YTP Collab** | Multi-creator single video | N/A (community format) |
| 30 | **Formant Shift** | Voice character change WITHOUT pitch change | ❌ MISSING |
| 31 | **Phoneme-Level Sentence Mix** | Splitting at phoneme boundaries for smooth mixes | ❌ MISSING |
| 32 | **Vocal Removal / Isolation** | Center channel extraction | ✅ wb_vocal_remove.c |
| 33 | **Beat-Slicer** | Chop audio to beats + rearrange | ✅ wb_beat_slicer.c |
| 34 | **Granular Freeze** | Sustain single grain forever | ❌ MISSING (wb_granular has freeze param) |
| 35 | **Sidechain Pumping** | Ducking compression triggered by other track | ✅ wb_sidechain.c |
| 36 | **Multiband Distortion** | Different distortion per frequency band | ✅ wb_multiband.c |
| 37 | **Tape Stop / Vinyl Brake** | Pitch+speed ramp to zero | ✅ wb_tape_stop.c |
| 38 | **Bitcrush / Sample Rate Reduction** | Lo-fi degradation | ✅ wb_bitcrush.c |
| 39 | **Chorus / Flanger / Phaser** | Modulation effects | ✅ wb_phaser.c, wb_chorus.c |
| 40 | **Wah-Wah / Envelope Filter** | Auto-wah / filter sweep | ❌ MISSING |
| 41 | **Compressor Models** | Optical, FET, VCA character | ✅ wb_comp.c |
| 42 | **Spectrum Visualizer** | Log-frequency bars, peak hold | ✅ wb_spectrum.c |
| 43 | **Audio-Reactive Zoom** | Zoom amount = bass energy | ✅ wb_audio_reactive.c |
| 44 | **Audio-Reactive Color** | Color shift = frequency content | ❌ MISSING |
| 45 | **Particle System** | Audio-reactive particles | ✅ wb_particle.c |
| 46 | **Kaleidoscope** | Mirrored/rotated segments | ❌ MISSING |
| 47 | **Source Stacking** | Layer multiple videos with blend modes | ✅ wb_compositor.c |
| 48 | **Split Screen** | Side-by-side reaction format | ✅ wb_reaction.c |
| 49 | **Picture-in-Picture** | Facecam overlay | ✅ wb_reaction.c |
| 50 | **Motion Tracking** | Track face/object, attach effects | ✅ wb_motion_track.c |

## 3. Source Material Catalog

### Tier 1: Most Popular YTP Sources
1. **Super Mario World** (TV series, 1991) — critically disregarded cartoon
2. **Adventures of Sonic the Hedgehog** (1993) — "pingas" meme origin
3. **Hotel Mario** (Philips CD-i, 1994) — "Nice of the princess to invite us over for a picnic, eh Luigi?"
4. **Link: The Faces of Evil / Zelda: Wand of Gamelon** (CD-i) — "Gee, it sure is boring around here"
5. **Super Mario Bros. 3** (Ruby-Spears cartoon, 1990) — "Recycled Koopa", "hot on our tail"
6. **SpongeBob SquarePants** (1999-present) — "Lotsa Spaghetti", "Dinner"
7. **Mega Man** (Ruby-Spears cartoon, 1994)
8. **Michael Rosen** children's poetry videos

### Tier 2: Common YTP Sources
9. **About Safety** (1970s educational) — "Guns Fucking Rule!"
10. **Steamboat Willie** (1928) — public domain Mickey
11. **Thomas the Tank Engine** (CGI era)
12. **Pokémon** (anime + games)
13. **Sonic Adventure 2** (cutscenes)
14. **Veggie Tales**
15. **Brain Age / Brain Training** (DS)
16. **Wii Shop Channel** music
17. **Windows Movie Maker** default titles/transitions
18. **Commercials** (especially 90s/2000s kids commercials)

### Tier 3: Subculture Sources
19. **Battle for Dream Island** (object show) — YTPMV source
20. **Donkey Kong Country** (Canadian cartoon)
21. **Kirby: Right Back At Ya!**
22. **Courage the Cowardly Dog**
23. **Ed, Edd n Eddy**
24. **Arthur** (PBS)
25. **Barney & Friends**
26. **Teletubbies**
27. **In the Night Garden**

### Sound Sources (from SpongeBob Edits Wiki)
- **Tenacious D** (album) — "City Hall", "Cock Pushups", "Double Team", "Explosivo", "Friendship", "Fuck Her Gently"
- **Hotel Mario** CD-i — "Nice of the princess...", "All toasters toast toast"
- **Wii Shop Channel** — iconic ambient music
- **Yahoo Messenger** — audible insult sounds
- **Staples Easy Button** — "That was easy"
- **Universal Studios rides** — "For all eternity!" (Revenge of the Mummy)
- **Tower of Terror** — scare chord
- **Toonami TOM** — "I love this job"

## 4. Key Memes & References

### The "Big Ones"
- **"Pingas"** — Stegblob's sentence-mix of "snooping as usual" → Sonic meme
- **"Lotsa Spaghetti"** — SpongeBob dinner scene
- **"Dinner"** — SpaDinner meme
- **"All toasters toast toast"** — Hotel Mario CD-i
- **"Nice of the princess to invite us"** — Hotel Mario
- **"Gee, it sure is boring around here"** — Zelda CD-i
- **"I'D SAY HE'S HOT ON OUR TAIL"** — first YTP on YouTube
- **"Morshu Gets A Car"** — YTP Movie genre classic
- **"The King Gets A Car"** — YTP Movie genre classic

### YTP Subgenres
- **YTPMV** — Music video remixes (otoMAD in Japan)
- **YTP Tennis** — Call-and-response remix chains
- **YTP Collab** — Multi-creator collaborations
- **YTP Shorties** — Short-form minis
- **RYTP** — Russian YouTube Poop
- **YTP FR** — French YouTube Poop
- **YTPH** — Spanish YouTube Poop
- **YTPBR** — Brazilian YouTube Poop

## 5. Gap Analysis: YTP Techniques → BigMac Modules

### HIGH PRIORITY (Missing, High YTP Impact)
| Technique | YTP Impact | Complexity | Module Target |
|-----------|-----------|------------|---------------|
| Formant Shift | ★★★★★ | Medium | wb_formant.c |
| Bleep Censor | ★★★★☆ | Low | wb_bleep.c |
| Paint Jobs / Frame Edit | ★★★☆☆ | Medium | wb_paint.c |
| Lip Sync / Mouth Remap | ★★★★☆ | High | wb_lipsync.c |
| Voice Transplant | ★★★★☆ | High | wb_voice_replace.c |
| Audio-Reactive Color | ★★★★☆ | Low | wb_audio_color.c |
| Kaleidoscope | ★★★☆☆ | Low | wb_kaleidoscope.c |
| Granular Freeze | ★★★★☆ | Low | wb_granular.c upgrade |
| Buzzing Stutter Loop | ★★★☆☆ | Low | wb_stutter.c upgrade |
| Stutter Loop Minus | ★★★☆☆ | Low | wb_stutter.c upgrade |

### MEDIUM PRIORITY
| Technique | YTP Impact | Complexity | Module Target |
|-----------|-----------|------------|---------------|
| Phoneme-Level Sentence Mix | ★★★★★ | High | wb_ytp.c upgrade |
| Sex-O-Phone Effect | ★★★☆☆ | Low | wb_sexophone.c |
| Wah-Wah / Envelope Filter | ★★★☆☆ | Medium | wb_wah.c |
| Audio-Reactive Color Grade | ★★★★☆ | Medium | wb_audio_grade.c |
| Frame Interpolation (smooth slow-mo) | ★★★★☆ | High | wb_interpolate.c |
| Automatic Meme Replacement | ★★★☆☆ | Medium | wb_meme_replace.c |

## 6. What BigMac Already Has (YTP-Relevant)
✅ wb_ytp.c — stutter_loop, sentence_mix, pitch_shift, earrape, reverse, time_stretch, word_salad, datamosh, vine_boom, insert_soundbite
✅ wb_stutter.c — 10 stutter types
✅ wb_pitch_bend.c — continuous pitch slides
✅ wb_timestretch.c — speed change
✅ wb_beat_slicer.c — chop/reverse/stutter/shuffle/speed
✅ wb_deep_fry.c — multi-pass saturation
✅ wb_vhs_effect.c — tracking errors, chroma noise
✅ wb_sidechain.c — feed-forward detector
✅ wb_multiband.c — frequency-dependent compression
✅ wb_tape_stop.c — vinyl brake
✅ wb_bitcrush.c — lo-fi
✅ wb_phaser.c, wb_chorus.c — modulation
✅ wb_spectrum.c — visualizer
✅ wb_audio_reactive.c — audio-reactive effects
✅ wb_particle.c — particles
✅ wb_motion_track.c — tracking
✅ wb_reaction.c — split screen, PIP
✅ wb_vocal_remove.c — center channel extraction
✅ wb_meme_sounds.c — procedural sounds
✅ wb_beat_sync.c — onset detection, BPM
✅ wb_auto_captions.c — word highlight
✅ wb_chroma_key.c — green screen
✅ wb_text_animate.c — kinetic text
✅ wb_speed_ramp.c — smooth speed
✅ wb_transitions.c — 25 transitions
✅ wb_color_grade.c — lift/gamma/gain
✅ wb_keyframes.c — bezier animation
✅ wb_compositor.c — node compositing
✅ wb_video_edit.c — full NLE

## 10. Iconic YTP Source Material (Acquired)

### Tier 1 — The Holy Grail (acquired)
- **Hotel Mario CD-i** — hilariously bad cutscenes, "Nice of the princess to invite us over for a picnic, eh Luigi?"
- **Super Mario World cartoon (1991)** — all 13 episodes, critically disregarded
- **Zelda CD-i** — Link: The Faces of Evil, Zelda: The Wand of Gamelon
- **Popeye PD cartoons** — 8 episodes (Fleischer)
- **Ub Iwerks Comicolor** — 20+ cartoons (1930s)

### Tier 2 — YTP Staples (acquired)
- **SpongeBob SquarePants** — compilation episodes via YouTube
- **Pokémon anime** — Indigo League (10 eps) + Sun&Moon (5 eps)
- **90s Commercials** — Game Genie, Pepsi, Fruity Pebbles, McDonald's, etc.
- **Vintage Commercials** — NBC 1987, CBS 1994, ABC 1987, Animaniacs 1993, Batman 1993
- **Nickelodeon IDs** — bumpers, claymation, rebrands

### Tier 3 — Still Needed
- **Adventures of Sonic the Hedgehog** — downloading
- **Michael Rosen** — children's poet, YTP staple
- **Wall-E** — popular YTP source
- **Dragon Ball Z Abridged** — Team Four Star
- **Hotel Mario full game** — not just cutscenes
