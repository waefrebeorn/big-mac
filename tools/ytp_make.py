#!/usr/bin/env python3
"""ytp_make.py — Full YTP pipeline: transcript → composition → render.

Usage: python3 ytp_make.py <video.mp4> <transcript.json> <output.mp4> [--chaos 7]
"""

import json
import subprocess
import os
import sys
import random
import tempfile
from pathlib import Path

def load_transcript(path):
    """Load word-level transcript JSON."""
    with open(path) as f:
        data = json.load(f)
    return data.get("words", [])

def build_composition(words, source_path, chaos=7):
    """Build a YTP composition from word timestamps."""
    if not words:
        print("ERROR: no words in transcript")
        return None
    
    n_words = len(words)
    
    # Group words into phrases (segments)
    segments = []
    i = 0
    while i < n_words:
        remaining = n_words - i
        if remaining <= 0:
            break
        # Phrase length: min(3, remaining) to min(7, remaining)
        phrase_len = random.randint(min(2, remaining), min(7, remaining))
        
        # Check for natural break (pause > 250ms or punctuation)
        for j in range(i, min(i + phrase_len, n_words)):
            if j > i and j < n_words:
                pause = words[j]["start"] - words[j-1]["end"]
                if pause > 250:
                    phrase_len = j - i
                    break
            w = words[j].get("word", "")
            if w and w[-1] in '.!?' and j > i:
                phrase_len = j - i + 1
                break
        
        phrase_len = max(1, min(phrase_len, n_words - i))
        
        start_ms = words[i]["start"]
        end_ms = words[i + phrase_len - 1]["end"]
        dur_ms = end_ms - start_ms
        
        if dur_ms < 50:
            dur_ms = 150
        
        text = " ".join(w.get("word", "") for w in words[i:i+phrase_len])
        
        # Determine segment type based on plot position
        plot_pos = i / n_words
        
        if i == 0:
            seg_type = "INTRO"
        elif i >= n_words - phrase_len:
            seg_type = "OUTRO"
        elif plot_pos < 0.2:
            seg_type = "SETUP"
        elif plot_pos < 0.5:
            seg_type = "ESCALATE"
        elif plot_pos < 0.8:
            seg_type = "CLIMAX"
        else:
            seg_type = "RESOLVE"
        
        # Choose effect based on chaos and position
        effect = choose_effect(plot_pos, chaos, text, seg_type)
        
        segments.append({
            "source": source_path,
            "source_start_ms": start_ms,
            "source_dur_ms": dur_ms,
            "timeline_dur_ms": dur_ms,
            "effect": effect["name"],
            "intensity": effect["intensity"],
            "text_overlay": effect.get("text", ""),
            "description": f"{effect['name']}: \"{text[:50]}\"",
            "type": seg_type,
        })
        
        i += phrase_len
    
    return {"segments": segments, "title": "YTP", "chaos": chaos}

def choose_effect(plot_pos, chaos, text, seg_type):
    """Choose an effect based on content and plot position."""
    r = random.random()
    
    # Content signals
    has_exclaim = "!" in text
    has_question = "?" in text
    has_the = "the " in text.lower() or "The " in text
    word_count = len(text.split())
    is_short = word_count <= 3
    is_long = word_count >= 6
    
    # Base probability of any effect
    if plot_pos < 0.2:
        fx_prob = chaos * 0.05  # 5-50%
    elif plot_pos < 0.5:
        fx_prob = 0.3 + chaos * 0.05  # 35-80%
    elif plot_pos < 0.8:
        fx_prob = 0.5 + chaos * 0.04  # 50-90%
    else:
        fx_prob = 0.2 + chaos * 0.03  # 20-50%
    
    if r > fx_prob:
        return {"name": "NONE", "intensity": 1}
    
    # Content-aware selection
    if has_the and random.random() < 0.3:
        return {"name": "STUTTER", "intensity": random.randint(2, 5)}
    if has_question and random.random() < 0.25:
        return {"name": "REVERSE", "intensity": 5}
    if has_exclaim and random.random() < 0.35:
        return {"name": "EARRAPE", "intensity": random.randint(5, 10)}
    if is_short and random.random() < 0.25:
        return {"name": "VINE_BOOM", "intensity": 5}
    if is_long and random.random() < 0.3:
        return {"name": "SENTENCE_MIX", "intensity": 5}
    
    # Position-based
    if seg_type == "CLIMAX":
        pool = ["DEEP_FRY", "DATAMOSH", "SCRAMBLE", "SONIC_SCREAM", "VHS", "ZOOM", "SHAKE"]
    elif seg_type == "ESCALATE":
        pool = ["SLOWMO", "FASTFORWARD", "PITCH_UP", "PITCH_DOWN", "SENTENCE_MIX", "FLASH"]
    elif seg_type == "SETUP":
        pool = ["SLOWMO", "PITCH_UP", "SUBTITLE"]
    elif seg_type == "OUTRO":
        pool = ["SLOWMO", "FREEZE", "TO_BE_CONTINUED"]
    else:
        pool = ["NONE"]
    
    effect_name = random.choice(pool)
    return {"name": effect_name, "intensity": random.randint(3, 8)}

def main():
    import argparse
    parser = argparse.ArgumentParser(description="Make a YTP from video + transcript")
    parser.add_argument("video", help="Input video path")
    parser.add_argument("transcript", help="Word-level transcript JSON")
    parser.add_argument("output", help="Output video path")
    parser.add_argument("--chaos", type=int, default=7, help="Chaos level 1-10")
    parser.add_argument("--seed", type=int, default=None, help="Random seed")
    args = parser.parse_args()
    
    if args.seed is not None:
        random.seed(args.seed)
    
    # Load transcript
    words = load_transcript(args.transcript)
    print(f"Loaded {len(words)} words from transcript")
    
    # Build composition
    comp = build_composition(words, args.video, args.chaos)
    if not comp:
        sys.exit(1)
    
    print(f"Built composition: {len(comp['segments'])} segments")
    
    # Save composition JSON
    comp_path = args.output + ".composition.json"
    with open(comp_path, "w") as f:
        json.dump(comp, f, indent=2)
    print(f"Composition saved to {comp_path}")
    
    # Render
    print(f"\nRendering to {args.output}...")
    result = subprocess.run([sys.executable, __file__.replace("ytp_make.py", "ytp_render.py"),
                           comp_path, args.output])
    sys.exit(result.returncode)

if __name__ == "__main__":
    main()
