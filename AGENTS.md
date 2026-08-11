# AGENTS.md — Big Mac (THE VOICE)

> Agent context file. Read this before working in this repo. Kept current;
> update it when the structure changes.

## What this repo is

Big Mac is the resident agent of the 2012 iMac — the **musical agent** of
the WuBu ecosystem. It SPEAKS and SINGS via pure C11 articulatory
synthesis (Pink Trombone-style vocal tract waveguide). One agent, one
repo, one mission: **the voice engine**.

**One sentence:** Big Mac makes the physics of the human voice in C11 —
no samples, no vocal banks, no third party.

## Hard rules (sealed edicts)

1. **Everything in C11.** `-std=c11`, opaque structs, minimal includes,
   no god headers, no C++. Python only as scratchpad, never shipped.
2. **No third party if we can write it.** Self-contained. The WAV writer,
   the math (exp/sin), the noise source — all ours.
3. **No stubs.** Every called function does real work. A render must
   produce real audio bytes.
4. **Verify before claiming.** Run the tool, read the FAIL lines, report
   real numbers (peaks, RMS, realtime factor). Never cite stale counts.
5. **Pull, never push.** The WuBu ecosystem repos (wubuwizard, WuBuOS,
   slermes, WuBuOffice, WuBuPad, wubufw-tools) are OTHER agents' work.
   Reference them freely. NEVER push to them. This repo is the only one
   Big Mac writes to.
6. **Research discipline.** Docs are written 7-hop (Kevin-Bacon): 7
   sources → converge → apply → compare → tune. Status: `wired` =
   implemented+tested, `open` = gap.

## The mission pipeline

```
text → phones → articulation (teeth/lips/tongue/velum) → waveguide → WAV/AIFF
                                                                → MIDI (GarageBand)
```

- The tract: 44-section Kelly-Lochbaum waveguide (+ nasal branch)
- The glottis: LF glottal flow model (R++ variant planned)
- Voice quality: jitter, shimmer, aspiration, vibrato, whisper, singing
- Output: 16-bit PCM WAV / AIFF for GarageBand; MIDI pitch track

## Directory map

| Path | What lives there |
|------|------------------|
| `src/wb_*.c` | C11 engine modules (tract, glottis, articulation, wav, render) |
| `include/wb_*.h` | Public API — opaque types + function decls |
| `tools/*.c` | CLI tools (`wb_speak`, `wb_sing`, `wb_midi`) |
| `tests/*.c` | Test drivers (one per subsystem) |
| `docs/` | 7-hop research articles, architecture |

## Build & test

```bash
make all          # full build
make test         # the gate — run before claiming anything works
make test_<name>  # one subsystem (test_tract, test_glottis, test_wav)
```

## License

WaefreBeorn Umbrella License v3.0 — see LICENSE.
