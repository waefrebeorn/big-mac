# R080 — YTP Source Material: Master Acquisition List

## Priority 1: The Holy Grail (Iconic YTP Sources)

| Source | Status | Notes |
|--------|--------|-------|
| Hotel Mario CD-i | ✅ ACQUIRED | All cutscenes (14MB) |
| Super Mario World cartoon | ✅ ACQUIRED | 6+ episodes downloading |
| Zelda CD-i (Faces of Evil + Wand of Gamelon) | ⬜ NEEDED | Cutscenes on archive.org |
| Link: The Faces of Evil | ⬜ NEEDED | |
| Zelda: The Wand of Gamelon | ⬜ NEEDED | |
| Adventures of Sonic the Hedgehog | ⬜ NEEDED | 65 episodes |
| Super Mario Bros. 3 cartoon (DiC) | ⬜ NEEDED | "Recycled Koopa" episode |
| Captain N: The Game Master | ⬜ NEEDED | Seasons 1-3 |

## Priority 2: Classic YTP Staples

| Source | Status | Notes |
|--------|--------|-------|
| SpongeBob SquarePants | ✅ ACQUIRED | Compilations via YouTube |
| Pokémon anime | ✅ ACQUIRED | Indigo League + Sun&Moon |
| Popeye (Fleischer) | ✅ ACQUIRED | 8 episodes PD |
| Ub Iwerks Comicolor | ✅ ACQUIRED | 20+ cartoons |
| 1956 Public Domain cartoons | ✅ ACQUIRED | 10 episodes |
| Michael Rosen | ⬜ NEEDED | Children's poet, YTP staple |
| Wall-E | ⬜ NEEDED | Popular YTP source |
| Dragon Ball Z Abridged | ⬜ NEEDED | Team Four Star |
| The Incredibles ("Jack-Jack Attack") | ⬜ NEEDED | Tennis staple |
| Steamboat Willie | ⬜ NEEDED | 1928, first YTP of it exists |
| Thomas the Tank Engine | ⬜ NEEDED | Classic YTP source |
| Dora the Explorer | ⬜ NEEDED | |
| Arthur | ⬜ NEEDED | |
| SpongeBob "Shanghaied" | ⬜ NEEDED | Specific famous episode |
| "The Sky Had a Weegee" source | ⬜ NEEDED | Hurricoaster's famous YTP |

## Priority 3: Commercials & Bumpers

| Source | Status | Notes |
|--------|--------|-------|
| 90s Nickelodeon bumpers | ✅ ACQUIRED | IDs, claymation |
| Kids WB bumpers | ✅ ACQUIRED | Tia Tamera Mowry |
| TGIF bumpers | ✅ ABC/NBC/CBS | Various |
| Vintage commercials (80s-00s) | ✅ ACQUIRED | 100+ ads |
| Nintendo commercials | ⬜ NEEDED | N64, GameCube ads |
| Sega commercials | ⬜ NEEDED | Genesis, Saturn, Dreamcast |
| PlayStation commercials | ⬜ NEEDED | PS1, PS2 era |
| cereal commercials | ⬜ NEEDED | Many are YTP staples |

## Priority 4: Internet Culture & Meme Sources

| Source | Status | Notes |
|--------|--------|-------|
| "Pingas" | ⬜ NEEDED | Uncle Dad, iconic YTP sound |
| "Me amo boat" / "Steamboat Willie" | ⬜ NEEDED | |
| "The fox says" | ⬜ NEEDED | |
| "Caramelldansen" | ⬜ NEEDED | |
| "Narwhal bacon" | ⬜ NEEDED | |
| "Techno Viking" | ⬜ NEEDED | |
| "Charlie the Unicorn" | ⬜ NEEDED | |
| "Dick in a Box" | ⬜ NEEDED | |
| "Chocolate Rain" | ⬜ NEEDED | |
| "Numa Numa" | ⬜ NEEDED | |
| "Rick Astley" | ✅ ACQUIRED | |
| "Shoes" (Kelly) | ⬜ NEEDED | |
| "David After Dentist" | ⬜ NEEDED | |
| "Leave Britney Alone" | ⬜ NEEDED | |
| "Double Rainbow" | ⬜ NEEDED | |

## Priority 5: YTP Tennis Specific

| Source | Status | Notes |
|--------|--------|-------|
| Tennis match archives | ⬜ NEEDED | YouWouldBeWelcome YouTube |
| Classic tennis serves | ⬜ NEEDED | conradslater, RabbitSnore |
| "5" by MycroProcessor | ⬜ NEEDED | Defining avant-garde work |

## Acquisition Methods

### Method 1: Archive.org (most reliable)
- Search: `site:archive.org [source name] mp4`
- Direct download via metadata API
- No rate limit issues with sequential downloads

### Method 2: YouTube via yt-dlp
- Requires deno JS runtime
- Works for ~30% of searches (rest are geoblocked/unavailable)
- Best for: compilation videos, fan uploads

### Method 3: Reddit/Forum hunting
- r/YTP, r/YouTubePoop, r/gaming, r/lostmedia
- Search for Google Drive links, MEGA links
- Use browser to access

### Method 4: Browser scraping
- Direct navigation to pages with embedded video
- Download via browser tools
- Use for: Reddit posts, forum threads, wiki pages

### Method 5: Google Drive links
- Often shared on Reddit/Forums
- Can be downloaded directly
- Use browser to resolve sharing restrictions

## Transcoding Pipeline

All sources must be transcoded to:
- **Resolution**: 480p max
- **Clip length**: 8-10 seconds
- **Bitrate**: ~500kbps video, ~64kbps audio
- **Format**: H.264 + AAC in MP4
- **Storage**: All clips in single `all_clips/` directory
- **Naming**: `{source_name}_{timestamp}.mp4`

This keeps the library manageable while preserving editability.
