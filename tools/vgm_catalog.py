#!/usr/bin/env python3
"""
tools/vgm_catalog.py — VGMusic.com catalog + download tool (R096).

SOURCES (in order of preference):
1. 2011 MediaFire zip snapshot (22,000+ files, single download):
   http://www.mediafire.com/download/i7q64yoj9j27xbu/2011-03-12-vgmusic.com.zip
   - Download once, extract locally. No repeated requests to VGMusic.

2. VGMusic directory listings (for files added after 2011):
   - Only fetches index HTML pages, never individual MIDI files.
   - Rate limited: 1s between console pages, 0.5s between game pages.

Usage:
    python3 tools/vgm_catalog.py --build-catalog > vgm_catalog.tsv
    python3 tools/vgm_catalog.py --top 10000 --output vgm_top10k.tsv
    python3 tools/vgm_catalog.py --extract-zip vgm_2011.zip --dir ~/vgm/
    
Do NOT use --download to fetch individual files from VGMusic.
Use the MediaFire zip or archive.org instead.
"""

import os
import sys
import re
import time
import zipfile
import urllib.request
import urllib.error
from collections import defaultdict

BASE_URL = "https://www.vgmusic.com"

# 2011 full archive snapshot (22,000+ MIDI files)
MEDIAFIRE_ZIP = "https://www.mediafire.com/file/i7q64yoj9j27xbu/2011-03-12-vgmusic.com.zip"

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

def extract_zip(zip_path, output_dir):
    """Extract VGMusic zip and catalog all files."""
    print(f"Extracting {zip_path}...", file=sys.stderr)
    catalog = []
    with zipfile.ZipFile(zip_path, 'r') as zf:
        for info in zf.infolist():
            if info.filename.lower().endswith('.mid'):
                # Path format: vgmusic.com/music/console/nintendo/snes/filename.mid
                parts = info.filename.split('/')
                if len(parts) >= 5 and parts[1] == 'music':
                    console = parts[3] if parts[2] == 'console' else parts[2]
                    game = parts[4] if len(parts) > 4 else '_root_'
                    filename = parts[-1]
                    catalog.append((console, game, filename))
        zf.extractall(output_dir)
    print(f"Extracted {len(catalog)} MIDI files to {output_dir}", file=sys.stderr)
    return catalog

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

# download_midi removed — use MediaFire zip instead

def main():
    import argparse
    parser = argparse.ArgumentParser(description='VGMusic.com catalog tool')
    parser.add_argument('--build-catalog', action='store_true', help='Build catalog from index')
    parser.add_argument('--top', type=int, default=10000, help='Top N songs')
    parser.add_argument('--output', type=str, help='Output file')
    parser.add_argument('--extract-zip', type=str, help='Extract VGMusic zip file')
    parser.add_argument('--dir', type=str, default='~/vgm', help='Output directory')
    args = parser.parse_args()

    if args.extract_zip:
        output_dir = os.path.expanduser(args.dir)
        catalog = extract_zip(args.extract_zip, output_dir)
        for console, game, filename in catalog:
            print(f"{console}\t{game}\t{filename}")
    elif args.build_catalog:
        catalog = build_catalog()
        for console, game_dict in sorted(catalog.items()):
            for game, files in sorted(game_dict.items()):
                for f in files:
                    print(f"{console}\t{game}\t{f}")
    else:
        # Default: build catalog and output top N
        catalog = build_catalog()
        top = get_top_games(catalog, args.top)
        for console, game, filename in top:
            print(f"{console}\t{game}\t{filename}")

if __name__ == '__main__':
    main()
