#!/usr/bin/env python3
"""
tools/vgm_catalog.py — VGMusic.com catalog builder (R06).

Builds a catalog of all MIDI files available on VGMusic.com by parsing
their directory listing pages (not scraping — just reading the index HTML).

Usage:
    python3 tools/vgm_catalog.py --build-catalog
    python3 tools/vgm_catalog.py --top 10000 --output vgm_top10k.txt
    python3 tools/vgm_catalog.py --download vgm_top10k.txt --dir ~/vgm/

Respects VGMusic.com's terms: only downloads for personal use.
"""

import os
import sys
import re
import time
import urllib.request
import urllib.error
from collections import defaultdict

BASE_URL = "https://www.vgmusic.com"

CONSOLES = [
    ("nes", "/music/console/nintendo/nes/"),
    ("snes", "/music/console/nintendo/snes/"),
    ("n64", "/music/console/nintendo/n64/"),
    ("gameboy", "/music/console/nintendo/gameboy/"),
    ("gba", "/music/console/nintendo/gba/"),
    ("gamecube", "/music/console/nintendo/gamecube/"),
    ("ds", "/music/console/nintendo/ds/"),
    ("3ds", "/music/console/nintendo/3ds/"),
    ("wii", "/music/console/nintendo/wii/"),
    ("wiiu", "/music/console/nintendo/wiiu/"),
    ("switch", "/music/console/nintendo/switch/"),
    ("genesis", "/music/console/sega/genesis/"),
    ("saturn", "/music/console/sega/saturn/"),
    ("dreamcast", "/music/console/sega/dreamcast/"),
    ("psx", "/music/console/sony/psx/"),
    ("ps2", "/music/console/sony/ps2/"),
    ("ps3", "/music/console/sony/ps3/"),
    ("ps4", "/music/console/sony/ps4/"),
    ("psp", "/music/console/sony/psp/"),
    ("xbox", "/music/console/microsoft/xbox/"),
    ("xbox360", "/music/console/microsoft/xbox360/"),
    ("xboxone", "/music/console/microsoft/xboxone/"),
    ("pc", "/music/computer/pc/"),
    ("arcade", "/music/other/arcade/"),
]

def fetch_page(path):
    """Fetch a directory listing page from VGMusic."""
    url = BASE_URL + path
    req = urllib.request.Request(url, headers={
        'User-Agent': 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36'
    })
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return resp.read().decode('utf-8', errors='replace')
    except (urllib.error.URLError, OSError) as e:
        print(f"  Warning: failed to fetch {url}: {e}", file=sys.stderr)
        return None

def parse_midi_listing(html):
    """Extract MIDI filenames from a directory listing page."""
    # VGMusic directory listings have links like: href="filename.mid"
    midis = re.findall(r'href="([^"]+\.mid)"', html, re.IGNORECASE)
    # Filter out non-MIDI links and navigation
    midis = [m for m in midis if not m.startswith(('http', 'mailto', '/'))]
    return sorted(set(midis))

def parse_subdirectories(html):
    """Extract subdirectory links (game folders) from a directory listing."""
    dirs = re.findall(r'href="([^/]+\/)"', html)
    return sorted(set(dirs))

def build_catalog():
    """Build complete catalog of all MIDI files on VGMusic."""
    catalog = {}  # console -> game -> [filenames]
    total = 0

    for console_name, console_path in CONSOLES:
        print(f"Scanning {console_name}...", file=sys.stderr)
        html = fetch_page(console_path)
        if not html:
            continue

        # Check for subdirectories (game folders)
        subdirs = parse_subdirectories(html)
        
        if subdirs:
            # Console has game subdirectories
            for subdir in subdirs:
                sub_path = console_path + subdir
                sub_html = fetch_page(sub_path)
                if not sub_html:
                    continue
                files = parse_midi_listing(sub_html)
                if files:
                    game_name = subdir.rstrip('/').replace('_', ' ').replace('-', ' ')
                    catalog.setdefault(console_name, {})[game_name] = files
                    total += len(files)
                time.sleep(0.5)  # Be nice to the server
        else:
            # Flat directory
            files = parse_midi_listing(html)
            if files:
                catalog.setdefault(console_name, {})["_root"] = files
                total += len(files)

        time.sleep(1)  # Rate limit between consoles

    print(f"Total MIDI files found: {total}", file=sys.stderr)
    return catalog

def get_top_games(catalog, n=10000):
    """Get the top N games by file count."""
    games = []
    for console, game_dict in catalog.items():
        for game, files in game_dict.items():
            games.append((console, game, len(files), files))

    # Sort by file count descending
    games.sort(key=lambda x: x[2], reverse=True)

    # Take top N songs
    result = []
    total = 0
    for console, game, count, files in games:
        if total >= n:
            break
        take = min(count, n - total)
        for f in files[:take]:
            result.append((console, game, f))
        total += take

    return result

def download_midi(console_name, game_name, filename, output_dir):
    """Download a single MIDI file."""
    # Build URL
    if game_name == "_root":
        url = f"{BASE_URL}/music/console/{console_name}/{filename}"
    else:
        game_folder = game_name.replace(' ', '_')
        url = f"{BASE_URL}/music/console/{console_name}/{game_folder}/{filename}"

    # Determine console path
    console_path = None
    for cname, cpath in CONSOLES:
        if cname == console_name:
            console_path = cpath
            break
    if console_path is None:
        return False

    if game_name == "_root":
        url = BASE_URL + console_path + filename
    else:
        game_folder = game_name.replace(' ', '_')
        url = BASE_URL + console_path + game_folder + '/' + filename

    # Create output path
    out_dir = os.path.join(output_dir, console_name, game_name.replace('/', '_'))
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, filename)

    if os.path.exists(out_path):
        return True  # Already downloaded

    req = urllib.request.Request(url, headers={
        'User-Agent': 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)'
    })
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            with open(out_path, 'wb') as f:
                f.write(resp.read())
        return True
    except (urllib.error.URLError, OSError) as e:
        print(f"  Failed: {url}: {e}", file=sys.stderr)
        return False

def main():
    import argparse
    parser = argparse.ArgumentParser(description='VGMusic.com catalog tool')
    parser.add_argument('--build-catalog', action='store_true', help='Build catalog')
    parser.add_argument('--top', type=int, default=10000, help='Top N songs')
    parser.add_argument('--output', type=str, help='Output file')
    parser.add_argument('--download', type=str, help='Download list file')
    parser.add_argument('--dir', type=str, default='~/vgm', help='Download directory')
    args = parser.parse_args()

    if args.build_catalog:
        catalog = build_catalog()
        # Output as TSV: console\tgame\tfilename
        for console, game_dict in sorted(catalog.items()):
            for game, files in sorted(game_dict.items()):
                for f in files:
                    print(f"{console}\t{game}\t{f}")

    elif args.download:
        # Download from a catalog file
        output_dir = os.path.expanduser(args.dir)
        os.makedirs(output_dir, exist_ok=True)

        with open(args.download) as f:
            lines = f.readlines()

        total = len(lines)
        for i, line in enumerate(lines):
            parts = line.strip().split('\t')
            if len(parts) != 3:
                continue
            console, game, filename = parts
            if i % 100 == 0:
                print(f"Downloading {i+1}/{total}...", file=sys.stderr)
            download_midi(console, game, filename, output_dir)
            time.sleep(0.2)  # Rate limit

        print(f"Downloaded to {output_dir}", file=sys.stderr)

    else:
        # Default: build catalog and output top N
        catalog = build_catalog()
        top = get_top_games(catalog, args.top)
        for console, game, filename in top:
            print(f"{console}\t{game}\t{filename}")

if __name__ == '__main__':
    main()
