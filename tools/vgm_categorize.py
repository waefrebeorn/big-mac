#!/usr/bin/env python3
"""tools/vgm_categorize.py — Categorize all VGMusic MIDI files by game."""

import os, sys, re
from collections import Counter

# (regex_pattern, game_name) — order matters: first match wins
PATTERNS = [
    # Pokemon (before other patterns that might match)
    (r'(pokemon|poke|pkMn)', 'Pokemon'),
    (r'(pokered|red_blue)', 'Pokemon R/B'),
    (r'(pokemongold|gold_silver)', 'Pokemon G/S'),
    (r'(pokemonruby|ruby_sapphire)', 'Pokemon R/S'),
    (r'(diamond_pearl)', 'Pokemon D/P'),
    (r'(black_white)', 'Pokemon B/W'),
    (r'(x_y|pokemonxy)', 'Pokemon X/Y'),
    (r'(sun_moon|pokemonsun)', 'Pokemon S/M'),
    (r'(sword_shield)', 'Pokemon S/S'),
    # Mario
    (r'(smw|super_mario_world)', 'Super Mario World'),
    (r'(smb|super_mario_bros)', 'Super Mario Bros'),
    (r'(sm64|super_mario_64)', 'Super Mario 64'),
    (r'(smk|mario_kart|mario_kart_64)', 'Mario Kart'),
    (r'(smrpg|mario_rpg)', 'Super Mario RPG'),
    (r"(yoshi|yarn)", "Yoshi's Island"),
    (r'(mario_paint)', 'Mario Paint'),
    # Zelda
    (r'(lttp|alttp|link_to_the_past)', 'Zelda: LttP'),
    (r'(oot|ocarina)', 'Zelda: OoT'),
    (r'(mm|majora)', 'Zelda: MM'),
    (r'(botw|breath)', 'Zelda: BotW'),
    (r'(ss|skyward)', 'Zelda: SS'),
    (r'(ww|wind_waker)', 'Zelda: WW'),
    (r'(ltp|twilight)', 'Zelda: TP'),
    (r'(loz|zelda)', 'Zelda'),
    # Final Fantasy
    (r'(ff7|ffvii|final_fantasy_vii)', 'FF7'),
    (r'(ff6|ffiii|final_fantasy_vi)', 'FF6'),
    (r'(ff4|ffii|final_fantasy_iv)', 'FF4'),
    (r'(ff5|final_fantasy_v)', 'FF5'),
    (r'(ff8|ffviii|final_fantasy_viii)', 'FF8'),
    (r'(ff9|ffix|final_fantasy_ix)', 'FF9'),
    (r'(ff10|ffx|final_fantasy_x)', 'FF10'),
    (r'(fft|final_fantasy_tactics)', 'FF Tactics'),
    (r'(ff|final_fantasy)', 'Final Fantasy'),
    # Mega Man
    (r'(mmx|mega_man_x)', 'Mega Man X'),
    (r'(mm|mega_man)', 'Mega Man'),
    (r'(rockman)', 'Mega Man'),
    # Sonic
    (r'(sonic|sonic[123]|s[123])', 'Sonic'),
    (r'(sadv|sa[12])', 'Sonic Adventure'),
    # Castlevania
    (r'(sotn|symphony)', 'CV: SOTN'),
    (r'(aria|soaria)', 'CV: Aria'),
    (r'(castlevania|cv[1-9]?)', 'Castlevania'),
    # Metroid
    (r'(super_metroid)', 'Super Metroid'),
    (r'(prime|mp[123])', 'Metroid Prime'),
    (r'(metroid)', 'Metroid'),
    # Chrono
    (r'(chrono_trigger|ct)', 'Chrono Trigger'),
    (r'(chrono_cross|cs)', 'Chrono Cross'),
    # Street Fighter
    (r'(sf2|street_fighter_2)', 'SF2'),
    (r'(street_fighter|sf)', 'Street Fighter'),
    # Donkey Kong
    (r'(dkc|dk[123]|donkey_kong_country)', 'DKC'),
    (r'(donkey_kong|dk)', 'Donkey Kong'),
    # Kirby
    (r"(kirby|kirby's|ksa)", 'Kirby'),
    # EarthBound
    (r'(earthbound|eb)', 'EarthBound'),
    (r'(mother[123]?)', 'Mother'),
    # Other SNES
    (r'(actraiser)', 'ActRaiser'),
    (r'(soul_blazer)', 'Soul Blazer'),
    (r'(illusion_of_gaia)', 'Illusion of Gaia'),
    (r'(terranigma)', 'Terranigma'),
    (r'(secreto?|secret_of_mana)', 'Secret of Mana'),
    (r'(xenogears)', 'Xenogears'),
    (r'(suikoden)', 'Suikoden'),
    (r"(breath_of_fire|bof[1-5])", 'Breath of Fire'),
    (r'(live_a_live)', 'Live A Live'),
    (r'(vagrant_story)', 'Vagrant Story'),
    (r'(legend_of_legaia)', 'Legend of Legaia'),
    (r'(valkyrie_profile)', 'Valkyrie Profile'),
    (r"(bahamut|bl[1-6])", 'Bahamut Lagoon'),
    (r'(star_fox|sf[12])', 'Star Fox'),
    (r'(f_zero)', 'F-Zero'),
    (r'(pilotwings)', 'Pilotwings'),
    (r'(simcity)', 'SimCity'),
    # Other
    (r'(undertale)', 'Undertale'),
    (r'(deltarune)', 'Deltarune'),
    (r'(doom)', 'DOOM'),
    (r'(wolfenstein)', 'Wolfenstein'),
    (r'(tetris)', 'Tetris'),
    (r'(pac[\s-]?man)', 'Pac-Man'),
    (r'(galaga)', 'Galaga'),
    (r'(contra|probotector)', 'Contra'),
    (r'(gradius)', 'Gradius'),
    (r'(rtype|rtype)', 'R-Type'),
    (r'(ninja_gaiden)', 'Ninja Gaiden'),
    (r'(tmnt|ninja_turtles)', 'TMNT'),
    (r'(battletoads)', 'Battletoads'),
    (r'(double_dragon)', 'Double Dragon'),
    (r'(gunstar)', 'Gunstar Heroes'),
    (r'(shinobi)', 'Shinobi'),
    (r'(comix_zone)', 'Comix Zone'),
    (r'(ecco)', 'Ecco'),
    (r'(columns)', 'Columns'),
    (r'(puyo)', 'Puyo Puyo'),
    (r'(lemmings)', 'Lemmings'),
    (r'(prince_of_persia)', 'Prince of Persia'),
    (r'(1942|1943|1944)', '1942/19XX'),
    (r'(ghosts.*goblins)', "Ghosts 'n Goblins"),
    (r'(final_fight)', 'Final Fight'),
    (r'(tekken)', 'Tekken'),
    (r'(soul_calibur|soul_edge)', 'Soul Calibur'),
    (r'(king_of_fighters|kof)', 'King of Fighters'),
    (r'(fatal_fury)', 'Fatal Fury'),
    (r'(samurai_shodown)', 'Samurai Shodown'),
    (r'(darkstalkers)', 'Darkstalkers'),
    (r'(guilty_gear)', 'Guilty Gear'),
    (r'(blazblue)', 'BlazBlue'),
    (r'(resident_evil|biohazard)', 'Resident Evil'),
    (r'(silent_hill)', 'Silent Hill'),
    (r'(metal_gear|mgs)', 'Metal Gear Solid'),
    (r'(crash)', 'Crash Bandicoot'),
    (r'(spyro)', 'Spyro'),
    (r'(banjo)', 'Banjo-Kazooie'),
    (r'(diddy)', 'Diddy Kong Racing'),
    (r'(perfect_dark)', 'Perfect Dark'),
    (r'(goldeneye|golden_eye)', 'GoldenEye'),
    (r'(mario_64|sm64)', 'Super Mario 64'),
    (r'(ocarina|oot)', 'Zelda: Ocarina of Time'),
    (r"(majora|mm)", "Zelda: Majora's Mask"),
    (r'(mario_kart_64)', 'Mario Kart 64'),
    (r'(star_fox_64)', 'Star Fox 64'),
    (r'(banjo_kazooie)', 'Banjo-Kazooie'),
    (r'(paper_mario)', 'Paper Mario'),
    (r'(jet_force)', 'Jet Force Gemini'),
]

def categorize(filename):
    name = filename.lower().replace('_', ' ').replace('-', ' ').replace('%20', ' ').replace('.', ' ')
    for pattern, game in PATTERNS:
        if re.search(pattern, name):
            return game
    return None

def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--catalog', required=True)
    parser.add_argument('--output', default='vgm_categorized.tsv')
    args = parser.parse_args()

    entries = []
    with open(args.catalog) as f:
        for line in f:
            parts = line.strip().split('\t')
            if len(parts) == 3:
                entries.append(parts)
            elif len(parts) == 2:
                # console\tfilename format
                entries.append([parts[0], '_root_', parts[1]])

    categorized = []
    uncategorized = []
    for console, game, filename in entries:
        g = categorize(filename)
        if g:
            categorized.append((console, g, filename))
        else:
            uncategorized.append((console, '_unknown_', filename))

    with open(args.output, 'w') as f:
        for c, g, fn in categorized + uncategorized:
            f.write(f'{c}\t{g}\t{fn}\n')

    gc = Counter(g for _, g, _ in categorized)
    print(f'Total: {len(entries)}', file=sys.stderr)
    pct = (100*len(categorized)//len(entries)) if entries else 0
    print(f'Categorized: {len(categorized)} ({pct}%)', file=sys.stderr)
    print(f'Uncategorized: {len(uncategorized)}', file=sys.stderr)
    print(f'\nTop 40 games:', file=sys.stderr)
    for g, c in gc.most_common(40):
        print(f'  {g}: {c}', file=sys.stderr)

if __name__ == '__main__':
    main()
