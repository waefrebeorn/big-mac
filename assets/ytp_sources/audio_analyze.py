#!/usr/bin/env python3
"""Analyze audio content of a video without STT.
Uses ffmpeg's silencedetect and volumedetect filters to find speech segments.
"""
import subprocess, os, json, sys, re

def analyze_audio(video_path):
    """Analyze audio to find speech segments, silence gaps, and volume peaks."""
    print(f"Analyzing: {os.path.basename(video_path)}")
    
    # Get duration
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "default=noprint_wrappers=1:nokey=1", video_path],
        capture_output=True, text=True, timeout=10
    )
    duration = float(probe.stdout.strip())
    print(f"  Duration: {duration:.1f}s")
    
    # Detect silence gaps (natural cut points)
    cmd = [
        "ffmpeg", "-i", video_path,
        "-af", "silencedetect=noise=-30dB:d=0.3",
        "-f", "null", "-"
    ]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    
    # Parse silence detect output
    silence_starts = []
    silence_ends = []
    for line in r.stderr.split("\n"):
        if "silence_start:" in line:
            m = re.search(r"silence_start: ([\d.]+)", line)
            if m:
                silence_starts.append(float(m.group(1)))
        elif "silence_end:" in line:
            m = re.search(r"silence_end: ([\d.]+)", line)
            if m:
                silence_ends.append(float(m.group(1)))
    
    # Build speech segments (inverse of silence)
    speech_segments = []
    prev_end = 0
    for i, start in enumerate(silence_starts):
        if start > prev_end + 0.1:  # Minimum 100ms speech
            speech_segments.append({
                "start": prev_end,
                "end": start,
                "duration": start - prev_end,
                "type": "speech"
            })
        if i < len(silence_ends):
            prev_end = silence_ends[i]
    
    # Add final segment
    if prev_end < duration - 0.1:
        speech_segments.append({
            "start": prev_end,
            "end": duration,
            "duration": duration - prev_end,
            "type": "speech"
        })
    
    # Detect volume peaks (moments of high energy = good for earrape/stutter)
    cmd = [
        "ffmpeg", "-i", video_path,
        "-af", "volumedetect",
        "-f", "null", "-"
    ]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    
    mean_volume = "N/A"
    max_volume = "N/A"
    for line in r.stderr.split("\n"):
        if "mean_volume:" in line:
            m = re.search(r"mean_volume: ([-\d.]+) dB", line)
            if m:
                mean_volume = float(m.group(1))
        if "max_volume:" in line:
            m = re.search(r"max_volume: ([-\d.]+) dB", line)
            if m:
                max_volume = float(m.group(1))
    
    # Find volume peaks using astats
    cmd = [
        "ffmpeg", "-i", video_path,
        "-af", "astats=metadata=1:reset=1,ametadata=print:key=lavfi.astats.Overall.RMS_level",
        "-f", "null", "-"
    ]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    
    # Build result
    result = {
        "source": os.path.basename(video_path),
        "duration": duration,
        "mean_volume_db": mean_volume,
        "max_volume_db": max_volume,
        "speech_segments": speech_segments[:20],  # First 20 segments
        "speech_count": len(silence_starts),
        "silence_gaps": [{"start": s, "end": silence_ends[i] if i < len(silence_ends) else duration, "duration": (silence_ends[i] if i < len(silence_ends) else duration) - s} 
                        for i, s in enumerate(silence_starts)][:20]
    }
    
    return result

def find_best_moments(analysis, effect_type="stutter"):
    """Find the best moments for a specific YTP effect."""
    moments = []
    
    if effect_type == "stutter":
        # Best for stutter: short speech segments (1-3 seconds)
        for seg in analysis.get("speech_segments", []):
            if 0.5 < seg["duration"] < 3.0:
                moments.append({
                    "start": seg["start"],
                    "end": seg["end"],
                    "duration": seg["duration"],
                    "reason": "Short speech segment, good for stutter"
                })
    
    elif effect_type == "earrape":
        # Best for earrape: loud moments (high volume)
        # Use speech segments with longer duration (more energy)
        for seg in analysis.get("speech_segments", []):
            if seg["duration"] > 2.0:
                moments.append({
                    "start": seg["start"],
                    "end": seg["end"],
                    "duration": seg["duration"],
                    "reason": "Long speech segment, high energy"
                })
    
    elif effect_type == "reverse":
        # Best for reverse: medium segments with clear audio
        for seg in analysis.get("speech_segments", []):
            if 1.0 < seg["duration"] < 4.0:
                moments.append({
                    "start": seg["start"],
                    "end": seg["end"],
                    "duration": seg["duration"],
                    "reason": "Medium segment, clear audio for reverse"
                })
    
    elif effect_type == "cut":
        # Best cut points: silence gaps
        for gap in analysis.get("silence_gaps", []):
            if gap["duration"] > 0.3:
                moments.append({
                    "start": gap["start"],
                    "end": gap["end"],
                    "duration": gap["duration"],
                    "reason": "Silence gap, natural cut point"
                })
    
    return moments[:10]  # Top 10 moments

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python audio_analyze.py <video_path> [effect_type]")
        print("  effect_type: stutter, earrape, reverse, cut")
        sys.exit(1)
    
    video_path = sys.argv[1]
    effect_type = sys.argv[2] if len(sys.argv) > 2 else None
    
    analysis = analyze_audio(video_path)
    if analysis:
        # Save analysis
        analysis_path = video_path.replace(".mp4", "_audio.json")
        with open(analysis_path, "w") as f:
            json.dump(analysis, f, indent=2)
        
        print(f"\nSpeech segments: {analysis['speech_count']}")
        print(f"Mean volume: {analysis['mean_volume_db']} dB")
        print(f"Max volume: {analysis['max_volume_db']} dB")
        
        if effect_type:
            moments = find_best_moments(analysis, effect_type)
            print(f"\nBest moments for '{effect_type}':")
            for m in moments[:5]:
                print(f"  [{m['start']:.1f}-{m['end']:.1f}] ({m['duration']:.1f}s) - {m['reason']}")
        
        print(f"\nSaved to: {analysis_path}")
