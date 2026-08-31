#!/usr/bin/env python3
"""YTP Transcript Pipeline — uses Big Mac's whisper.cpp engine.
Transcribes a video, finds speech segments, identifies best moments for effects.
"""
import subprocess, os, json, sys

WHISPER_CLI = "/Users/waefrebeorn/whisper.cpp/build/bin/whisper-cli"
WHISPER_MODEL = "/Users/waefrebeorn/whisper.cpp/models/ggml-tiny.en-q5_1.bin"

def extract_audio(video_path, wav_path):
    """Extract audio as 16kHz mono WAV for whisper."""
    cmd = [
        "ffmpeg", "-y", "-i", video_path,
        "-vn", "-acodec", "pcm_s16le", "-ar", "16000", "-ac", "1",
        wav_path
    ]
    r = subprocess.run(cmd, capture_output=True, timeout=120)
    return r.returncode == 0 and os.path.exists(wav_path)

def transcribe(video_path):
    """Transcribe video using whisper.cpp."""
    wav_path = "/tmp/ytp_transcribe.wav"
    srt_path = "/tmp/ytp_transcribe.wav.srt"
    txt_path = "/tmp/ytp_transcribe.wav.txt"
    
    # Extract audio
    if not extract_audio(video_path, wav_path):
        return None
    
    # Run whisper.cpp (outputs SRT and TXT next to the WAV file)
    cmd = [
        WHISPER_CLI, "-m", WHISPER_MODEL,
        "-f", wav_path,
        "-osrt", "-otxt",
        "--language", "en"
    ]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd="/tmp", timeout=600)
    
    result = {"video": os.path.basename(video_path), "segments": []}
    
    # Read TXT
    if os.path.exists(txt_path):
        with open(txt_path) as f:
            result["text"] = f.read().strip()
    
    # Read SRT
    if os.path.exists(srt_path):
        with open(srt_path) as f:
            result["srt"] = f.read()
    
    # Parse SRT into segments
    if result.get("srt"):
        import re
        blocks = result["srt"].strip().split("\n\n")
        for block in blocks:
            lines = block.strip().split("\n")
            if len(lines) >= 3:
                ts_match = re.search(r'(\d+):(\d+):(\d+)[,.](\d+)\s*-->\s*(\d+):(\d+):(\d+)[,.](\d+)', lines[1])
                if ts_match:
                    g = [int(x) for x in ts_match.groups()]
                    start = g[0]*3600 + g[1]*60 + g[2] + g[3]/1000
                    end = g[4]*3600 + g[5]*60 + g[6] + g[7]/1000
                    text = " ".join(lines[2:]).strip()
                    result["segments"].append({
                        "start": start,
                        "end": end,
                        "duration": end - start,
                        "text": text
                    })
    
    # Clean up
    for p in [wav_path, srt_path, txt_path]:
        if os.path.exists(p):
            os.remove(p)
    
    return result

def find_ytp_moments(transcript, effect_type="stutter"):
    """Find best moments for YTP effects from transcript."""
    moments = []
    segments = transcript.get("segments", [])
    
    if effect_type == "stutter":
        # Short segments with clear speech
        for seg in segments:
            if 0.3 < seg["duration"] < 2.0 and len(seg["text"].split()) <= 3:
                moments.append({
                    "start": seg["start"],
                    "end": seg["end"],
                    "duration": seg["duration"],
                    "text": seg["text"],
                    "reason": f"Short phrase: '{seg['text']}'"
                })
    
    elif effect_type == "earrape":
        # Longer segments with more words (more energy)
        for seg in segments:
            if seg["duration"] > 1.5 and len(seg["text"].split()) >= 3:
                moments.append({
                    "start": seg["start"],
                    "end": seg["end"],
                    "duration": seg["duration"],
                    "text": seg["text"],
                    "reason": f"Long phrase: '{seg['text']}'"
                })
    
    elif effect_type == "reverse":
        # Medium segments with clear audio
        for seg in segments:
            if 0.5 < seg["duration"] < 3.0:
                moments.append({
                    "start": seg["start"],
                    "end": seg["end"],
                    "duration": seg["duration"],
                    "text": seg["text"],
                    "reason": f"Clear audio: '{seg['text']}'"
                })
    
    elif effect_type == "chipmunk":
        # Any speech segment (pitch shift works on all)
        for seg in segments:
            if seg["duration"] > 0.5:
                moments.append({
                    "start": seg["start"],
                    "end": seg["end"],
                    "duration": seg["duration"],
                    "text": seg["text"],
                    "reason": f"Speech: '{seg['text']}'"
                })
    
    elif effect_type == "demon":
        # Deeper voices sound better demonized
        for seg in segments:
            if seg["duration"] > 1.0:
                moments.append({
                    "start": seg["start"],
                    "end": seg["end"],
                    "duration": seg["duration"],
                    "text": seg["text"],
                    "reason": f"Deep voice: '{seg['text']}'"
                })
    
    return sorted(moments, key=lambda x: x["duration"])[:10]

def make_ytp(video_path, transcript, output_dir, prefix="ytp"):
    """Generate all YTP effects for a video."""
    moments = {}
    for effect in ["stutter", "earrape", "chipmunk", "demon", "reverse", "fried"]:
        moments[effect] = find_ytp_moments(transcript, effect)
    
    results = {}
    
    for effect, moment_list in moments.items():
        if not moment_list:
            continue
        
        # Pick the best moment
        best = moment_list[0]
        start = best["start"]
        end = best["end"]
        
        # Extract clip
        clip_path = f"/tmp/{prefix}_{effect}_clip.mp4"
        cmd = ["ffmpeg", "-y", "-ss", str(start), "-i", video_path, "-t", str(end-start+0.5),
            "-c:v", "libx264", "-preset", "fast", "-crf", "22", "-c:a", "aac", "-ar", "44100", clip_path]
        subprocess.run(cmd, capture_output=True, timeout=60)
        
        if not os.path.exists(clip_path) or os.path.getsize(clip_path) < 5000:
            continue
        
        output_path = os.path.join(output_dir, f"{prefix}_{effect}.mp4")
        
        if effect == "stutter":
            # Extract 0.3s segment and repeat
            seg = f"/tmp/{prefix}_stut_seg.mp4"
            subprocess.run(["ffmpeg", "-y", "-i", clip_path, "-t", "0.3", "-c", "copy", seg], capture_output=True, timeout=30)
            if os.path.exists(seg) and os.path.getsize(seg) > 500:
                concat = "|".join([seg] * 8)
                subprocess.run(["ffmpeg", "-y", "-i", f"concat:{concat}", "-c", "copy", output_path], capture_output=True, timeout=30)
        
        elif effect == "chipmunk":
            subprocess.run(["ffmpeg", "-y", "-i", clip_path, "-af", "asetrate=44100*2.5,aresample=44100", "-c:v", "copy", output_path], capture_output=True, timeout=60)
        
        elif effect == "demon":
            subprocess.run(["ffmpeg", "-y", "-i", clip_path, "-af", "asetrate=44100*0.3,aresample=44100,volume=4.0", "-c:v", "copy", output_path], capture_output=True, timeout=60)
        
        elif effect == "earrape":
            subprocess.run(["ffmpeg", "-y", "-i", clip_path, "-af", "volume=20.0", "-c:v", "copy", output_path], capture_output=True, timeout=60)
        
        elif effect == "reverse":
            subprocess.run(["ffmpeg", "-y", "-i", clip_path, "-vf", "reverse", "-af", "areverse", "-c:v", "libx264", "-preset", "fast", "-crf", "23", "-c:a", "aac", output_path], capture_output=True, timeout=60)
        
        elif effect == "fried":
            subprocess.run(["ffmpeg", "-y", "-i", clip_path, "-vf", "eq=contrast=3:saturation=6,noise=alls=80:allf=t+u", "-af", "volume=5.0", "-c:v", "libx264", "-preset", "fast", "-crf", "12", "-c:a", "aac", output_path], capture_output=True, timeout=60)
        
        if os.path.exists(output_path):
            results[effect] = output_path
            print(f"  {effect}: {os.path.getsize(output_path)//1024}K — {best['text'][:40]}")
    
    return results

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python ytp_transcribe.py <video_path> [prefix]")
        print("Uses whisper.cpp for transcription, then generates YTP effects.")
        sys.exit(1)
    
    video_path = sys.argv[1]
    prefix = sys.argv[2] if len(sys.argv) > 2 else "ytp"
    
    print(f"Transcribing: {os.path.basename(video_path)}")
    transcript = transcribe(video_path)
    
    if not transcript:
        print("Transcription failed!")
        sys.exit(1)
    
    print(f"Segments: {len(transcript['segments'])}")
    if transcript.get("text"):
        print(f"Text: {transcript['text'][:200]}...")
    
    # Save transcript
    transcript_path = video_path.replace(".mp4", "_transcript.json")
    with open(transcript_path, "w") as f:
        json.dump(transcript, f, indent=2)
    print(f"Saved: {transcript_path}")
    
    # Generate YTP effects
    print(f"\nGenerating YTP effects...")
    out_dir = os.path.dirname(video_path)
    results = make_ytp(video_path, transcript, out_dir, prefix)
    print(f"\nGenerated {len(results)} effects")
