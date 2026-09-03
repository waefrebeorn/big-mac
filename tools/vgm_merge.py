#!/usr/bin/env python3
"""tools/vgm_merge.py — Merge filename + MIDI metadata categorization.
Analyzes overlaps, finds most popular titles, genre variety."""

import os, sys
from collections import Counter, defaultdict

def load_cat(path):
    """Load categorized TSV: console, game, filename"""
    entries = {}
    with open(path) as f:
        for line in f:
            parts = line.strip().split('\t')
            if len(parts) >= 3:
                key = (parts[0], parts[2])  # (console, filename)
                entries[key] = parts[1]
    return entries

def load_meta(path):
    """Load MIDI metadata TSV: console, game, filename, text"""
    entries = {}
    with open(path) as f:
        for line in f:
            parts = line.strip().split('\t')
            if len(parts) >= 4:
                key = (parts[0], parts[2])
                entries[key] = (parts[1], parts[3])
    return entries

def main():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('--filename-cat', default='~/vgm/vgm_categorized.tsv')
    p.add_argument('--midi-meta', default='~/vgm/vgm_midi_metadata.tsv')
    p.add_argument('--output', default='~/vgm/vgm_final_categorized.tsv')
    args = p.parse_args()
    
    fn_cat = load_cat(os.path.expanduser(args.filename_cat))
    midi_meta = load_meta(os.path.expanduser(args.midi_meta))
    
    # Merge: use filename cat if known, else MIDI metadata, else unknown
    merged = []
    overlap = Counter()
    both_known = 0
    fn_only = 0
    midi_only = 0
    neither = 0
    
    all_keys = set(fn_cat.keys()) | set(midi_meta.keys())
    
    for key in sorted(all_keys):
        console, filename = key
        fn_game = fn_cat.get(key, '_unknown_')
        midi_info = midi_meta.get(key)
        midi_game = midi_info[0] if midi_info else '_unknown_'
        midi_text = midi_info[1] if midi_info else ''
        
        if fn_game != '_unknown_' and midi_game != '_unknown_':
            both_known += 1
            # If they agree, great. If not, prefer filename (more specific)
            game = fn_game
            overlap[(fn_game, midi_game)] += 1
        elif fn_game != '_unknown_':
            fn_only += 1
            game = fn_game
        elif midi_game != '_unknown_':
            midi_only += 1
            game = midi_game
        else:
            neither += 1
            game = '_unknown_'
        
        merged.append((console, game, filename, midi_text))
    
    # Write merged output
    out_path = os.path.expanduser(args.output)
    with open(out_path, 'w') as f:
        for console, game, filename, text in merged:
            f.write(f'{console}\t{game}\t{filename}\t{text}\n')
    
    # Stats
    gc = Counter(g for _, g, _, _ in merged if g != '_unknown_')
    total = len(merged)
    categorized = total - neither
    
    print(f'=== MERGE RESULTS ===', file=sys.stderr)
    print(f'Total files: {total}', file=sys.stderr)
    print(f'Categorized: {categorized} ({100*categorized//total}%)', file=sys.stderr)
    print(f'  Both methods: {both_known}', file=sys.stderr)
    print(f'  Filename only: {fn_only}', file=sys.stderr)
    print(f'  MIDI only: {midi_only}', file=sys.stderr)
    print(f'Uncategorized: {neither}', file=sys.stderr)
    
    print(f'\n=== TOP 50 GAMES ===', file=sys.stderr)
    for g, c in gc.most_common(50):
        print(f'  {g}: {c}', file=sys.stderr)
    
    print(f'\n=== OVERLAPS (filename game -> MIDI game) ===', file=sys.stderr)
    print(f'(Where both methods agree or disagree)', file=sys.stderr)
    for (fn, midi), c in overlap.most_common(30):
        marker = '✓' if fn == midi else '✗'
        print(f'  {marker} {fn} -> {midi}: {c}', file=sys.stderr)
    
    # Genre analysis
    genres = defaultdict(int)
    genre_keywords = {
        'Platformer': ['mario', 'sonic', 'kirby', 'megaman', 'mega man', 'dkc', 'donkey kong', 'platform'],
        'RPG': ['final fantasy', 'ff', 'chrono', 'pokemon', 'zelda', 'fire emblem', 'suikoden', 'xenogears', 'skyrim', 'elder scrolls', 'fallout', 'baldur', 'wizardry', 'ultima', 'might and magic', 'dragon quest', 'dq', 'persona', 'dark souls', 'elden ring', 'bloodborne'],
        'Fighting': ['street fighter', 'sf2', 'tekken', 'king of fighters', 'fatal fury', 'samurai shodown', 'darkstalkers', 'guilty gear', 'blazblue', 'melty blood', 'soul calibur', 'mortal kombat'],
        'Action/Adventure': ['metroid', 'castlevania', 'resident evil', 'silent hill', 'tomb raider', 'metal gear', 'crash', 'spyro', 'banjo'],
        'Racing': ['mario kart', 'f-zero', 'gran turismo', 'need for speed', 'burnout'],
        'Puzzle': ['tetris', 'puyo', 'columns', 'lemmings', 'dr mario'],
        'Shooter': ['doom', 'quake', 'halflife', 'half-life', 'counter.strike', 'team fortress', 'duke nukem', 'wolfenstein', 'unreal', 'star fox', 'starwing', '1942', '1943', 'gradius', 'rtype', 'salamander', 'thunder force', 'axelay', 'aleste'],
        'Simulation': ['simcity', 'civilization', 'age of empires', 'warcraft', 'starcraft', 'command and conquer', 'x-com', 'master of heroes', 'populous', 'theme park', 'rollercoaster', 'tycoon'],
        'Rhythm': ['rhythm heaven', 'beatmania', 'ddr', 'dance dance', 'parappa', 'gitadora', 'popn'],
        'Horror': ['resident evil', 'silent hill', 'alone in the dark', 'fatal frame', 'clock tower', 'haunting ground'],
        'Strategy': ['advance wars', 'fire emblem', 'fft', 'final fantasy tactics', 'shining force', 'langrisser', 'super robot wars', 'disgaea'],
        'Sports': ['fifa', 'madden', 'nba', 'nhl', 'tony hawk', 'skate', 'ssx', 'wii sports', 'mario tennis', 'mario golf', 'mario sports'],
    }
    
    for game, count in gc.items():
        gl = game.lower()
        matched = False
        for genre, keywords in genre_keywords.items():
            if any(kw in gl for kw in keywords):
                genres[genre] += count
                matched = True
                break
        if not matched:
            genres['Other'] += count
    
    print(f'\n=== GENRE DISTRIBUTION ===', file=sys.stderr)
    for genre, count in sorted(genres.items(), key=lambda x: -x[1]):
        print(f'  {genre}: {count} ({100*count//categorized}%)', file=sys.stderr)

if __name__ == '__main__':
    main()
