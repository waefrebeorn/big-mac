#!/usr/bin/env python3
"""YTPMV Producer v7 — Multi-root chromatic instrument.
Builds multiple chromatic instruments from samples at different pitches,
then for each MIDI note picks the closest root (minimal shift).
"""
import subprocess, sys, os, struct, wave, math
import mido

def run(cmd, **kwargs):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, **kwargs)
    return r.returncode, r.stdout, r.stderr

def detect_pitch(path):
    """Detect pitch of a WAV file using autocorrelation."""
    w = wave.open(path, 'r')
    n = w.getnframes(); sr = w.getframerate(); ch = w.getnchannels()
    raw = w.readframes(n); w.close()
    samples = struct.unpack(f'<{n * ch}', raw)
    
    # Mono
    mono = []
    for i in range(n):
        s = sum(samples[i*ch+c] for c in range(ch)) / ch / 32768.0
        mono.append(s)
    
    # Autocorrelation
    max_lag = min(int(sr/50), n//2)
    min_lag = max(int(sr/2000), 1)
    best_lag = 0; best_corr = -1
    
    for lag in range(min_lag, max_lag):
        corr = sum(mono[i]*mono[i+lag] for i in range(min(n-lag, 5000)))
        if corr > best_corr:
            best_corr = corr
            best_lag = lag
    
    return sr / best_lag if best_lag > 0 else 0

def extract_and_build_instruments(audio_path, output_dir):
    """Extract samples, detect pitches, build multi-root chromatic instrument."""
    os.makedirs(output_dir, exist_ok=True)
    
    # Read audio
    w = wave.open(audio_path, 'r')
    n_frames = w.getnframes(); sr = w.getframerate(); n_ch = w.getnchannels()
    raw = w.readframes(n_frames); w.close()
    samples = struct.unpack(f'<{n_frames * n_ch}h', raw)
    
    # Mono float
    mono = [sum(samples[i*n_ch+c] for c in range(n_ch))/n_ch/32768.0 for i in range(n_frames)]
    
    # Find onsets
    ws = int(sr*0.005); hop = ws//2
    energy = []
    for i in range(0, len(mono)-ws, hop):
        energy.append((i/sr, sum(mono[j]**2 for j in range(i, i+ws))/ws))
    
    onsets = []
    for i in range(2, len(energy)-2):
        if energy[i][1] > energy[i-1][1]*1.5 and energy[i][1] > 0.0003:
            t = energy[i][0]
            if not onsets or t - onsets[-1] > 0.05:
                onsets.append(t)
    
    print(f"Found {len(onsets)} onsets")
    
    # Extract samples with pitch detection
    sample_pitches = {}  # midi_note -> best sample path
    
    for idx, start in enumerate(onsets):
        dur = 0.2
        if idx + 1 < len(onsets):
            dur = min(onsets[idx+1] - start, 0.3)
        if dur < 0.03:
            continue
        
        start_frame = int(start * sr)
        end_frame = int((start + dur) * sr)
        
        # Detect pitch
        seg = mono[start_frame:end_frame]
        if len(seg) < 200:
            continue
        
        max_lag = min(int(sr/50), len(seg)//2)
        min_lag = max(int(sr/2000), 1)
        best_lag = 0; best_corr = -1
        for lag in range(min_lag, max_lag):
            corr = sum(seg[i]*seg[i+lag] for i in range(min(len(seg)-lag, 3000)))
            if corr > best_corr:
                best_corr = corr
                best_lag = lag
        
        freq = sr / best_lag if best_lag > 0 else 0
        if freq < 80 or freq > 1500:
            continue
        
        midi = int(69 + 12*math.log2(freq/440) + 0.5)
        
        # Save sample
        path = os.path.join(output_dir, f"root_{midi}_{idx}.wav")
        with wave.open(path, 'w') as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(sr)
            for j in range(start_frame, min(end_frame, n_frames)):
                s = max(-1, min(1, mono[j]))
                wf.writeframes(struct.pack('<h', int(s*32767)))
        
        # Keep best sample per MIDI note (highest energy)
        energy_val = sum(s**2 for s in seg) / len(seg)
        if midi not in sample_pitches or energy_val > sample_pitches[midi][1]:
            sample_pitches[midi] = (path, energy_val)
    
    print(f"Found {len(sample_pitches)} unique pitches")
    
    # Build chromatic scale from each root sample
    # For MIDI notes, use the closest root and shift minimally
    note_files = {}
    
    for target_midi in range(24, 108):
        # Find closest root
        best_root = None; best_dist = 999
        for root_midi in sample_pitches:
            dist = abs(root_midi - target_midi)
            if dist < best_dist:
                best_dist = dist
                best_root = root_midi
        
        if best_root is None or best_dist > 12:
            continue
        
        # Compute pitch shift
        target_freq = 440 * (2**((target_midi - 69)/12))
        root_freq = 440 * (2**((best_root - 69)/12))
        ratio = target_freq / root_freq
        
        path = sample_pitches[best_root][0]
        out_path = os.path.join(output_dir, f"note_{target_midi}.wav")
        
        run(f'ffmpeg -y -v error -i "{path}" -af "rubberband=pitch={ratio:.6f}:formant=preserved,loudnorm=I=-14:TP=-1.5:LRA=11" "{out_path}"')
        note_files[target_midi] = out_path
    
    print(f"Built {len(note_files)} note files")
    return note_files

def render(midi_path, audio_source, output_path, video_path=None, max_notes=500):
    print("=== YTPMV Producer v7 ===")
    
    # Build instrument
    instrument_dir = "/tmp/ytpmv7_inst"
    note_files = extract_and_build_instruments(audio_source, instrument_dir)
    
    if not note_files:
        print("ERROR: No notes!")
        return False
    
    available = sorted(note_files.keys())
    print(f"  Available notes: {available[0]}-{available[-1]}")
    
    # Parse MIDI
    mid = mido.MidiFile(midi_path)
    best_idx = 0; best_count = 0
    for i, track in enumerate(mid.tracks):
        notes = [m for m in track if m.type == 'note_on' and m.velocity > 0]
        if len(notes) > best_count:
            best_count = len(notes); best_idx = i
    
    events = []
    abs_tick = 0; tempo = 500000; note_ons = {}
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
    
    # Transpose to fit available range
    min_midi = min(e['note'] for e in events)
    max_midi = max(e['note'] for e in events)
    transpose = (available[0] + available[-1])//2 - (min_midi + max_midi)//2
    if min_midi + transpose < available[0]: transpose = available[0] - min_midi
    if max_midi + transpose > available[-1]: transpose = available[-1] - max_midi
    print(f"  Transpose: {transpose}")
    
    # Render audio using sox for speed (or ffmpeg with concat)
    print("Rendering audio...")
    
    # Create a single audio file per note placement using sox
    total_dur = max(e['start'] + e['dur'] for e in events) + 1.0
    sample_rate = 44100
    
    # Use sox to create silence, then mix in each note
    # But sox might not be available — use ffmpeg with adelay
    
    # Build note files list
    os.makedirs("/tmp/ytpmv7_mix", exist_ok=True)
    placed_files = []
    
    for i, evt in enumerate(events):
        note = evt['note'] + transpose
        # Find closest available note
        closest = min(available, key=lambda x: abs(x - note))
        
        if closest not in note_files:
            continue
        
        src = note_files[closest]
        dst = f"/tmp/ytpmv7_mix/note_{i}.wav"
        
        # Copy with fade and trim
        dur = evt['dur']
        fade_out = max(0, dur - 0.003)
        rc, _, _ = run(f'ffmpeg -y -v error -i "{src}" -af "afade=t=in:st=0:d=0.002,afade=t=out:st={fade_out:.4f}:d=0.002" -t {dur+0.01} "{dst}"')
        if rc == 0:
            placed_files.append((i, evt['start'], dst))
    
    print(f"  {len(placed_files)} notes placed")
    
    # Mix with batched adelay+amix
    print("Mixing...")
    BATCH = 32
    batch_files = []
    
    for batch_idx, batch_start in enumerate(range(0, len(placed_files), BATCH)):
        batch_end = min(batch_start + BATCH, len(placed_files))
        batch = placed_files[batch_start:batch_end]
        
        aud_ins = ""; aud_flt = ""
        for seg_idx, (idx, start, path) in enumerate(batch):
            ms = int(start * 1000)
            aud_ins += f' -i "{path}"'
            aud_flt += f"[{seg_idx}:a]adelay={ms}|{ms}[b{seg_idx}];"
        
        mix_inputs = ''.join(f'[b{seg_idx}]' for seg_idx in range(len(batch)))
        label = f"mix{batch_idx}"
        aud_flt += f"{mix_inputs}amix=inputs={len(batch)}:duration=longest[{label}]"
        
        rc, _, err = run(f'ffmpeg -y -v error{aud_ins} -filter_complex "{aud_flt}" -map "[{label}]" /tmp/ytpmv7_{label}.m4a')
        if rc == 0:
            batch_files.append(f"/tmp/ytpmv7_{label}.m4a")
    
    # Combine
    if len(batch_files) > 1:
        final_ins = "".join(f" -i {f}" for f in batch_files)
        final_flt = ''.join(f'[{i}:a]' for i in range(len(batch_files)))
        final_flt += f"amix=inputs={len(batch_files)}:duration=longest,loudnorm=I=-14:TP=-1.5:LRA=11[aout]"
        run(f'ffmpeg -y -v error{final_ins} -filter_complex "{final_flt}" -map "[aout]" -acodec aac -t {int(total_dur+2)} /tmp/ytpmv7_audio.m4a')
    elif len(batch_files) == 1:
        run(f'ffmpeg -y -v error -i {batch_files[0]} -af "loudnorm=I=-14:TP=-1.5:LRA=11" -acodec aac /tmp/ytpmv7_audio.m4a')
    
    # Merge video
    if video_path and os.path.exists(video_path):
        r = run('ffprobe -v error -show_entries format=duration -of csv=p=0 /tmp/ytpmv7_audio.m4a')
        aud_dur = float(r[1].strip())
        run(f'ffmpeg -y -v error -stream_loop 100 -i "{video_path}" -t {int(aud_dur+1)} -an -c:v libx264 -preset fast -crf 28 /tmp/ytpmv7_video.mp4')
        run(f'ffmpeg -y -i /tmp/ytpmv7_video.mp4 -i /tmp/ytpmv7_audio.m4a -c:v libx264 -preset fast -crf 23 -c:a aac -b:a 192k -shortest "{output_path}"')
    else:
        run(f'cp /tmp/ytpmv7_audio.m4a "{output_path}"')
    
    # Check
    if os.path.exists(output_path):
        size = os.path.getsize(output_path)
        r = run(f'ffprobe -v error -show_entries format=duration -of csv=p=0 "{output_path}"')
        print(f"=== Done: {output_path} ({size/1024:.0f}KB, {r[1].strip()}s) ===")
    
    # Cleanup
    import shutil
    for d in ['/tmp/ytpmv7_inst', '/tmp/ytpmv7_mix']:
        if os.path.exists(d): shutil.rmtree(d)
    for f in os.listdir('/tmp'):
        if f.startswith('ytpmv7_'):
            try: os.remove(f'/tmp/{f}')
            except: pass
    
    return True

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print("Usage: ytpmv_produce_v7.py <midi.mid> <audio.wav> <output.mp4> [video.mp4] [--max-notes N]")
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
    
    success = render(midi_path, audio_source, output_path, video_path, max_notes)
    sys.exit(0 if success else 1)
