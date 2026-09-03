#!/usr/bin/env python3
"""tools/vgm_midiscan.py — Scan MIDI files for game metadata in track names."""

import os, sys, struct, re
from collections import Counter

def read_vlq(data, pos):
    value = 0
    for _ in range(4):
        if pos >= len(data):
            break
        b = data[pos]
        pos += 1
        value = (value << 7) | (b & 0x7F)
        if not (b & 0x80):
            break
    return value, pos

def scan_midi(filepath):
    try:
        with open(filepath, 'rb') as f:
            data = f.read()
    except:
        return None
    if len(data) < 14 or data[:4] != b'MThd':
        return None
    
    pos = 8 + struct.unpack('>I', data[4:8])[0]
    texts = []
    
    while pos < len(data) - 8:
        if data[pos:pos+4] != b'MTrk':
            pos += 1
            continue
        track_len = struct.unpack('>I', data[pos+4:pos+8])[0]
        track_end = min(pos + 8 + track_len, len(data))
        pos += 8
        
        while pos < track_end - 2:
            delta, pos = read_vlq(data, pos)
            if pos >= track_end:
                break
            if pos >= len(data):
                break
            
            status = data[pos]
            if status < 0x80:
                pos += 1
                continue
            pos += 1
            if pos >= len(data):
                break
            
            try:
                if status == 0xFF:
                    meta_type = data[pos] if pos < len(data) else 0
                    pos += 1
                    meta_len, pos = read_vlq(data, pos)
                    if 0 < meta_len <= track_end - pos:
                        md = data[pos:pos+meta_len]
                        if meta_type in (0x01, 0x02, 0x03):
                            t = md.decode('ascii', errors='replace').strip()
                            if 0 < len(t) < 200:
                                texts.append(t)
                        pos += meta_len
                    else:
                        break
                elif status >= 0xF0:
                    sl, pos = read_vlq(data, pos)
                    pos = min(pos + sl, track_end)
                else:
                    et = status & 0xF0
                    pos += 1 if et in (0xC0, 0xD0) else 2
            except:
                break
        pos = track_end
    
    return texts or None

SKIP = {'by ', 'sequenced by', 'sequence by', 'arranged by', 'transcribed by',
        'composed by', 'original by', 'midi by', 'vgmusic', 'vgma',
        'game', 'from the game', 'title', 'intro', 'ending', 'boss',
        'stage', 'level', 'world', 'theme', 'battle', 'final', 'map',
        'select', 'credits', 'overworld', 'dungeon', 'temple', 'castle',
        'house', 'room', 'zone', 'area', 'round', 'phase', 'act',
        'main menu', 'title screen', 'game over', 'continue', 'options',
        'prologue', 'epilogue', 'opening', 'closing', 'staff roll',
        'prelude', 'fanfare', 'jingle', 'remix', 'cover', 'version',
        'original', 'ost', 'bgm', 'sfx', 'sound effect', 'sequence tag'}

def extract_game(texts):
    for text in texts:
        tl = text.lower().strip()
        if len(tl) < 3 or len(tl) > 60:
            continue
        if any(tl.startswith(s) for s in SKIP):
            continue
        if tl in SKIP:
            continue
        # Look for "Game - Song" pattern
        for sep in [' - ', ' – ', ': ', ' | ', ' ~ ']:
            if sep in text:
                c = text.split(sep)[0].strip()
                cl = c.lower()
                if 2 < len(c) < 60 and not any(cl.startswith(s) for s in SKIP):
                    return c
        # If it contains game-like keywords, use the whole text
        game_kws = ['mario', 'zelda', 'pokemon', 'sonic', 'megaman', 'mega man',
                    'final fantasy', 'chrono', 'castlevania', 'metroid', 'kirby',
                    'donkey kong', 'street fighter', 'tekken', 'king of fighters',
                    'resident evil', 'silent hill', 'metal gear', 'tomb raider',
                    'crash', 'spyro', 'banjo', 'xenogears', 'suikoden',
                    'fire emblem', 'breath of fire', 'secret of mana',
                    'illusion of gaia', 'terranigma', 'actraiser', 'soul blazer',
                    'vagrant story', 'legend of legaia', 'valkyrie profile',
                    'bahamut', 'doom', 'quake', 'halflife', 'half-life',
                    'counter-strike', 'team fortress', 'duke nukem', 'wolfenstein',
                    'unreal', 'warcraft', 'diablo', 'starcraft', 'age of empires',
                    'civilization', 'simcity', 'tetris', 'pac-man', 'galaga',
                    'gradius', 'rtype', 'contra', 'ninja gaiden', 'tmnt',
                    'battletoads', 'gunstar', 'shinobi', 'comix zone', 'ecco',
                    'columns', 'puyo', 'lemmings', 'prince of persia', 'dizzy',
                    'monkey island', 'space quest', 'kings quest', 'zork', 'myst',
                    'riven', 'undertale', 'deltarune', 'hollow knight', 'celeste',
                    'stardew valley', 'cuphead', 'ori', 'rayman', 'f-zero',
                    'star fox', 'pilotwings', 'animal crossing', 'rhythm heaven',
                    'wario ware', 'brain age', 'nintendogs', 'advance wars',
                    'golden sun', 'mystery dungeon', 'pokemon ranger', 'persona',
                    'dark souls', 'elden ring', 'bloodborne', 'sekiro', 'yoshi',
                    'diddy', 'jet force', 'perfect dark', 'goldeneye', 'banjo-kazooie',
                    'paper mario', 'mario party', 'mario tennis', 'mario golf',
                    'kirby super star', 'kirby dream land', 'mega man x',
                    'mega man zero', 'mega man battle', 'mega man star',
                    'chrono cross', 'chrono trigger', 'xenogears', 'suikoden',
                    'vagrant story', 'legend of legaia', 'valkyrie profile',
                    'bahamut lagoon', 'breath of fire', 'secret of mana',
                    'seiken densetsu', 'trials of mana', 'live a live',
                    'radical dreamers', 'illusion of gaia', 'terranigma',
                    'actraiser', 'soul blazer', 'street fighter alpha',
                    'street fighter iii', 'street fighter iv', 'fatal fury',
                    'samurai shodown', 'darkstalkers', 'guilty gear', 'blazblue',
                    'melty blood', 'soul calibur', 'soul edge', 'virtua fighter',
                    'daytona', 'outrun', 'after burner', 'thunder force',
                    'alex kidd', 'wonder boy', 'adventure island', 'blaster master',
                    'gauntlet', 'ghosts', 'ghouls', 'final fight', 'punisher',
                    '1942', '1943', '19xx', 'rtype', 'gradius', 'salamander',
                    'parodius', 'twin bee', 'axelay', 'aleste', 'shinobi',
                    'ninja gaiden', 'tmnt', 'battletoads', 'double dragon',
                    'gunstar', 'comix zone', 'ecco', 'columns', 'puyo', 'lemmings',
                    'prince of persia', 'dizzy', 'monkey island', 'space quest',
                    'kings quest', 'zork', 'myst', 'riven', 'undertale', 'deltarune',
                    'hollow knight', 'celeste', 'stardew valley', 'cuphead', 'ori',
                    'rayman', 'f-zero', 'star fox', 'pilotwings', 'animal crossing',
                    'rhythm heaven', 'wario ware', 'brain age', 'nintendogs',
                    'advance wars', 'golden sun', 'mystery dungeon', 'pokemon ranger',
                    'persona', 'dark souls', 'elden ring', 'bloodborne', 'sekiro']
        if any(kw in tl for kw in game_kws):
            return text
    return None

def main():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('--dir', default='~/vgm')
    p.add_argument('--output', default='vgm_midi_metadata.tsv')
    p.add_argument('--limit', type=int, default=0)
    args = p.parse_args()
    
    base = os.path.expanduser(args.dir)
    results, scanned, found = [], 0, 0
    
    for root, dirs, files in os.walk(base):
        for fn in files:
            if not fn.lower().endswith('.mid'):
                continue
            if args.limit and scanned >= args.limit:
                break
            fp = os.path.join(root, fn)
            console = os.path.relpath(root, base).split(os.sep)[0]
            texts = scan_midi(fp)
            scanned += 1
            game = extract_game(texts) if texts else None
            if game:
                found += 1
            results.append((console, fn, game or '_unknown_', (texts[0] if texts else '').replace('\t',' ')))
            if scanned % 5000 == 0:
                print(f'{scanned} scanned, {found} found...', file=sys.stderr)
    
    with open(args.output, 'w') as f:
        for c, fn, g, t in results:
            f.write(f'{c}\t{g}\t{fn}\t{t}\n')
    
    gc = Counter(g for _, g, _, _ in results if g != '_unknown_')
    print(f'\nScanned {scanned}, found {found} ({100*found//scanned}%)', file=sys.stderr)
    print(f'Top 30:', file=sys.stderr)
    for g, c in gc.most_common(30):
        print(f'  {g}: {c}', file=sys.stderr)

if __name__ == '__main__':
    main()
