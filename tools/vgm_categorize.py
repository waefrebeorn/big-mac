#!/usr/bin/env python3
"""tools/vgm_categorize.py — Categorize all VGMusic MIDI files by game (R096f)."""

import os, sys, re
from collections import Counter

# (regex_pattern, game_name) — order matters: first match wins
PATTERNS = [
    # === TIER 1: Most iconic ===
    # Pokemon (very common abbreviation patterns)
    (r'pok[eé]mon|poke\b|pkMn|pkmn|pok_', 'Pokemon'),
    (r'pokered|pok[eé]mon_red|red_blue|pokemonrb', 'Pokemon R/B'),
    (r'pokemongold|pok[eé]mon_gold|gold_silver|pokemonsilver', 'Pokemon G/S'),
    (r'pokemonruby|pok[eé]mon_ruby|ruby_sapphire|emerald', 'Pokemon R/S/E'),
    (r'pokemondiamond|diamond_pearl|platinum', 'Pokemon D/P'),
    (r'pokemonblack|black_white|b2w2', 'Pokemon B/W'),
    (r'pokemonxy|pokemon_x|x_y', 'Pokemon X/Y'),
    (r'pokemonsun|sun_moon|ultrasun', 'Pokemon S/M'),
    (r'pokemonsword|sword_shield', 'Pokemon S/S'),
    (r'pokemonlegends|arceus', 'Pokemon Legends'),
    (r'pokemongo', 'Pokemon GO'),
    (r'pok[eé]mon', 'Pokemon'),
    
    # Mario
    (r'super_mario_world|smw\b|smw2|smw3|smw_', 'Super Mario World'),
    (r'super_mario_bros|smb\b|smb1|smb2|smb3|smb_', 'Super Mario Bros'),
    (r'super_mario_64|sm64', 'Super Mario 64'),
    (r'mario_kart|smk\b|mario_kart_64|mk64|mkds|mkwii', 'Mario Kart'),
    (r'super_mario_rpg|smrpg', 'Super Mario RPG'),
    (r"yoshi|yoshi's_island|yarn", "Yoshi's Island"),
    (r'mario_paint', 'Mario Paint'),
    (r'mario_party', 'Mario Party'),
    (r'mario_tennis', 'Mario Tennis'),
    (r'mario_golf', 'Mario Golf'),
    (r'paper_mario', 'Paper Mario'),
    (r'mario_vs_donkey', 'Mario vs DK'),
    (r'super_mario_galaxy', 'Super Mario Galaxy'),
    (r'super_mario_odyssey', 'Super Mario Odyssey'),
    (r'mario', 'Mario (other)'),
    
    # Zelda
    (r'link_to_the_past|alttp|lttp', 'Zelda: LttP'),
    (r'ocarina_of_time|oot\b', 'Zelda: OoT'),
    (r'majoras_mask|majora\b', 'Zelda: MM'),
    (r'breath_of_the_wild|botw', 'Zelda: BotW'),
    (r'skyward_sword', 'Zelda: SS'),
    (r'wind_waker|ww\b.*zelda', 'Zelda: WW'),
    (r'twilight_princess|tp\b.*zelda', 'Zelda: TP'),
    (r"link's_awakening|links_awakening|la\b", "Zelda: Link's Awakening"),
    (r'the_minish_cap|minish_cap', 'Zelda: Minish Cap'),
    (r'oracle_of_ages|oracle_of_seasons', 'Zelda: Oracle'),
    (r'four_swords', 'Zelda: Four Swords'),
    (r'trilorce_heroes', 'Zelda: Triforce Heroes'),
    (r'zelda', 'Zelda (other)'),
    
    # Final Fantasy
    (r'ff7|ffvii|final_fantasy_7|final_fantasy_vii', 'FF7'),
    (r'ff6|ffiii|final_fantasy_6|final_fantasy_vi', 'FF6'),
    (r'ff4|ffii|final_fantasy_4|final_fantasy_iv', 'FF4'),
    (r'ff5|final_fantasy_5|final_fantasy_v', 'FF5'),
    (r'ff8|ffviii|final_fantasy_8|final_fantasy_viii', 'FF8'),
    (r'ff9|ffix|final_fantasy_9|final_fantasy_ix', 'FF9'),
    (r'ff10|ffx|final_fantasy_10|final_fantasy_x', 'FF10'),
    (r'ff12|ffxii|final_fantasy_12', 'FF12'),
    (r'ff13|ffxiii|final_fantasy_13', 'FF13'),
    (r'ff14|ffxiv|final_fantasy_14', 'FF14'),
    (r'ff15|ffxv|final_fantasy_15', 'FF15'),
    (r'fft|final_fantasy_tactics', 'FF Tactics'),
    (r'ff_adventure|crystal_chronicles', 'FF Adventure'),
    (r'final_fantasy', 'Final Fantasy (other)'),
    (r'ff\b', 'Final Fantasy (other)'),
    
    # Mega Man
    (r'mega_man_x|mmx\b|mmx[123]|megaman_x', 'Mega Man X'),
    (r'mega_man_battle_network|battle_network|exe[0-9]?', 'Mega Man BN'),
    (r'mega_man_star_force|star_force', 'Mega Man SF'),
    (r'mega_man_zero|mmz|zero[1234]', 'Mega Man Zero'),
    (r'mega_man_legends|legends|megaman_legends', 'Mega Man Legends'),
    (r'rockman', 'Mega Man (Rockman)'),
    (r'mega_man|megaman|mm[1-9]\b', 'Mega Man'),
    
    # Sonic
    (r'sonic_adventure|sadv|sa1|sa2', 'Sonic Adventure'),
    (r'sonic_heroes', 'Sonic Heroes'),
    (r'shadow_the_hedgehog|shadow', 'Shadow the Hedgehog'),
    (r'sonic_cd|sonic_blast', 'Sonic CD/3'),
    (r'sonic.*rush|rush_adventure', 'Sonic Rush'),
    (r'sonic.*unleashed|werehog', 'Sonic Unleashed'),
    (r'sonic.*colors|sonic_generations', 'Sonic Colors/Gen'),
    (r'sonic.*lost_world|sonic_forces', 'Sonic Lost World/Forces'),
    (r'sonic', 'Sonic (other)'),
    
    # Castlevania
    (r'symphony_of_the_night|sotn', 'CV: SOTN'),
    (r'aria_of_sorrow|soaria|dos', 'CV: Aria/DoS'),
    (r'dawn_of_sorrow|soma', 'CV: Dawn of Sorrow'),
    (r'order_of_ecclesia|ooe', 'CV: Order of Ecclesia'),
    (r'portrait_of_ruin|por', 'CV: Portrait of Ruin'),
    (r'harmony_of_dissonance|hod', 'CV: HoD'),
    (r"castlevania|cv\b|cv[1-9]|akumajou|akumajo|dracula", 'Castlevania'),
    
    # Metroid
    (r'super_metroid', 'Super Metroid'),
    (r'metroid_prime|mp1|mp2|mp3|mp\b', 'Metroid Prime'),
    (r'metroid_fusion', 'Metroid Fusion'),
    (r'metroid_zero_mission|mzm', 'Metroid ZM'),
    (r'metroid|samus', 'Metroid (other)'),
    
    # Chrono
    (r'chrono_trigger|ct\b', 'Chrono Trigger'),
    (r'chrono_cross', 'Chrono Cross'),
    
    # Street Fighter
    (r'street_fighter_ii|sf2|sfii|super_sf2|ssf2', 'SF2'),
    (r'street_fighter_alpha|sfalpha|sfa[123]', 'SF Alpha'),
    (r'street_fighter_iii|sf3|sfiii', 'SF3'),
    (r'street_fighter_iv|sf4|sfiv', 'SF4'),
    (r'street_fighter', 'Street Fighter'),
    
    # Donkey Kong
    (r'donkey_kong_country|dkc|dk1|dk2|dk3', 'DKC'),
    (r'diddy_kong_racing|diddy', 'Diddy Kong Racing'),
    (r'donkey_kong|dk\b', 'Donkey Kong'),
    
    # Kirby
    (r"kirby.*dream_land|kirby's_dream", "Kirby: Dream Land"),
    (r'kirby_super_star|kirby_ultra', 'Kirby Super Star'),
    (r'kirby.*nightmare|nightmare_in_dreamland', "Kirby: Nightmare"),
    (r'kirby.*return|kirby.*triple_deluxe', "Kirby: Return/TD"),
    (r'kirby.*planet_robobot|kirby.*star_allies', "Kirby: Robobot/Allies"),
    (r'kirby', 'Kirby (other)'),
    
    # EarthBound / Mother
    (r'earthbound|eb\b', 'EarthBound'),
    (r'mother[123]?|mother_\d', 'Mother'),
    
    # === TIER 2: Very popular ===
    # Fire Emblem
    (r'fire_emblem|fe[0-9]+|fe1[0-9]?', 'Fire Emblem'),
    
    # Animal Crossing
    (r'animal_crossing|ac\b|acww|acnl|achhd', 'Animal Crossing'),
    
    # Kingdom Hearts
    (r'kingdom_hearts|kh\b|kh1|kh2|kh3', 'Kingdom Hearts'),
    
    # Metal Gear
    (r'metal_gear_solid|mgs[1234]?', 'Metal Gear Solid'),
    (r'metal_gear|metalgear', 'Metal Gear'),
    
    # Mega Man spinoffs
    (r'megaman_x|mmx', 'Mega Man X'),
    
    # SNES classics
    (r'actraiser', 'ActRaiser'),
    (r'soul_blazer', 'Soul Blazer'),
    (r'illusion_of_gaia', 'Illusion of Gaia'),
    (r'terranigma', 'Terranigma'),
    (r'secret_of_mana|seiken_densetsu_2|sd2', 'Secret of Mana'),
    (r'seiken_densetsu_3|sd3', 'Seiken Densetsu 3'),
    (r'trials_of_mana', 'Trials of Mana'),
    (r'xenogears', 'Xenogears'),
    (r'suikoden', 'Suikoden'),
    (r'vagrant_story', 'Vagrant Story'),
    (r'legend_of_legaia', 'Legend of Legaia'),
    (r'valkyrie_profile', 'Valkyrie Profile'),
    (r'bahamut_lagoon|bahamut', 'Bahamut Lagoon'),
    (r'breath_of_fire|bof[1-5]|breath_of_fire', 'Breath of Fire'),
    (r'live_a_live', 'Live A Live'),
    (r'radical_dreamers', 'Radical Dreamers'),
    (r'star_fox|sf[12]|star_fox_64', 'Star Fox'),
    (r'f_zero', 'F-Zero'),
    (r'pilotwings', 'Pilotwings'),
    (r'simcity', 'SimCity'),
    (r'earthworm_jim', 'Earthworm Jim'),
    (r"donkey_kong.*country|dkc", 'DKC'),
    
    # PS1 classics
    (r'crash_bandicoot|crash', 'Crash Bandicoot'),
    (r'spyro', 'Spyro'),
    (r'turok', 'Turok'),
    (r'banjo.kazooie|banjo', 'Banjo-Kazooie'),
    (r'jet_force_gemini|jet_force', 'Jet Force Gemini'),
    (r'perfect_dark', 'Perfect Dark'),
    (r'goldeneye|golden_eye|007', 'GoldenEye'),
    (r'resident_evil|biohazard', 'Resident Evil'),
    (r'silent_hill|sh1|sh2|sh3', 'Silent Hill'),
    (r'tomb_raider', 'Tomb Raider'),
    (r'crash_team_racing|ctr', 'Crash Team Racing'),
    (r'spyro.*year_of_the_dragon|spyro.*2|spyro.*3', 'Spyro'),
    (r'gran_turismo', 'Gran Turismo'),
    (r'tekken', 'Tekken'),
    (r'soul_calibur|soul_edge|soulblade', 'Soul Calibur'),
    (r'king_of_fighters|kof', 'King of Fighters'),
    (r'fatal_fury', 'Fatal Fury'),
    (r'samurai_shodown', 'Samurai Shodown'),
    (r'darkstalkers', 'Darkstalkers'),
    (r'guilty_gear', 'Guilty Gear'),
    (r'blazblue', 'BlazBlue'),
    (r'melty_blood', 'Melty Blood'),
    (r'guilty_gear', 'Guilty Gear'),
    
    # GBA/DS/3DS
    (r'pokemon_mystery_dungeon|pmd', 'Pokemon Mystery Dungeon'),
    (r'pokemon_ranger', 'Pokemon Ranger'),
    (r'pokemon_conquest', 'Pokemon Conquest'),
    (r'advance_wars', 'Advance Wars'),
    (r'golden_sun', 'Golden Sun'),
    (r'mario_vs_donkey_kong|mvdk', 'Mario vs DK'),
    (r'kirby.*canvas_curse|kirby.*squeak_squad', 'Kirby DS'),
    (r'wario_ware', 'WarioWare'),
    (r'brain_age', 'Brain Age'),
    (r'nintendogs', 'Nintendogs'),
    (r'elite_beat_agents', 'Elite Beat Agents'),
    (r'osu_tatakae_ouendan', 'Ouendan'),
    (r'rhythm_heaven|rhythm_tengoku', 'Rhythm Heaven'),
    (r'elden_ring', 'Elden Ring'),
    (r'dark_souls|darksouls|ds[123]', 'Dark Souls'),
    (r'bloodborne', 'Bloodborne'),
    (r'sekiro', 'Sekiro'),
    
    # Modern
    (r'undertale', 'Undertale'),
    (r'deltarune', 'Deltarune'),
    (r'hollow_knight', 'Hollow Knight'),
    (r'celeste', 'Celeste'),
    (r'stardew_valley', 'Stardew Valley'),
    (r'cuphead', 'Cuphead'),
    (r'ori.*blind_forest|ori.*will_of_the_wisps', 'Ori'),
    (r'rayman', 'Rayman'),
    
    # Retro/Arcade
    (r'doom\b', 'DOOM'),
    (r'wolfenstein', 'Wolfenstein'),
    (r'duke_nukem', 'Duke Nukem'),
    (r'quake', 'Quake'),
    (r'unreal', 'Unreal'),
    (r'half.life|halflife', 'Half-Life'),
    (r'counter.strike|cs[12]?', 'Counter-Strike'),
    (r'team_fortress|tf2', 'Team Fortress'),
    (r'deus_ex', 'Deus Ex'),
    (r'system_shock', 'System Shock'),
    (r'thief', 'Thief'),
    (r'myst', 'Myst'),
    (r'riven', 'Riven'),
    (r'zork', 'Zork'),
    (r"king's_quest|king_quest", "King's Quest"),
    (r'space_quest', 'Space Quest'),
    (r'leisure_suit_larry', 'Leisure Suit Larry'),
    (r'monkey_island', 'Monkey Island'),
    (r'dizzy', 'Dizzy'),
    (r'prince_of_persia', 'Prince of Persia'),
    (r'tetris', 'Tetris'),
    (r'pac[\s-]?man', 'Pac-Man'),
    (r'galaga', 'Galaga'),
    (r'space_invaders', 'Space Invaders'),
    (r'frogger', 'Frogger'),
    (r'q[\s-]?bert', 'Q*bert'),
    (r'dig_dug', 'Dig Dug'),
    (r'bubble_bobble', 'Bubble Bobble'),
    (r'rtype', 'R-Type'),
    (r'gradius', 'Gradius'),
    (r'salamander|life_force', 'Salamander'),
    (r'parodius', 'Parodius'),
    (r'twin_bee', 'Twin Bee'),
    (r'axelay', 'Axelay'),
    (r'aleste', 'Aleste'),
    (r'blaster_master', 'Blaster Master'),
    (r'adventure_island', 'Adventure Island'),
    (r'wonder_boy', 'Wonder Boy'),
    (r'gauntlet', 'Gauntlet'),
    (r"ghosts.*goblins|ghouls.*ghosts", "Ghosts 'n Goblins"),
    (r'final_fight', 'Final Fight'),
    (r'1942|1943|1944|19xx', '1942/19XX'),
    (r'contra|probotector', 'Contra'),
    (r'double_dragon', 'Double Dragon'),
    (r'ninja_gaiden', 'Ninja Gaiden'),
    (r'tmnt|ninja_turtles', 'TMNT'),
    (r'battletoads', 'Battletoads'),
    (r'gunstar', 'Gunstar Heroes'),
    (r'shinobi', 'Shinobi'),
    (r'comix_zone', 'Comix Zone'),
    (r'ecco', 'Ecco'),
    (r'columns', 'Columns'),
    (r'puyo', 'Puyo Puyo'),
    (r'lemmings', 'Lemmings'),
    (r'simcity', 'SimCity'),
    (r'populous', 'Populous'),
    (r'power_monger', 'Power Monger'),
    (r'civilization', 'Civilization'),
    (r'master_of_orion', 'Master of Orion'),
    (r'x[\s-]?com', 'X-COM'),
    (r'warcraft', 'Warcraft'),
    (r'diablo', 'Diablo'),
    (r'starcraft', 'StarCraft'),
    (r'age_of_empires', 'Age of Empires'),
    (r'total_annihilation', 'Total Annihilation'),
    (r'heroes_of_might|homm', 'Heroes of Might'),
    (r'warcraft', 'Warcraft'),
    (r'diablo', 'Diablo'),
    (r'starcraft', 'StarCraft'),
    (r'age_of_empires', 'Age of Empires'),
    (r'total_annihilation', 'Total Annihilation'),
    (r'heroes_of_might|homm', 'Heroes of Might'),
    
    # Sega specific
    (r'alex_kidd', 'Alex Kidd'),
    (r'wonder_boy', 'Wonder Boy'),
    (r'gunstar', 'Gunstar Heroes'),
    (r'shinobi', 'Shinobi'),
    (r'comix_zone', 'Comix Zone'),
    (r'ecco', 'Ecco the Dolphin'),
    (r'toes_jane', "ToeJam & Earl"),
    (r'columns', 'Columns'),
    (r'puyo', 'Puyo Puyo'),
    (r'lemmings', 'Lemmings'),
    (r'earthworm_jim', 'Earthworm Jim'),
    (r'virtua_fighter', 'Virtua Fighter'),
    (r'fighting_vipers', 'Fighting Vipers'),
    (r'daytona', 'Daytona USA'),
    (r'virtua_racing', 'Virtua Racing'),
    (r'outrun', 'OutRun'),
    (r'after_burner', 'After Burner'),
    (r'thunder_force', 'Thunder Force'),
    (r'alex_kidd', 'Alex Kidd'),
    
    # Computer/PC
    (r'doom', 'DOOM'),
    (r'wolfenstein', 'Wolfenstein'),
    (r'duke_nukem', 'Duke Nukem'),
    (r'quake', 'Quake'),
    (r'unreal', 'Unreal'),
    (r'half_life', 'Half-Life'),
    (r'counter_strike', 'Counter-Strike'),
    (r'team_fortress', 'Team Fortress'),
    (r'deus_ex', 'Deus Ex'),
    (r'system_shock', 'System Shock'),
    (r'thief', 'Thief'),
    (r'myst', 'Myst'),
    (r'riven', 'Riven'),
    (r'zork', 'Zork'),
    (r'kings_quest', "King's Quest"),
    (r'space_quest', 'Space Quest'),
    (r'leisure_suit_larry', 'Leisure Suit Larry'),
    (r'monkey_island', 'Monkey Island'),
    (r'dizzy', 'Dizzy'),
    (r'prince_of_persia', 'Prince of Persia'),
    (r'simcity', 'SimCity'),
    (r'populous', 'Populous'),
    (r'civilization', 'Civilization'),
    (r'master_of_orion', 'Master of Orion'),
    (r'x_com', 'X-COM'),
    (r'warcraft', 'Warcraft'),
    (r'diablo', 'Diablo'),
    (r'starcraft', 'StarCraft'),
    (r'age_of_empires', 'Age of Empires'),
    (r'total_annihilation', 'Total Annihilation'),
    (r'heroes_of_might', 'Heroes of Might'),
    (r'command_and_conquer|cnc', 'C&C'),
    (r'dune', 'Dune'),
    (r'myst', 'Myst'),
    (r'riven', 'Riven'),
    (r'zork', 'Zork'),
    (r'kings_quest', "King's Quest"),
    (r'space_quest', 'Space Quest'),
    (r'leisure_suit_larry', 'Leisure Suit Larry'),
    (r'monkey_island', 'Monkey Island'),
    (r'dizzy', 'Dizzy'),
    (r'prince_of_persia', 'Prince of Persia'),
    (r'wasteland', 'Wasteland'),
    (r'bards_tale', "The Bard's Tale"),
    (r'wizardry', 'Wizardry'),
    (r'ultima', 'Ultima'),
    (r'might_and_magic', 'Might and Magic'),
]

def categorize(filename):
    name = filename.lower().replace('_', ' ').replace('-', ' ').replace('%20', ' ').replace('.', ' ').replace("'", '').replace('"', '')
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
    print(f'\nTop 50 games:', file=sys.stderr)
    for g, c in gc.most_common(50):
        print(f'  {g}: {c}', file=sys.stderr)

if __name__ == '__main__':
    main()
