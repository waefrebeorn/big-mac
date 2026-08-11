#!/usr/bin/env python3
"""Trim CMUdict to pure-alpha words (no digits/apostrophes) for fast builds."""
def h(s):
    hv = 2166136261
    for c in s.encode():
        hv ^= c
        hv = (hv * 16777619) & 0xFFFFFFFF
    return hv

seen = {}
with open("data/cmudict.dict", encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if not line or line.startswith(";;;"):
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            continue
        word, phones = parts[0], parts[1].strip()
        if word.isalpha() and len(word) <= 12:
            w = word.lower()
            if w not in seen:
                seen[w] = phones

items = sorted(seen.items(), key=lambda kv: h(kv[0]))
with open("data/tts_dict.h", "w") as f:
    f.write("/* Auto-generated from CMUdict (public domain, Carnegie Mellon).\n")
    f.write(" * word (FNV-1a hash) -> ARPABET phoneme string. Binary search. */\n")
    f.write("typedef struct { unsigned int hash; const char *phones; } wb_dict_entry_t;\n")
    f.write(f"static const wb_dict_entry_t WB_DICT[{len(items)}] = {{\n")
    for w, ph in items:
        f.write(f'    {{0x{h(w):08x}u, "{ph}"}},\n')
    f.write("};\n")
    f.write(f"#define WB_DICT_N {len(items)}\n")
print(f"wrote data/tts_dict.h with {len(items)} entries")
