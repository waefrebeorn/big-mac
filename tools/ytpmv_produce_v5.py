#!/usr/bin/env python3
"""YTPMV Producer v5 — The correct way.
1. Parse MIDI with mido
2. Build chromatic instrument from best vowel sample
3. Render each note at the correct pitch with rubberband
4. Place notes on timeline with sox/ffmpeg
5. Normalize and output
"""
import subprocess, sys, os, struct, wave, math
import mido

def run(cmd, **kwargs):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, **kwargs)
    return r.returncode, r.stdout, r.stderr

def find_best_vowel(path, sr=44100):
    """Find the best sustained vowel in an audio file."""
    w = wave.open(path, 'r')
    n = w.getnframes(); raw = w.readframes(n); w.close()
    samples = struct.unpack(f'<{n}h', raw)
    
    ws = int(sr * 0.01); hop = ws // 2
    best = None; best_score = 0
    
    for i in range(0, n - ws * 20, hop):
        energy_total = 0; zcr_list = []
        for j in range(20):
            window = samples[i+j*ws:i+(j+1)*ws]
            energy = sum(s*s for s in window) / ws
            zcr = sum(1 for k in range(1, len(window)) if (window[k] >= 0) != (window[k-1] >= 0)) / ws
            energy_total += energy; zcr_list.append(zcr)
        
        avg_energy = energy_total / 20
        avg_zcr = sum(zcr_list) / 20
        zcr_var = sum((z - avg_zcr)**2 for z in zcr_list) / 20
        
        if avg_energy > 5e5 and 0.03 < avg_zcr < 0.4 and zcr_var < 0.001:
            pitch = sr * avg_zcr / 2.0
            if 100 < pitch < 2000:
                score = avg_energy / (1 + zcr_var * 10000)
                if score > best_score:
                    best_score = score
                    best = {'start': i/sr, 'pitch': pitch, 'midi': int(69 + 12*math.log2(pitch/440)+0.5)}
    
    return best

def build_chromatic_instrument(sample_path, output_dir, root_midi, sr=44100):
    """Build a chromatic scale from a single sample using rubberband."""
    os.makedirs(output_dir, exist_ok=True)
    
    # Extract 200ms of the best vowel position
    vowel = find_best_vowel(sample_path)
    if not vowel:
        print("ERROR: No vowel found!")
        return None
    
    start = vowel['start']
    print(f"Best vowel: t={start:.3f}s pitch={vowel['pitch']:.1f}Hz midi={vowel['midi']}")
    
    # Extract the sample
    base_wav = os.path.join(output_dir, "base.wav")
    run(f'ffmpeg -y -v error -ss {start:.3f} -t 0.2 -i "{sample_path}" -acodec pcm_s16le -ar {sr} -ac 1 "{base_wav}"')
    
    # Build chromatic scale from MIDI 48 to 96
    src_freq = vowel['pitch']
    for midi in range(48, 97):
        target_freq = 440 * (2 ** ((midi - 69) / 12))
        ratio = target_freq / src_freq
        # Clamp ratio to avoid extreme shifts
        ratio = max(0.25, min(ratio, 4.0))
        
        out_path = os.path.join(output_dir, f"note_{midi}.wav")
        run(f'ffmpeg -y -v error -i "{base_wav}" -af "rubberband=pitch={ratio:.6f}:formant=preserved,loudnorm=I=-14:TP=-1.5:LRA=11" "{out_path}"')
    
    print(f"Built chromatic instrument: MIDI 48-96 ({src_freq:.1f}Hz root)")
    return vowel

def parse_midi(path, max_notes=2000):
    """Parse MIDI file and extract note events."""
    mid = mido.MidiFile(path)
    
    # Find best track
    best_idx = 0; best_count = 0
    for i, track in enumerate(mid.tracks):
        notes = [m for m in track if m.type == 'note_on' and m.velocity > 0]
        if len(notes) > best_count:
            best_count = len(notes); best_idx = i
    
    # Extract events
    events = []
    abs_tick = 0
    tempo = 500000
    note_ons = {}
    
    for msg in mid.tracks[best_idx]:
        abs_tick += msg.time
        if msg.type == 'set_tempo':
            tempo = msg.tempo
        elif msg.type == 'note_on' and msg.velocity > 0:
            t = mido.tick2second(abs_tick, mid.ticks_per_beat, tempo)
            note_ons[msg.note] = (t, msg.velocity)
        elif msg.type == 'note_off' or (msg.type == 'note_on' and msg.velocity == 0):
            if msg.note in note_ons:
                start, vel = note_ons.pop(msg.note)
                t = mido.tick2second(abs_tick, mid.ticks_per_beat, tempo)
                events.append({
                    'start': start,
                    'dur': max(0.05, t - start),
                    'note': msg.note,
                    'vel': vel
                })
    
    return events[:max_notes]

def render_ytpmv(midi_path, sample_path, output_path, video_path=None, max_notes=500):
    """Produce a YTPMV."""
    print(f"=== YTPMV Producer v5 ===")
    
    # Build instrument
    instrument_dir = "/tmp/ytpmv5_instrument"
    vowel = build_chromatic_instrument(sample_path, instrument_dir, 0)
    if not vowel:
        return False
    
    # Parse MIDI
    events = parse_midi(midi_path, max_notes)
    if not events:
        print("ERROR: No MIDI events!")
        return False
    
    print(f"Parsed {len(events)} MIDI events")
    
    # Calculate note range and transpose
    min_note = min(e['note'] for e in events)
    max_note = max(e['note'] for e in events)
    print(f"MIDI range: {min_note}-{max_note}")
    
    # Transpose to fit instrument range (48-96)
    center = (min_note + max_note) // 2
    target_center = 72  # Middle of instrument range
    transpose = target_center - center
    # Clamp so all notes fit in 48-96
    if min_note + transpose < 48: transpose = 48 - min_note
    if max_note + transpose > 96: transpose = 96 - max_note
    print(f"Transpose: {transpose} semitones")
    
    # Render each note
    print("Rendering notes...")
    total_dur = max(e['start'] + e['dur'] for e in events) + 1.0
    
    os.makedirs("/tmp/ytpmv5_notes", exist_ok=True)
    
    for i, evt in enumerate(events):
        note = evt['note'] + transpose
        note = max(48, min(note, 96))
        
        note_file = f"{instrument_dir}/note_{note}.wav"
        out_file = f"/tmp/ytpmv5_notes/note_{i}.wav"
        
        # Copy note file with fade envelope
        dur = evt['dur']
        fade_out = max(0, dur - 0.005)
        rc, _, err = run(f'ffmpeg -y -i "{note_file}" -af "afade=t=in:st=0:d=0.003,afade=t=out:st={fade_out:.4f}:d=0.003" -t {dur + 0.05} "{out_file}"')
        if rc != 0:
            print(f"  Note {i} failed: {err[:100]}")
            continue
        
        if i % 50 == 0:
            print(f"  {i}/{len(events)}")
    
    print(f"Rendered {len(events)} notes")
    
    # Mix audio using batched amix
    print("Mixing audio...")
    BATCH = 24
    batch_files = []
    
    for batch_idx, batch_start in enumerate(range(0, len(events), BATCH)):
        batch_end = min(batch_start + BATCH, len(events))
        batch = events[batch_start:batch_end]
        
        aud_ins = ""; aud_flt = ""
        for seg_idx, evt in enumerate(batch):
            ms = int(evt['start'] * 1000)
            aud_ins += f" -i /tmp/ytpmv5_notes/note_{batch_start + seg_idx}.wav"
            aud_flt += f"[{seg_idx}:a]adelay={ms}|{ms}[b{seg_idx}];"
        
        mix_inputs = ''.join(f'[b{seg_idx}]' for seg_idx in range(len(batch)))
        batch_label = f"mix{batch_idx}"
        aud_flt += f"{mix_inputs}amix=inputs={len(batch)}:duration=longest[{batch_label}]"
        
        rc, _, err = run(f'ffmpeg -y -v error{aud_ins} -filter_complex "{aud_flt}" -map "[{batch_label}]" /tmp/ytpmv5_{batch_label}.m4a')
        if rc != 0:
            print(f"  Batch {batch_idx} failed: {err[:100]}")
            continue
        batch_files.append(f"/tmp/ytpmv5_{batch_label}.m4a")
    
    print(f"  {len(batch_files)} batches")
    
    # Combine batches
    if len(batch_files) > 1:
        final_ins = "".join(f" -i {f}" for f in batch_files)
        final_flt = ''.join(f'[{i}:a]' for i in range(len(batch_files)))
        final_flt += f"amix=inputs={len(batch_files)}:duration=longest,loudnorm=I=-14:TP=-1.5:LRA=11[aout]"
        run(f'ffmpeg -y -v error{final_ins} -filter_complex "{final_flt}" -map "[aout]" -acodec aac -t {int(total_dur+2)} /tmp/ytpmv5_audio.m4a')
    elif len(batch_files) == 1:
        run(f'ffmpeg -y -v error -i {batch_files[0]} -af "loudnorm=I=-14:TP=-1.5:LRA=11" -acodec aac -t {int(total_dur+2)} /tmp/ytpmv5_audio.m4a')
    
    # Check audio
    r = run('ffprobe -v error -show_entries format=duration -of csv=p=0 /tmp/ytpmv5_audio.m4a')
    print(f"  Audio: {r[1].strip()}s")
    
    # Merge with video if provided
    if video_path and os.path.exists(video_path):
        print("Merging video...")
        # Loop video to match audio duration
        r = run(f'ffmpeg -y -v error -i "{video_path}" -af "volumedetect" -f null /dev/null 2>&1')
        
        # Create video loop
        run(f'ffmpeg -y -v error -stream_loop 100 -i "{video_path}" -t {int(total_dur+1)} -an -c:v libx264 -preset fast -crf 28 /tmp/ytpmv5_video.mp4')
        
        # Merge
        rc, _, err = run(f'ffmpeg -y -i /tmp/ytpmv5_video.mp4 -i /tmp/ytpmv5_audio.m4a -c:v libx264 -preset fast -crf 23 -c:a aac -b:a 192k -shortest "{output_path}"')
        if rc != 0:
            print(f"  Merge failed: {err[:200]}")
    else:
        # Just copy audio
        run(f'cp /tmp/ytpmv5_audio.m4a "{output_path}"')
    
    # Check output
    if os.path.exists(output_path):
        size = os.path.getsize(output_path)
        r = run(f'ffprobe -v error -show_entries format=duration -of csv=p=0 "{output_path}"')
        print(f"=== Done: {output_path} ({size/1024:.0f}KB, {r[1].strip()}s) ===")
    
    # Cleanup
    for d in ['/tmp/ytpmv5_notes', '/tmp/ytpmv5_instrument']:
        if os.path.exists(d):
            for f in os.listdir(d):
                os.remove(os.path.join(d, f))
            os.rmdir(d)
    for f in os.listdir('/tmp'):
        if f.startswith('ytpmv5_'):
            try: os.remove(f'/tmp/{f}')
            except: pass
    
    return True

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print("Usage: ytpmv_produce_v5.py <midi.mid> <sample.wav> <output.mp4> [video.mp4] [--max-notes N]")
        sys.exit(1)
    
    midi_path = sys.argv[1]
    sample_path = sys.argv[2]
    output_path = sys.argv[3]
    video_path = None
    max_notes = 500
    
    i = 4
    while i < len(sys.argv):
        if sys.argv[i] == '--max-notes' and i+1 < len(sys.argv): max_notes = int(sys.argv[i+1]); i += 2
        elif not sys.argv[i].startswith('--') and os.path.exists(sys.argv[i]): video_path = sys.argv[i]; i += 1
        else: i += 1
    
    success = render_ytpmv(midi_path, sample_path, output_path, video_path, max_notes)
    sys.exit(0 if success else 1)
