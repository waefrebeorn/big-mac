/* wb_asset_dl.c — character asset downloader for YTP/3D poop (R082).
 *
 * Fetches sprites and 3D models from VG Resource sites and organizes them
 * into a local library for use in YTP editing.
 *
 * Usage: wb_asset_dl <command> [args]
 *   wb_asset dl-sprite <game> <character>   — download sprite sheet
 *   wb_asset dl-model <game> <character>    — download 3D model
 *   wb_asset dl-list <tier>                 — download full tier list
 *   wb_asset scan                           — scan library status
 *
 * Pure C11, uses curl for HTTP, no other deps.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ASSET_LIB "/Users/waefrebeorn/Documents/big-mac/assets/characters"
#define MAX_PATH 1024

/* ---- Character Database ----------------------------------------------- */

typedef struct {
    const char *name;
    const char *game;
    const char *console;
    const char *source_url;     /* URL on spriters-resource or models-resource */
    int is_3d;                  /* 0 = 2D sprite, 1 = 3D model */
    int tier;                   /* 1-5, priority */
} asset_entry;

/* Tier 1: Essential 2D sprites */
static asset_entry tier1_2d[] = {
    /* Nintendo */
    {"Mario", "Super Mario World", "SNES", "https://www.spriters-resource.com/snes/smarioworld/", 0, 1},
    {"Luigi", "Super Mario World", "SNES", "https://www.spriters-resource.com/snes/smarioworld/", 0, 1},
    {"Yoshi", "Super Mario World", "SNES", "https://www.spriters-resource.com/snes/smarioworld/", 0, 1},
    {"Bowser", "Super Mario World", "SNES", "https://www.spriters-resource.com/snes/smarioworld/", 0, 1},
    {"Peach", "Super Mario World", "SNES", "https://www.spriters-resource.com/snes/smarioworld/", 0, 1},
    {"Toad", "Super Mario World", "SNES", "https://www.spriters-resource.com/snes/smarioworld/", 0, 1},
    {"Link", "A Link to the Past", "SNES", "https://www.spriters-resource.com/snes/legendofzeldaalinktothepast/", 0, 1},
    {"Zelda", "A Link to the Past", "SNES", "https://www.spriters-resource.com/snes/legendofzeldaalinktothepast/", 0, 1},
    {"Ganon", "A Link to the Past", "SNES", "https://www.spriters-resource.com/snes/legendofzeldaalinktothepast/", 0, 1},
    {"Kirby", "Kirby Super Star", "SNES", "https://www.spriters-resource.com/snes/kirbysuperstar/", 0, 1},
    {"Meta Knight", "Kirby Super Star", "SNES", "https://www.spriters-resource.com/snes/kirbysuperstar/", 0, 1},
    {"King Dedede", "Kirby Super Star", "SNES", "https://www.spriters-resource.com/snes/kirbysuperstar/", 0, 1},
    {"Samus", "Super Metroid", "SNES", "https://www.spriters-resource.com/snes/supermetroid/", 0, 1},
    {"Fox", "Star Fox", "SNES", "https://www.spriters-resource.com/snes/starfox/", 0, 1},
    {"Ness", "EarthBound", "SNES", "https://www.spriters-resource.com/snes/earthbound/", 0, 1},
    {"Captain Falcon", "Super Smash Bros.", "SNES", "https://www.spriters-resource.com/snes/ssb/", 0, 1},

    /* Sega */
    {"Sonic", "Sonic the Hedgehog", "Genesis", "https://www.spriters-resource.com/sega_genesis/sonicth1/", 0, 1},
    {"Tails", "Sonic the Hedgehog 2", "Genesis", "https://www.spriters-resource.com/sega_genesis/sonicth2/", 0, 1},
    {"Knuckles", "Sonic the Hedgehog 3", "Genesis", "https://www.spriters-resource.com/sega_genesis/sonicth3/", 0, 1},
    {"Dr. Eggman", "Sonic the Hedgehog", "Genesis", "https://www.spriters-resource.com/sega_genesis/sonicth1/", 0, 1},
    {"Shadow", "Sonic Adventure", "Dreamcast", "https://www.spriters-resource.com/dreamcast/sonicadventure/", 0, 1},

    /* Other retro */
    {"Mega Man", "Mega Man 2", "NES", "https://www.spriters-resource.com/nes/megaman2/", 0, 1},
    {"Simon Belmont", "Castlevania", "NES", "https://www.spriters-resource.com/nes/castlevania/", 0, 1},
    {"Ryu", "Street Fighter II", "SNES", "https://www.spriters-resource.com/snes/streetfighteriit/", 0, 1},
    {"Scorpion", "Mortal Kombat", "Genesis", "https://www.spriters-resource.com/sega_genesis/mortalkombat/", 0, 1},
    {"Crash Bandicoot", "Crash Bandicoot", "PS1", "https://www.spriters-resource.com/playstation/crashbandicoot/", 0, 1},
    {"Cloud Strife", "Final Fantasy VII", "PS1", "https://www.spriters-resource.com/playstation/ff7/", 0, 1},
    {"Sephiroth", "Final Fantasy VII", "PS1", "https://www.spriters-resource.com/playstation/ff7/", 0, 1},

    /* Indie */
    {"Frisk", "Undertale", "PC", "https://www.spriters-resource.com/pc_computer/undertale/", 0, 1},
    {"Sans", "Undertale", "PC", "https://www.spriters-resource.com/pc_computer/undertale/", 0, 1},
    {"Papyrus", "Undertale", "PC", "https://www.spriters-resource.com/pc_computer/undertale/", 0, 1},
    {"Flowey", "Undertale", "PC", "https://www.spriters-resource.com/pc_computer/undertale/", 0, 1},
    {"Toriel", "Undertale", "PC", "https://www.spriters-resource.com/pc_computer/undertale/", 0, 1},
    {"Asgore", "Undertale", "PC", "https://www.spriters-resource.com/pc_computer/undertale/", 0, 1},
    {"Undyne", "Undertale", "PC", "https://www.spriters-resource.com/pc_computer/undertale/", 0, 1},
    {"Alphys", "Undertale", "PC", "https://www.spriters-resource.com/pc_computer/undertale/", 0, 1},
    {"Mettaton", "Undertale", "PC", "https://www.spriters-resource.com/pc_computer/undertale/", 0, 1},
    {"Kris", "Deltarune", "PC", "https://www.spriters-resource.com/pc_computer/deltarune/", 0, 1},
    {"Susie", "Deltarune", "PC", "https://www.spriters-resource.com/pc_computer/deltarune/", 0, 1},
    {"Ralsei", "Deltarune", "PC", "https://www.spriters-resource.com/pc_computer/deltarune/", 0, 1},
    {"Noelle", "Deltarune", "PC", "https://www.spriters-resource.com/pc_computer/deltarune/", 0, 1},
    {"Berdly", "Deltarune", "PC", "https://www.spriters-resource.com/pc_computer/deltarune/", 0, 1},
    {"Spamton", "Deltarune", "PC", "https://www.spriters-resource.com/pc_computer/deltarune/", 0, 1},
    {"Jevil", "Deltarune", "PC", "https://www.spriters-resource.com/pc_computer/deltarune/", 0, 1},
    {"Queen", "Deltarune", "PC", "https://www.spriters-resource.com/pc_computer/deltarune/", 0, 1},
    {"Lancer", "Deltarune", "PC", "https://www.spriters-resource.com/pc_computer/deltarune/", 0, 1},
    {"Boyfriend", "Friday Night Funkin'", "Browser", "https://www.spriters-resource.com/browser_games/fridaynightfunkin/", 0, 1},
    {"Girlfriend", "Friday Night Funkin'", "Browser", "https://www.spriters-resource.com/browser_games/fridaynightfunkin/", 0, 1},
    {"Pico", "Friday Night Funkin'", "Browser", "https://www.spriters-resource.com/browser_games/fridaynightfunkin/", 0, 1},
    {"Tankman", "Friday Night Funkin'", "Browser", "https://www.spriters-resource.com/browser_games/fridaynightfunkin/", 0, 1},
    {"Monika", "Doki Doki Literature Club", "PC", "https://www.spriters-resource.com/pc_computer/ddlc/", 0, 1},
    {"Yuri", "Doki Doki Literature Club", "PC", "https://www.spriters-resource.com/pc_computer/ddlc/", 0, 1},
    {"Sayori", "Doki Doki Literature Club", "PC", "https://www.spriters-resource.com/pc_computer/ddlc/", 0, 1},
    {"Natsuki", "Doki Doki Literature Club", "PC", "https://www.spriters-resource.com/pc_computer/ddlc/", 0, 1},

    /* Pokémon */
    {"Pikachu", "Pokemon Red/Blue", "GB", "https://www.spriters-resource.com/game_boy_gbc/pokemonredblue/", 0, 1},
    {"Charizard", "Pokemon Red/Blue", "GB", "https://www.spriters-resource.com/game_boy_gbc/pokemonredblue/", 0, 1},
    {"Mewtwo", "Pokemon Red/Blue", "GB", "https://www.spriters-resource.com/game_boy_gbc/pokemonredblue/", 0, 1},

    {NULL}
};

/* Tier 2: 3D models */
static asset_entry tier2_3d[] = {
    {"Mario", "Super Mario 64", "N64", "https://models.spriters-resource.com/nintendo_64/supermario64/", 1, 2},
    {"Luigi", "Super Mario 64", "N64", "https://models.spriters-resource.com/nintendo_64/supermario64/", 1, 2},
    {"Bowser", "Super Mario 64", "N64", "https://models.spriters-resource.com/nintendo_64/supermario64/", 1, 2},
    {"Peach", "Super Mario 64", "N64", "https://models.spriters-resource.com/nintendo_64/supermario64/", 1, 2},
    {"Yoshi", "Super Mario 64", "N64", "https://models.spriters-resource.com/nintendo_64/supermario64/", 1, 2},
    {"Link", "Ocarina of Time", "N64", "https://models.spriters-resource.com/nintendo_64/thelegendofzeldaocarinaoftime/", 1, 2},
    {"Zelda", "Ocarina of Time", "N64", "https://models.spriters-resource.com/nintendo_64/thelegendofzeldaocarinaoftime/", 1, 2},
    {"Ganondorf", "Ocarina of Time", "N64", "https://models.spriters-resource.com/nintendo_64/thelegendofzeldaocarinaoftime/", 1, 2},
    {"Navi", "Ocarina of Time", "N64", "https://models.spriters-resource.com/nintendo_64/thelegendofzeldaocarinaoftime/", 1, 2},
    {"Fox", "Smash 64", "N64", "https://models.spriters-resource.com/nintendo_64/ssb/", 1, 2},
    {"Pikachu", "Smash 64", "N64", "https://models.spriters-resource.com/nintendo_64/ssb/", 1, 2},
    {"Jigglypuff", "Smash 64", "N64", "https://models.spriters-resource.com/nintendo_64/ssb/", 1, 2},
    {"Captain Falcon", "Smash 64", "N64", "https://models.spriters-resource.com/nintendo_64/ssb/", 1, 2},
    {"Crash Bandicoot", "Crash Bandicoot", "PS1", "https://models.spriters-resource.com/playstation/crashbandicoot/", 1, 2},
    {"Cortex", "Crash Bandicoot", "PS1", "https://models.spriters-resource.com/playstation/crashbandicoot/", 1, 2},
    {"Spyro", "Spyro the Dragon", "PS1", "https://models.spriters-resource.com/playstation/spyro/", 1, 2},
    {"Cloud Strife", "Final Fantasy VII", "PS1", "https://models.spriters-resource.com/playstation/ff7/", 1, 2},
    {"Sephiroth", "Final Fantasy VII", "PS1", "https://models.spriters-resource.com/playstation/ff7/", 1, 2},
    {"Solid Snake", "Metal Gear Solid", "PS1", "https://models.spriters-resource.com/playstation/metalgearsolid/", 1, 2},
    {"Mario", "Super Mario Galaxy", "Wii", "https://models.spriters-resource.com/wii/supermariogalaxy/", 1, 2},
    {"Luigi", "Super Mario Galaxy", "Wii", "https://models.spriters-resource.com/wii/supermariogalaxy/", 1, 2},
    {"Bowser", "Super Mario Galaxy", "Wii", "https://models.spriters-resource.com/wii/supermariogalaxy/", 1, 2},
    {"Rosalina", "Super Mario Galaxy", "Wii", "https://models.spriters-resource.com/wii/supermariogalaxy/", 1, 2},
    {"Sonic", "Sonic Adventure", "Dreamcast", "https://models.spriters-resource.com/dreamcast/sonicadventure/", 1, 2},
    {"Tails", "Sonic Adventure", "Dreamcast", "https://models.spriters-resource.com/dreamcast/sonicadventure/", 1, 2},
    {"Knuckles", "Sonic Adventure", "Dreamcast", "https://models.spriters-resource.com/dreamcast/sonicadventure/", 1, 2},
    {"Banjo", "Banjo-Kazooie", "N64", "https://models.spriters-resource.com/nintendo_64/banjokazoo/", 1, 2},
    {"Kazooie", "Banjo-Kazooie", "N64", "https://models.spriters-resource.com/nintendo_64/banjokazoo/", 1, 2},
    {"Kirby", "Kirby 64", "N64", "https://models.spriters-resource.com/nintendo_64/kirby64/", 1, 2},
    {"Paper Mario", "Paper Mario", "N64", "https://models.spriters-resource.com/nintendo_64/pm/", 1, 2},
    {"SpongeBob", "Battle for Bikini Bottom", "GameCube", "https://models.spriters-resource.com/gamecube/spongebobsquarepantsbattleforbikinibottom/", 1, 2},
    {"Patrick", "Battle for Bikini Bottom", "GameCube", "https://models.spriters-resource.com/gamecube/spongebobsquarepantsbattleforbikinibottom/", 1, 2},
    {"Steve", "Minecraft", "PC", "https://models.spriters-resource.com/pc_computer/minecraft/", 1, 2},
    {"Creeper", "Minecraft", "PC", "https://models.spriters-resource.com/pc_computer/minecraft/", 1, 2},
    {NULL}
};

/* ---- Library Organization --------------------------------------------- */

int ensure_dir(const char *path) {
    char cmd[MAX_PATH];
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", path);
    return system(cmd);
}

int init_library(void) {
    ensure_dir(ASSET_LIB);
    ensure_dir(ASSET_LIB "/2d");
    ensure_dir(ASSET_LIB "/2d/nes");
    ensure_dir(ASSET_LIB "/2d/snes");
    ensure_dir(ASSET_LIB "/2d/genesis");
    ensure_dir(ASSET_LIB "/2d/gb");
    ensure_dir(ASSET_LIB "/2d/gba");
    ensure_dir(ASSET_LIB "/2d/n64");
    ensure_dir(ASSET_LIB "/2d/ps1");
    ensure_dir(ASSET_LIB "/2d/dreamcast");
    ensure_dir(ASSET_LIB "/2d/pc");
    ensure_dir(ASSET_LIB "/2d/browser");
    ensure_dir(ASSET_LIB "/2d/custom");
    ensure_dir(ASSET_LIB "/3d");
    ensure_dir(ASSET_LIB "/3d/n64");
    ensure_dir(ASSET_LIB "/3d/ps1");
    ensure_dir(ASSET_LIB "/3d/dreamcast");
    ensure_dir(ASSET_LIB "/3d/gamecube");
    ensure_dir(ASSET_LIB "/3d/wii");
    ensure_dir(ASSET_LIB "/3d/pc");
    ensure_dir(ASSET_LIB "/3d/custom");
    ensure_dir(ASSET_LIB "/mocap");
    ensure_dir(ASSET_LIB "/mocap/cmu");
    ensure_dir(ASSET_LIB "/poses");
    ensure_dir(ASSET_LIB "/scenes");
    return 0;
}

/* ---- Scan Library ----------------------------------------------------- */

int scan_library(void) {
    printf("=== Character Asset Library ===\n");
    printf("Location: %s\n\n", ASSET_LIB);

    char cmd[MAX_PATH];

    snprintf(cmd, sizeof(cmd), "find \"%s\" -type f | wc -l", ASSET_LIB);
    printf("Total files: "); fflush(stdout);
    system(cmd);

    snprintf(cmd, sizeof(cmd), "du -sh \"%s\"", ASSET_LIB);
    printf("Total size: "); fflush(stdout);
    system(cmd);

    printf("\n--- 2D Sprites ---\n");
    snprintf(cmd, sizeof(cmd), "find \"%s/2d\" -type f 2>/dev/null | wc -l", ASSET_LIB);
    printf("Files: "); fflush(stdout);
    system(cmd);

    printf("\n--- 3D Models ---\n");
    snprintf(cmd, sizeof(cmd), "find \"%s/3d\" -type f 2>/dev/null | wc -l", ASSET_LIB);
    printf("Files: "); fflush(stdout);
    system(cmd);

    printf("\n--- Mocap Data ---\n");
    snprintf(cmd, sizeof(cmd), "find \"%s/mocap\" -type f 2>/dev/null | wc -l", ASSET_LIB);
    printf("Files: "); fflush(stdout);
    system(cmd);

    return 0;
}

/* ---- Download (placeholder — actual download requires page parsing) ----- */

int download_sprite(const char *name, const char *game, const char *console, const char *url) {
    printf("  [2D] %s (%s, %s)\n", name, game, console);
    printf("    Source: %s\n", url);
    printf("    Status: MANUAL DOWNLOAD REQUIRED (parse page for asset links)\n");
    return 0;
}

int download_model(const char *name, const char *game, const char *console, const char *url) {
    printf("  [3D] %s (%s, %s)\n", name, game, console);
    printf("    Source: %s\n", url);
    printf("    Status: MANUAL DOWNLOAD REQUIRED (parse page for asset links)\n");
    return 0;
}

int download_tier(int tier) {
    asset_entry *entries = NULL;
    int count = 0;

    switch (tier) {
        case 1: entries = tier1_2d; break;
        case 2: entries = tier2_3d; break;
        default: printf("Unknown tier %d\n", tier); return -1;
    }

    for (int i = 0; entries[i].name; i++) count++;
    printf("=== Downloading Tier %d (%d characters) ===\n", tier, count);

    for (int i = 0; entries[i].name; i++) {
        if (entries[i].is_3d)
            download_model(entries[i].name, entries[i].game, entries[i].console, entries[i].source_url);
        else
            download_sprite(entries[i].name, entries[i].game, entries[i].console, entries[i].source_url);
    }

    return 0;
}

/* ---- Main ------------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("wb_asset_dl — character asset downloader for YTP/3D poop\n");
        printf("Usage:\n");
        printf("  wb_asset_dl init            — create library directory structure\n");
        printf("  wb_asset_dl scan            — scan library status\n");
        printf("  wb_asset_dl tier <1|2>      — list download URLs for tier\n");
        printf("  wb_asset_dl list-all        — list all characters in database\n");
        return 0;
    }

    if (strcmp(argv[1], "init") == 0) {
        init_library();
        printf("Library initialized at %s\n", ASSET_LIB);
    } else if (strcmp(argv[1], "scan") == 0) {
        scan_library();
    } else if (strcmp(argv[1], "tier") == 0 && argc >= 3) {
        int tier = atoi(argv[2]);
        download_tier(tier);
    } else if (strcmp(argv[1], "list-all") == 0) {
        printf("=== Tier 1: Essential 2D Sprites ===\n");
        for (int i = 0; tier1_2d[i].name; i++)
            printf("  %-20s %-30s %s\n", tier1_2d[i].name, tier1_2d[i].game, tier1_2d[i].console);
        printf("\n=== Tier 2: 3D Models ===\n");
        for (int i = 0; tier2_3d[i].name; i++)
            printf("  %-20s %-30s %s\n", tier2_3d[i].name, tier2_3d[i].game, tier2_3d[i].console);
    }

    return 0;
}
