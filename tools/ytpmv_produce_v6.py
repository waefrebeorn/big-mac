#!/usr/bin/env python3
"""YTPMV Producer v6 — Correct technique: multi-sample pitch matching.

The RIGHT way to make YTPMV:
1. Extract MANY short samples from the character (every distinct sound/syllable)
2. Detect the pitch of each sample (using ZCR + autocorrelation)
3. For each MIDI note, pick the CLOSEST sample (minimal pitch shift)
4. Apply small pitch correction to match exactly
5. Place on timeline

This preserves the character's voice because:
- Most notes use a sample already close to that pitch
- Pitch shifts are minimal (1-3 semitones instead of 12+)
- Different syllables add variety (not the same sound repeated)
"""
import subprocess, sys, os, struct, wave, math
import mido

def run(cmd, **kwargs):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, **kwargs)
    return r.returncode, r.stdout, r.stderr

def detect_pitch(audio, n_frames, n_channels, sr):
    """Detect fundamental frequency using autocorrelation."""
    # Mix to mono
    mono = []
    for i in range(n_frames):
        s = 0
        for c in range(n_channels):
            s += audio[i * n_channels + c]
        mono.append(s / n_channels)
    
    # Autocorrelation pitch detection
    max_lag = min(int(sr / 50), n_frames // 2)  # Lowest 50Hz
    min_lag = max(int(sr / 2000), 1)  # Highest 2000Hz
    
    best_lag = 0
    best_corr = -1
    
    for lag in range(min_lag, max_lag):
        corr = 0
        count = 0
        for i in range(n_frames - lag):
            corr += mono[i] * mono[i + lag]
            count += 1
        if count > 0:
            corr /= count
        
        if corr > best_corr:
            best_corr = corr
            best_lag = lag
    
    if best_lag > 0:
        freq = sr / best_lag
        return freq
    return 0

def extract_samples(audio_path, output_dir, min_dur=0.05, max_dur=0.3):
    """Extract individual sound samples from audio file.
    Uses onset detection to find sound boundaries."""
    os.makedirs(output_dir, exist_ok=True)
    
    # Read audio
    w = wave.open(audio_path, 'r')
    n = w.getnframes(); sr = w.getframerate()
    n_channels = w.getnchannels()
    raw = w.readframes(n)
    w.close()
    
    samples = struct.unpack(f'<{n}h', raw)
    
    # Convert to mono float
    mono = []
    for i in range(n):
        s = 0
        for c in range(n_channels):
            s += samples[i * n_channels + c]
        mono.append(s / n_channels / 32768.0)
    
    # Find onsets using energy flux
    ws = int(sr * 0.005)  # 5ms windows for finer resolution
    hop = ws // 2
    
    energy = []
    for i in range(0, len(mono) - ws, hop):
        e = sum(mono[j]**2 for j in range(i, i+ws)) / ws
        energy.append((i / sr, e))
    
    # Find peaks in energy flux (onsets)
    onsets = []
    threshold = 0.0005  # Lower threshold for quiet sounds
    
    for i in range(2, len(energy) - 2):
        prev_e = energy[i-1][1]
        curr_e = energy[i][1]
        next_e = energy[i+1][1]
        
        # Onset: energy increases significantly
        if curr_e > prev_e * 1.5 and curr_e > threshold:
            t = energy[i][0]
            # Minimum 50ms between onsets (shorter for quick sounds)
            if not onsets or t - onsets[-1] > 0.05:
                onsets.append(t)
    
    print(f"Found {len(onsets)} onsets")
    
    # Extract samples between onsets
    sample_list = []
    for i, start in enumerate(onsets):
        end = onsets[i + 1] if i + 1 < len(onsets) else start + max_dur
        dur = end - start
        
        if dur < min_dur:
            continue
        if dur > max_dur:
            dur = max_dur
        
        start_frame = int(start * sr)
        end_frame = int((start + dur) * sr)
        
        # Detect pitch
        audio_segment = []
        for j in range(start_frame, min(end_frame, n)):
            for c in range(n_channels):
                audio_segment.append(float(samples[j * n_channels + c]) / 32768.0)
        
        seg_frames = len(audio_segment) // n_channels
        if seg_frames < 100:
            continue
        
        freq = detect_pitch(audio_segment, seg_frames, n_channels, sr)
        if freq < 50 or freq > 2000:
            continue
        
        midi_note = int(69 + 12 * math.log2(freq / 440) + 0.5)
        
        # Save sample
        sample_path = os.path.join(output_dir, f"sample_{i:03d}_midi{midi_note}.wav")
        save_wav(audio_segment, seg_frames, n_channels, sr, sample_path)
        
        sample_list.append({
            'path': sample_path,
            'midi': midi_note,
            'freq': freq,
            'start': start,
            'dur': dur
        })
    
    print(f"Extracted {len(sample_list)} pitched samples")
    return sample_list

def save_wav(audio, n_frames, n_channels, sr, path):
    """Save float audio as 16-bit WAV."""
    with wave.open(path, 'w') as w:
        w.setnchannels(n_channels)
        w.setsampwidth(2)
        w.setframerate(sr)
        for i in range(n_frames * n_channels):
            s = max(-1, min(1, audio[i]))
            w.writeframes(struct.pack('<h', int(s * 32767)))

def find_closest_sample(sample_list, target_midi):
    """Find the sample with MIDI note closest to target."""
    best = None
    best_dist = 999
    for s in sample_list:
        dist = abs(s['midi'] - target_midi)
        if dist < best_dist:
            best_dist = dist
            best = s
    return best, best_dist

def render_ytpmv(midi_path, audio_source, output_path, video_path=None, max_notes=500):
    """Produce YTPMV with multi-sample pitch matching."""
    print(f"=== YTPMV Producer v6 (Multi-Sample) ===")
    
    # Step 1: Extract samples from character audio
    sample_dir = "/tmp/ytpmv6_samples"
    print("Extracting samples from character audio...")
    samples = extract_samples(audio_source, sample_dir)
    
    if not samples:
        print("ERROR: No samples extracted!")
        return False
    
    # Show sample distribution
    midi_notes = [s['midi'] for s in samples]
    print(f"  Sample MIDI range: {min(midi_notes)}-{max(midi_notes)}")
    print(f"  Unique pitches: {len(set(midi_notes))}")
    
    # Step 2: Parse MIDI
    print("Parsing MIDI...")
    mid = mido.MidiFile(midi_path)
    
    # Find best track
    best_idx = 0; best_count = 0
    for i, track in enumerate(mid.tracks):
        notes = [m for m in track if m.type == 'note_on' and m.velocity > 0]
        if len(notes) > best_count:
            best_count = len(notes); best_idx = i
    
    # Extract events
    events = []
    abs_tick = 0; tempo = 500000
    note_ons = {}
    
    for msg in mid.tracks[best_idx]:
        abs_tick += msg.time
        if msg.type == 'set_tempo': tempo = msg.tempo
        elif msg.type == 'note_on' and msg.velocity > 0:
            t = mido.tick2second(abs_tick, mid.ticks_per_beat, tempo)
            note_ons[msg.note] = (t, msg.velocity)
        elif msg.type == 'note_off' or (msg.type == 'note_on' and msg.velocity == 0):
            if msg.note in note_ons:
                start, vel = note_ons.pop(msg.note)
                t = mido.tick2second(abs_tick, mid.ticks_per_beat, tempo)
                events.append({'start': start, 'dur': max(0.05, t-start), 'note': msg.note, 'vel': vel})
    
    events = events[:max_notes]
    print(f"  {len(events)} MIDI events")
    
    # Step 3: For each MIDI note, find closest sample and compute pitch shift
    print("Matching samples to notes...")
    note_files = []
    
    for i, evt in enumerate(events):
        target_midi = evt['note']
        sample, dist = find_closest_sample(samples, target_midi)
        
        if sample is None:
            continue
        
        # Compute pitch ratio
        target_freq = 440 * (2 ** ((target_midi - 69) / 12))
        ratio = target_freq / sample['freq']
        
        # Limit pitch shift to ±12 semitones
        if ratio > 2.0: ratio = 2.0
        if ratio < 0.5: ratio = 0.5
        
        # Apply pitch shift with formant preservation
        out_file = f"/tmp/ytpmv6_note_{i}.wav"
        dur = evt['dur']
        fade_out = max(0, dur - 0.003)
        
        cmd = (f'ffmpeg -y -v error -i "{sample["path"]}" '
               f'-af "rubberband=pitch={ratio:.6f}:formant=preserved,'
               f'afade=t=in:st=0:d=0.002,afade=t=out:st={fade_out:.4f}:d=0.002" '
               f'-t {dur + 0.005} "{out_file}"')
        rc, _, _ = run(cmd)
        if rc == 0:
            note_files.append((i, evt['start'], out_file))
        
        if i % 100 == 0:
            print(f"  {i}/{len(events)} (dist={dist}st)")
    
    print(f"Rendered {len(note_files)} notes")
    
    # Step 4: Mix audio with adelay
    print("Mixing audio...")
    BATCH = 24
    batch_files = []
    
    for batch_idx, batch_start in enumerate(range(0, len(note_files), BATCH)):
        batch_end = min(batch_start + BATCH, len(note_files))
        batch = note_files[batch_start:batch_end]
        
        aud_ins = ""; aud_flt = ""
        for seg_idx, (idx, start, path) in enumerate(batch):
            ms = int(start * 1000)
            aud_ins += f' -i "{path}"'
            aud_flt += f'[{seg_idx}:a]adelay={ms}|{ms}[b{seg_idx}];'
        
        mix_inputs = ''.join(f'[b{seg_idx}]' for seg_idx in range(len(batch)))
        batch_label = f"mix{batch_idx}"
        aud_flt += f"{mix_inputs}amix=inputs={len(batch)}:duration=longest[{batch_label}]"
        
        rc, _, err = run(f'ffmpeg -y -v error{aud_ins} -filter_complex "{aud_flt}" -map "[{batch_label}]" /tmp/ytpmv6_{batch_label}.m4a')
        if rc == 0:
            batch_files.append(f"/tmp/ytpmv6_{batch_label}.m4a")
    
    # Combine batches
    if len(batch_files) > 1:
        final_ins = "".join(f" -i {f}" for f in batch_files)
        final_flt = ''.join(f'[{i}:a]' for i in range(len(batch_files)))
        final_flt += f"amix=inputs={len(batch_files)}:duration=longest,loudnorm=I=-14:TP=-1.5:LRA=11[aout]"
        total_dur = max(s + 1.0 for _, s, _ in note_files)
        run(f'ffmpeg -y -v error{final_ins} -filter_complex "{final_flt}" -map "[aout]" -acodec aac -t {int(total_dur+2)} /tmp/ytpmv6_audio.m4a')
    elif len(batch_files) == 1:
        run(f'ffmpeg -y -v error -i {batch_files[0]} -af "loudnorm=I=-14:TP=-1.5:LRA=11" -acodec aac /tmp/ytpmv6_audio.m4a')
    
    # Check audio
    r = run('ffprobe -v error -show_entries format=duration -of csv=p=0 /tmp/ytpmv6_audio.m4a')
    print(f"  Audio: {r[1].strip()}s")
    
    # Merge with video
    if video_path and os.path.exists(video_path):
        print("Merging video...")
        r = run('ffprobe -v error -show_entries format=duration -of csv=p=0 /tmp/ytpmv6_audio.m4a')
        aud_dur = float(r[1].strip())
        run(f'ffmpeg -y -v error -stream_loop 100 -i "{video_path}" -t {int(aud_dur+1)} -an -c:v libx264 -preset fast -crf 28 /tmp/ytpmv6_video.mp4')
        run(f'ffmpeg -y -i /tmp/ytpmv6_video.mp4 -i /tmp/ytpmv6_audio.m4a -c:v libx264 -preset fast -crf 23 -c:a aac -b:a 192k -shortest "{output_path}"')
    else:
        run(f'cp /tmp/ytpmv6_audio.m4a "{output_path}"')
    
    # Check output
    if os.path.exists(output_path):
        size = os.path.getsize(output_path)
        r = run(f'ffprobe -v error -show_entries format=duration -of csv=p=0 "{output_path}"')
        print(f"=== Done: {output_path} ({size/1024:.0f}KB, {r[1].strip()}s) ===")
    
    # Cleanup
    for f in os.listdir('/tmp'):
        if f.startswith('ytpmv6_'):
            try: os.remove(f'/tmp/{f}')
            except: pass
    import shutil
    if os.path.exists(sample_dir):
        shutil.rmtree(sample_dir)
    
    return True

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print("Usage: ytpmv_produce_v6.py <midi.mid> <character_audio.wav> <output.mp4> [video.mp4] [--max-notes N]")
        sys.exit(1)
    
    midi_path = sys.argv[1]
    audio_source = sys.argv[2]
    output_path = sys.argv[3]
    video_path = None
    max_notes = 500
    
    i = 4
    while i < len(sys.argv):
        if sys.argv[i] == '--max-notes' and i+1 < len(sys.argv): max_notes = int(sys.argv[i+1]); i += 2
        elif not sys.argv[i].startswith('--') and os.path.exists(sys.argv[i]): video_path = sys.argv[i]; i += 1
        else: i += 1
    
    success = render_ytpmv(midi_path, audio_source, output_path, video_path, max_notes)
    sys.exit(0 if success else 1)
