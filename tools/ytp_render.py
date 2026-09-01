#!/usr/bin/env python3
"""ytp_render.py — Proper YTP renderer using ffmpeg segment extraction + xfade.

This is the CORRECT approach:
1. Extract each segment as a temp file with effects applied via ffmpeg
2. Build an xfade filter chain for smooth video transitions
3. Build an acrossfade filter chain for smooth audio transitions
4. Render everything in one pass

Usage: python3 ytp_render.py <composition.json> <output.mp4>
"""

import json
import subprocess
import os
import sys
import tempfile
from pathlib import Path

FFMPEG = "ffmpeg"
FFPROBE = "ffprobe"

def probe_duration(path):
    try:
        r = subprocess.run([FFPROBE, "-v", "error", "-show_entries", "format=duration",
                           "-of", "default=noprint_wrappers=1:nokey=1", path],
                          capture_output=True, text=True, timeout=10)
        return float(r.stdout.strip())
    except:
        return 0.0

def extract_segment(src_path, start_sec, dur_sec, output_path, vfilter, afilter, intensity):
    """Extract a segment with effects applied."""
    cmd = [FFMPEG, "-y", "-ss", f"{start_sec:.3f}", "-t", f"{dur_sec:.3f}",
           "-i", src_path]
    
    vf_parts = []
    if vfilter:
        vf_parts.append(vfilter)
    vf_parts.extend([
        "scale=854:480:force_original_aspect_ratio=decrease",
        "pad=854:480:(ow-iw)/2:(oh-ih)/2",
        "setsar=1", "fps=24", "format=yuv420p"
    ])
    cmd.extend(["-vf", ",".join(vf_parts)])
    
    af_parts = []
    if afilter:
        af_parts.append(afilter)
    af_parts.append("aformat=sample_fmts=fltp:sample_rates=44100:channel_layouts=stereo")
    cmd.extend(["-af", ",".join(af_parts)])
    
    cmd.extend([
        "-c:v", "libx264", "-preset", "ultrafast", "-crf", "28", "-r", "24",
        "-c:a", "aac", "-b:a", "64k",
        output_path
    ])
    
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        # Fallback: extract without effects
        cmd_simple = [FFMPEG, "-y", "-ss", f"{start_sec:.3f}", "-t", f"{dur_sec:.3f}",
                     "-i", src_path,
                     "-vf", "scale=854:480:force_original_aspect_ratio=decrease,pad=854:480:(ow-iw)/2:(oh-ih)/2,setsar=1,fps=24,format=yuv420p",
                     "-af", "aformat=sample_fmts=fltp:sample_rates=44100:channel_layouts=stereo",
                     "-c:v", "libx264", "-preset", "ultrafast", "-crf", "28", "-r", "24",
                     "-c:a", "aac", "-b:a", "64k",
                     output_path]
        r = subprocess.run(cmd_simple, capture_output=True, text=True, timeout=300)
    
    return r.returncode == 0 and os.path.exists(output_path)

EFFECT_FILTERS = {
    "NONE": ("", ""),
    "STUTTER": ("", ""),  # handled by repeating segment
    "SLOWMO": ("setpts=1.5*PTS", "atempo=0.67"),
    "FASTFORWARD": ("setpts=0.5*PTS", "atempo=2.0"),
    "REVERSE": ("reverse", "areverse"),
    "PITCH_UP": ("", "asetrate=44100*1.5,aresample=44100"),
    "PITCH_DOWN": ("", "asetrate=44100*0.6,aresample=44100"),
    "EARRAPE": ("", "volume=8"),
    "DEEP_FRY": ("eq=contrast=1.5:brightness=0.05:saturation=3,unsharp=7:7:5", "volume=2"),
    "VHS": ("noise=alls=20:allf=t+u,colorchannelmixer=0.3:0.4:0.3:0:0.3:0.4:0.3:0:0.3:0.4:0.3,eq=contrast=1.2:brightness=-0.05:saturation=0.7", ""),
    "SHAKE": ("crop=iw*0.92:ih*0.92:(iw-iw*0.92)*random(1):(ih-ih*0.92)*random(2),scale=iw:ih", ""),
    "ZOOM": ("zoompan=z='1+0.5*in/50':d=1:x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)':fps=24", ""),
    "FREEZE": ("trim=start_frame=0:end_frame=1,loop=-1:1:0,setpts=N/FRAME_RATE/TB", ""),
    "FLASH": ("geq='lum=255':a=255", "volume=0"),
    "SENTENCE_MIX": ("setpts=0.7*PTS", "atempo=1.4,asetrate=44100*1.3,aresample=44100"),
    "SCRAMBLE": ("colorchannelmixer=0:0:1:0:1:0:0:0:0:1:0:0,eq=contrast=1.5:hue=180", ""),
    "DATAMOSH": ("noise=alls=80:allf=t+u,eq=contrast=2:saturation=3", ""),
    "SUBTITLE": (None, ""),  # special case - text overlay
    "SONIC_SCREAM": ("eq=contrast=2:saturation=0,noise=alls=20:allf=t", "volume=3,asetrate=44100*2,aresample=44100"),
    "TO_BE_CONTINUED": (None, None),  # special case - fade out
    "BLEEP": ("", "asplit=2[orig][bleep];[bleep]sine=frequency=1000:duration=0.3,volume=0.8[b2];[orig][b2]amix=inputs=2:duration=first:normalize=0"),
    "KALEIDO": ("split=4[a][b][c][d];[a]crop=iw/2:ih/2:0:0,transpose=1[a1];[b]crop=iw/2:ih/2:iw/2:0,transpose=2[b1];[c]crop=iw/2:ih/2:0:ih/2,transpose=1[c1];[d]crop=iw/2:ih/2:iw/2:ih/2,transpose=2[d1];[a1][b1]hstack=inputs=2[top];[c1][d1]hstack=inputs=2[bot];[top][bot]vstack=inputs=2", ""),
    "VOICE_CHANGE": ("", "asetrate=44100*0.7,aresample=44100"),
    "MEME_SOUND": ("", "volume=1.5"),
    "VINE_BOOM": ("", "volume=2"),
    "MOCAP_OVERLAY": ("", ""),
}

def render_composition(comp, output_path):
    """Render a composition to video."""
    segments = comp["segments"]
    n = len(segments)
    
    print(f"Rendering {n} segments...")
    
    # Create temp directory
    tmpdir = tempfile.mkdtemp(prefix="ytp_")
    seg_files = []
    
    # Step 1: Extract each segment
    for i, seg in enumerate(segments):
        src = seg["source"]
        start = seg["source_start_ms"] / 1000.0
        dur = seg["source_dur_ms"] / 1000.0
        effect = seg.get("effect", "NONE")
        intensity = seg.get("intensity", 5)
        text = seg.get("text_overlay", "")
        
        seg_file = os.path.join(tmpdir, f"seg_{i:04d}.mp4")
        
        # Get effect filters
        vf, af = EFFECT_FILTERS.get(effect, ("", ""))
        
        # Special cases
        if effect == "SUBTITLE" and text:
            vf = f"drawtext=text='{text}':fontsize=42:fontcolor=yellow:borderw=4:bordercolor=black:x=(w-text_w)/2:y=h-th-30"
        elif effect == "TO_BE_CONTINUED":
            fade_st = max(0, dur - 0.7)
            vf = f"fade=t=out:st={fade_st:.1f}:d=0.5,eq=brightness=-0.1"
            af = f"fade=t=out:st={fade_st:.1f}:d=0.5"
        elif effect == "STUTTER":
            # Stutter = loop the segment N times in place
            vf = f"loop={intensity}:1:0"
        
        ok = extract_segment(src, start, dur, seg_file, vf, af, intensity)
        if ok:
            seg_files.append(seg_file)
        
        if i % 10 == 0:
            print(f"  Extracted {i+1}/{n}")
    
    print(f"  Extracted {len(seg_files)}/{n} segments successfully")
    
    if len(seg_files) == 0:
        print("ERROR: no valid segments")
        return False
    
    if len(seg_files) == 1:
        # Just copy
        import shutil
        shutil.copy(seg_files[0], output_path)
        return True
    
    # Step 2: Build xfade chain
    print("Building transitions...")
    
    fade_dur = 0.15  # 150ms crossfade
    
    # Build command
    cmd = [FFMPEG, "-y"]
    for sf in seg_files:
        cmd.extend(["-i", sf])
    
    # Build filter_complex
    filter_parts = []
    
    # Video xfade chain
    cumulative = 0.0
    for i in range(len(seg_files)):
        if i == 0:
            filter_parts.append(f"[{i}:v]copy[vx0]")
        else:
            prev_dur = segments[i-1]["source_dur_ms"] / 1000.0
            cumulative += prev_dur - fade_dur
            filter_parts.append(f"[vx{i-1}][{i}:v]xfade=transition=fade:duration={fade_dur}:offset={cumulative:.3f}[vx{i}]")
    
    # Audio acrossfade chain
    for i in range(len(seg_files)):
        if i == 0:
            filter_parts.append(f"[{i}:a]acopy[ax0]")
        else:
            filter_parts.append(f"[ax{i-1}][{i}:a]acrossfade=d={fade_dur}:c1=tri:c2=tri[ax{i}]")
    
    last_v = f"vx{len(seg_files)-1}"
    last_a = f"ax{len(seg_files)-1}"
    
    filter_str = ";".join(filter_parts)
    cmd.extend([
        "-filter_complex", filter_str,
        "-map", f"[{last_v}]", "-map", f"[{last_a}]",
        "-c:v", "libx264", "-preset", "ultrafast", "-crf", "28", "-r", "24",
        "-c:a", "aac", "-b:a", "64k", "-movflags", "+faststart",
        output_path
    ])
    
    print(f"Compositing {len(seg_files)} segments with crossfades...")
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    
    # Cleanup
    import shutil
    shutil.rmtree(tmpdir, ignore_errors=True)
    
    if r.returncode == 0 and os.path.exists(output_path):
        size_mb = os.path.getsize(output_path) / 1024 / 1024
        print(f"✓ Done! {output_path} ({size_mb:.1f} MB)")
        return True
    else:
        print(f"✗ Render failed: {r.stderr[-500:]}")
        return False


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 ytp_render.py <composition.json> <output.mp4>")
        sys.exit(1)
    
    comp_path = sys.argv[1]
    output_path = sys.argv[2]
    
    with open(comp_path) as f:
        comp = json.load(f)
    
    success = render_composition(comp, output_path)
    sys.exit(0 if success else 1)
