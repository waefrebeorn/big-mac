#!/usr/bin/env python3
"""Transcribe a video source and build a searchable index of content.
Uses ffmpeg for audio extraction and whisper for STT.
"""
import subprocess, os, json, sys

def extract_audio(video_path, output_wav):
    """Extract audio from video as 16kHz mono WAV."""
    cmd = [
        "ffmpeg", "-y", "-i", video_path,
        "-vn", "-acodec", "pcm_s16le", "-ar", "16000", "-ac", "1",
        output_wav
    ]
    r = subprocess.run(cmd, capture_output=True, timeout=120)
    return r.returncode == 0 and os.path.exists(output_wav)

def transcribe(audio_path):
    """Transcribe audio using whisper CLI."""
    # Try whisper.cpp first (faster), then fall back to whisper
    whisper_cmd = None
    
    # Check for whisper.cpp
    if os.path.exists("/opt/homebrew/bin/whisper") or subprocess.run(["which", "whisper"], capture_output=True).returncode == 0:
        whisper_cmd = ["whisper", audio_path, "--model", "base", "--output_format", "json", "--output_dir", "/tmp/whisper_out"]
    elif os.path.exists("/opt/homebrew/bin/whisper-cpp") or os.path.exists(os.path.expanduser("~/whisper.cpp/whisper")):
        # whisper.cpp path
        whisper_bin = os.path.expanduser("~/whisper.cpp/whisper")
        model = os.path.expanduser("~/whisper.cpp/models/ggml-base.bin")
        if os.path.exists(whisper_bin) and os.path.exists(model):
            whisper_cmd = [whisper_bin, "-m", model, "-f", audio_path, "-of", "/tmp/whisper_out", "--output-json"]
    
    if not whisper_cmd:
        # Fall back to Python whisper
        try:
            import whisper
            model = whisper.load_model("base")
            result = model.transcribe(audio_path, word_timestamps=True)
            return result
        except ImportError:
            print("No whisper available. Install with: pip install openai-whisper")
            return None
    
    # Run whisper CLI
    r = subprocess.run(whisper_cmd, capture_output=True, text=True, timeout=600)
    
    # Read output
    json_path = "/tmp/whisper_out/" + os.path.basename(audio_path).replace(".wav", ".json")
    if os.path.exists(json_path):
        with open(json_path) as f:
            return json.load(f)
    
    return None

def build_index(video_path):
    """Build a searchable transcript index for a video."""
    print(f"Processing: {os.path.basename(video_path)}")
    
    # Extract audio
    audio_path = "/tmp/transcribe_audio.wav"
    if not extract_audio(video_path, audio_path):
        print("  Failed to extract audio")
        return None
    
    # Get duration
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "default=noprint_wrappers=1:nokey=1", video_path],
        capture_output=True, text=True, timeout=10
    )
    duration = float(probe.stdout.strip())
    print(f"  Duration: {duration:.1f}s")
    
    # Transcribe
    result = transcribe(audio_path)
    if not result:
        print("  Transcription failed")
        return None
    
    # Build index
    index = {
        "source": os.path.basename(video_path),
        "duration": duration,
        "segments": []
    }
    
    if "segments" in result:
        for seg in result["segments"]:
            index["segments"].append({
                "start": seg.get("start", 0),
                "end": seg.get("end", 0),
                "text": seg.get("text", "").strip(),
                "words": [{"word": w.get("word", ""), "start": w.get("start", 0), "end": w.get("end", 0)} 
                         for w in seg.get("words", [])] if "words" in seg else []
            })
    
    # Clean up
    if os.path.exists(audio_path):
        os.remove(audio_path)
    
    return index

def search_index(index, query):
    """Search the transcript index for specific words or phrases."""
    results = []
    query_lower = query.lower()
    for seg in index.get("segments", []):
        if query_lower in seg["text"].lower():
            results.append(seg)
    return results

def find_silence_gaps(index, min_duration=0.5):
    """Find gaps between speech segments (natural cut points)."""
    gaps = []
    segments = index.get("segments", [])
    for i in range(len(segments) - 1):
        gap_start = segments[i]["end"]
        gap_end = segments[i + 1]["start"]
        gap_duration = gap_end - gap_start
        if gap_duration >= min_duration:
            gaps.append({
                "start": gap_start,
                "end": gap_end,
                "duration": gap_duration,
                "before_text": segments[i]["text"],
                "after_text": segments[i + 1]["text"]
            })
    return gaps

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python transcribe.py <video_path> [search_query]")
        sys.exit(1)
    
    video_path = sys.argv[1]
    search_query = sys.argv[2] if len(sys.argv) > 2 else None
    
    index = build_index(video_path)
    if index:
        # Save index
        index_path = video_path.replace(".mp4", "_transcript.json")
        with open(index_path, "w") as f:
            json.dump(index, f, indent=2)
        print(f"\nIndex saved to: {index_path}")
        print(f"Segments: {len(index['segments'])}")
        
        if search_query:
            results = search_index(index, search_query)
            print(f"\nSearch '{search_query}': {len(results)} results")
            for r in results[:5]:
                print(f"  [{r['start']:.1f}-{r['end']:.1f}] {r['text'][:80]}")
        
        # Show silence gaps
        gaps = find_silence_gaps(index)
        print(f"\nSilence gaps (>0.5s): {len(gaps)}")
        for g in gaps[:5]:
            print(f"  [{g['start']:.1f}-{g['end']:.1f}] ({g['duration']:.1f}s)")
