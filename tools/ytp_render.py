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
    """Extract a segment with effects applied.
    Uses input seeking (-ss before -i) for speed, with output seeking for reverse.
    """
    is_reverse = "reverse" in (vfilter or "")
    
    if is_reverse:
        # For reverse, we need output seeking (decode from start, trim, then reverse)
        # Extract a larger window to ensure we have enough frames
        seek_start = max(0, start_sec - 5)
        cmd = [FFMPEG, "-y", "-ss", f"{seek_start:.3f}", "-i", src_path,
               "-t", f"{dur_sec + 10:.3f}"]
    else:
        # Fast path: input seeking (works well for most files)
        cmd = [FFMPEG, "-y", "-ss", f"{start_sec:.3f}", "-i", src_path, "-t", f"{dur_sec:.3f}"]
    
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

def _extract_one(args):
    """Worker function for parallel segment extraction."""
    i, seg, tmpdir = args
    src = seg["source"]
    start = seg["source_start_ms"] / 1000.0
    dur = seg["source_dur_ms"] / 1000.0
    effect = seg.get("effect", "NONE")
    intensity = seg.get("intensity", 5)
    text = seg.get("text_overlay", "")
    
    seg_file = os.path.join(tmpdir, f"seg_{i:04d}.mp4")
    
    vf, af = EFFECT_FILTERS.get(effect, ("", ""))
    
    if effect == "SUBTITLE" and text:
        vf = f"drawtext=text='{text}':fontsize=42:fontcolor=yellow:borderw=4:bordercolor=black:x=(w-text_w)/2:y=h-th-30"
    elif effect == "TO_BE_CONTINUED":
        fade_st = max(0, dur - 0.7)
        vf = f"fade=t=out:st={fade_st:.1f}:d=0.5,eq=brightness=-0.1"
        af = f"fade=t=out:st={fade_st:.1f}:d=0.5"
    elif effect == "STUTTER":
        vf = f"loop={intensity}:1:0"
    
    ok = extract_segment(src, start, dur, seg_file, vf, af, intensity)
    return (i, seg_file if ok else None)


def render_composition(comp, output_path):
    """Render a composition to video."""
    segments = comp["segments"]
    n = len(segments)
    
    print(f"Rendering {n} segments...")
    
    # Create temp directory
    tmpdir = tempfile.mkdtemp(prefix="ytp_")
    seg_files = []
    
    # Step 1: Extract each segment (in parallel)
    from concurrent.futures import ProcessPoolExecutor, as_completed
    
    print(f"  Extracting {n} segments (parallel)...")
    
    # Use 4 parallel workers (matching dual-core iMac with hyperthreading)
    with ProcessPoolExecutor(max_workers=4) as executor:
        futures = {executor.submit(_extract_one, (i, seg, tmpdir)): i for i, seg in enumerate(segments)}
        completed = 0
        for future in as_completed(futures):
            i, seg_file = future.result()
            if seg_file:
                seg_files.append(seg_file)
            completed += 1
            if completed % 20 == 0:
                print(f"    {completed}/{n}...")
    
    seg_files.sort()  # Ensure correct order
    print(f"  Extracted {len(seg_files)}/{n} segments successfully")
    
    if len(seg_files) == 0:
        print("ERROR: no valid segments")
        return False
    
    if len(seg_files) == 1:
        # Just copy
        import shutil
        shutil.copy(seg_files[0], output_path)
        return True
    
    # Step 2: Build xfade chain (in batches if needed)
    fade_dur = 0.15  # 150ms crossfade
    BATCH_SIZE = 50  # ffmpeg can handle ~50 inputs at once
    
    if len(seg_files) <= BATCH_SIZE:
        # Single-pass compositing
        ok = _xfade_composite(seg_files, segments, fade_dur, output_path)
    else:
        # Multi-pass: composite batches, then chain batches
        print(f"  Using batch compositing ({BATCH_SIZE} per batch)...")
        batch_files = []
        n_batches = (len(seg_files) + BATCH_SIZE - 1) // BATCH_SIZE
        
        for b in range(n_batches):
            start_idx = b * BATCH_SIZE
            end_idx = min(start_idx + BATCH_SIZE, len(seg_files))
            batch_segs = seg_files[start_idx:end_idx]
            batch_segments = segments[start_idx:end_idx]
            
            batch_out = os.path.join(tmpdir, f"batch_{b:03d}.mp4")
            print(f"    Batch {b+1}/{n_batches}: segments {start_idx}-{end_idx}")
            ok = _xfade_composite(batch_segs, batch_segments, fade_dur, batch_out)
            if ok:
                batch_files.append(batch_out)
        
        if len(batch_files) == 0:
            print("ERROR: no valid batches")
            return False
        elif len(batch_files) == 1:
            import shutil
            shutil.copy(batch_files[0], output_path)
            return True
        else:
            # Chain batches with concat demuxer
            print(f"  Chaining {len(batch_files)} batches...")
            concat_list = os.path.join(tmpdir, "concat.txt")
            with open(concat_list, "w") as f:
                for bf in batch_files:
                    f.write(f"file '{bf}'\n")
            cmd = [FFMPEG, "-y", "-f", "concat", "-safe", "0", "-i", concat_list,
                "-c:v", "libx264", "-preset", "ultrafast", "-crf", "28", "-r", "24",
                "-c:a", "aac", "-b:a", "64k", "-movflags", "+faststart",
                output_path]
            print(f"  Final concat of {len(batch_files)} batches...")
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
            if r.returncode != 0:
                print(f"  Batch chain failed: {r.stderr[-300:]}")
                # Fallback: just use first batch
                import shutil
                shutil.copy(batch_files[0], output_path)
    
    # Cleanup
    import shutil
    shutil.rmtree(tmpdir, ignore_errors=True)
    
    if os.path.exists(output_path):
        size_mb = os.path.getsize(output_path) / 1024 / 1024
        print(f"✓ Done! {output_path} ({size_mb:.1f} MB)")
        return True
    else:
        print("✗ Render failed: no output file")
        return False


def _xfade_composite(seg_files, segments, fade_dur, output_path):
    """Composite a list of segments with xfade/acrossfade."""
    cmd = [FFMPEG, "-y"]
    for sf in seg_files:
        cmd.extend(["-i", sf])
    
    filter_parts = []
    cumulative = 0.0
    
    # Video xfade chain
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
    
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    return r.returncode == 0 and os.path.exists(output_path)


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
