#!/usr/bin/env python3
"""
tools/vgm_top10k.py — Build a curated top-10,000 VGMusic MIDI list.

Filters the VGMusic catalog for the most iconic/popular video game songs.
Uses game franchise popularity heuristics + file availability.

Output: TSV file (console, game, filename) ready for vgm_catalog.py --download
"""

import os
import sys
import re
import random

# Priority franchises (most popular VGM by cultural impact)
# Higher score = higher priority
FRANCHISE_PRIORITY = {
    # Tier 1: The absolute most iconic
    "super mario": 100, "zelda": 100, "pokemon": 95,
    "sonic": 90, "mega man": 90, "final fantasy": 90,
    "castlevania": 85, "metroid": 85, "street fighter": 80,
    "chrono trigger": 95, "earthbound": 80,
    
    # Tier 2: Very popular
    "kirby": 75, "donkey kong": 75, "kirby's": 75,
    "ff7": 85, "ff6": 80, "ff4": 70,
    "mario kart": 70, "smash bros": 70,
    "chrono": 75, "secretof mana": 70,
    "actraiser": 65, "illusion of gaia": 65,
    "terranigma": 65, "breath of fire": 65,
    
    # Tier 3: Popular
    "mega man x": 60, "rockman": 60,
    "sonic the hedgehog": 70,
    "bomberman": 50, "contra": 50,
    "gradius": 50, "ninja gaiden": 50,
    "tetris": 60, "dragon quest": 60,
    "zelda link": 80, "link to the past": 80,
    "ocarina of time": 85, "majoras mask": 75,
    
    # Tier 4: Niche but beloved
    "act razer": 40, "soul blazer": 40,
    "triple play": 30, "nba jam": 30,
}

def score_filename(filename):
    """Score a MIDI filename by franchise popularity."""
    name_lower = filename.lower().replace('_', ' ').replace('-', ' ')
    score = 0
    for franchise, priority in FRANCHISE_PRIORITY.items():
        if franchise in name_lower:
            score = max(score, priority)
    return score

def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--catalog', type=str, help='Catalog file')
    parser.add_argument('--output', type=str, default='vgm_top10k.txt')
    parser.add_argument('--count', type=int, default=10000)
    args = parser.parse_args()

    if not args.catalog:
        print("Need --catalog file", file=sys.stderr)
        sys.exit(1)

    # Read catalog
    entries = []
    with open(args.catalog) as f:
        for line in f:
            parts = line.strip().split('\t')
            if len(parts) != 3:
                continue
            console, game, filename = parts
            score = score_filename(filename) + score_filename(game)
            entries.append((score, console, game, filename))

    # Sort by score descending, then random within same score
    random.seed(42)  # reproducible
    entries.sort(key=lambda x: (-x[0], random.random()))

    # Take top N
    top = entries[:args.count]

    # Write output
    with open(args.output, 'w') as f:
        for score, console, game, filename in top:
            f.write(f"{console}\t{game}\t{filename}\n")

    print(f"Wrote {len(top)} entries to {args.output}", file=sys.stderr)
    
    # Print distribution
    from collections import Counter
    console_counts = Counter(e[1] for e in top)
    print("\nDistribution by console:", file=sys.stderr)
    for console, count in console_counts.most_common():
        print(f"  {console}: {count}", file=sys.stderr)
    
    score_dist = Counter(e[0] for e in top)
    print("\nScore distribution:", file=sys.stderr)
    for score in sorted(score_dist.keys(), reverse=True)[:10]:
        print(f"  score {score}: {score_dist[score]} files", file=sys.stderr)

if __name__ == '__main__':
    main()
