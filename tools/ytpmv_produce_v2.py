#!/usr/bin/env python3
"""YTPMV Producer v2 — Better video-audio sync.
Usage: python3 tools/ytpmv_produce_v2.py <audio.wav> <video.mp4> <midi.mid> <output.mp4>
"""
import subprocess, sys, os, struct, wave, math

def run(cmd):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr

def parse_midi(path):
    import mido
    mid = mido.MidiFile(path)
    best_idx = 0; best_count = 0
    for i, track in enumerate(mid.tracks):
        notes = [m for m in track if m.type == 'note_on' and m.velocity > 0]
        if len(notes) > best_count:
            best_count = len(notes); best_idx = i
    events = []; abs_tick = 0
    tempo = 500000; ticks_per_beat = mid.ticks_per_beat
    note_ons = {}
    for msg in mid.tracks[best_idx]:
        abs_tick += msg.time
        if msg.type == 'set_tempo': tempo = msg.tempo
        if msg.type == 'note_on' and msg.velocity > 0:
            t = mido.tick2second(abs_tick, ticks_per_beat, tempo)
            note_ons[msg.note] = (t, msg.velocity)
            events.append({'start': t, 'dur': 0.2, 'note': msg.note, 'vel': msg.velocity})
        elif msg.type == 'note_off' or (msg.type == 'note_on' and msg.velocity == 0):
            if msg.note in note_ons:
                start, vel = note_ons.pop(msg.note)
                end_t = mido.tick2second(abs_tick, ticks_per_beat, tempo)
                for e in events:
                    if abs(e['start'] - start) < 0.001:
                        e['dur'] = max(0.08, end_t - start)
                        break
    return events

def find_all_vowels(path, sr=44100):
    """Find all stable vowel segments with good quality."""
    w = wave.open(path, 'r')
    n = w.getnframes(); raw = w.readframes(n); w.close()
    samples = struct.unpack(f'<{n}h', raw)
    ws = int(sr * 0.02); hop = ws // 2
    segments = []
    i = 0
    while i < n - ws * 10:
        energy = sum(s*s for s in samples[i:i+ws*10]) / (ws*10)
        zcr_list = []
        for j in range(10):
            window = samples[i+j*ws:i+(j+1)*ws]
            zcr = sum(1 for k in range(1, len(window)) if (window[k] >= 0) != (window[k-1] >= 0)) / ws
            zcr_list.append(zcr)
        avg_zcr = sum(zcr_list) / 10
        zcr_var = sum((z - avg_zcr)**2 for z in zcr_list) / 10
        
        if energy > 5e5 and 0.03 < avg_zcr < 0.35 and zcr_var < 0.001:
            pitch = sr * avg_zcr / 2.0
            if 80 < pitch < 1200:
                midi_note = int(69 + 12 * math.log2(pitch / 440) + 0.5)
                segments.append({
                    'start': i / sr,
                    'pitch': pitch,
                    'midi': midi_note,
                    'energy': energy
                })
            i += hop * 3  # Skip ahead
        else:
            i += hop
    return segments

def main():
    if len(sys.argv) < 5:
        print("Usage: ytpmv_produce_v2.py <audio.wav> <video.mp4> <midi.mid> <output.mp4> [--max-notes N]")
        sys.exit(1)
    audio_path, video_path, midi_path, output_path = sys.argv[1:5]
    max_notes = 64
    i = 5
    while i < len(sys.argv):
        if sys.argv[i] == '--max-notes' and i+1 < len(sys.argv): max_notes = int(sys.argv[i+1]); i += 2
        else: i += 1

    MIN_CLIP = 0.3; FADE = 0.005; XFADE = 0.02
    print(f"=== YTPMV Producer v2 ===")

    events = parse_midi(midi_path)[:max_notes]
    print(f"Parsed {len(events)} notes")

    # Find all vowel segments
    vowels = find_all_vowels(audio_path)
    print(f"Found {len(vowels)} vowel segments")
    
    if not vowels:
        print("ERROR: No vowel segments found!")
        sys.exit(1)

    # Group vowels by MIDI note for fast lookup
    vowel_by_midi = {}
    for v in vowels:
        midi = v['midi']
        if midi not in vowel_by_midi:
            vowel_by_midi[midi] = []
        vowel_by_midi[midi].append(v)

    # Get video duration
    dur_out = run(f'ffprobe -v error -show_entries format=duration -of csv=p=0 "{video_path}"')
    video_dur = float(dur_out[1].strip()) if dur_out[0] == 0 else 30.0

    # Process notes
    segments = []; total_dur = 0
    for count, evt in enumerate(events):
        note = evt['note']; start = evt['start']
        dur = max(MIN_CLIP, min(evt['dur'], 1.0))
        total_dur = max(total_dur, start + dur)

        # Find closest vowel by pitch
        best_vowel = None; best_dist = 999
        for v in vowels:
            dist = abs(v['midi'] - note)
            if dist < best_dist:
                best_dist = dist; best_vowel = v

        if best_vowel is None:
            best_vowel = vowels[0]

        # Pitch ratio
        note_freq = 440 * (2 ** ((note - 69) / 12))
        ratio = max(0.4, min(note_freq / best_vowel['pitch'], 2.5))

        # Extract audio from the best vowel position
        src_pos = best_vowel['start']
        fade_out = max(0, dur - FADE)
        cmd = (f'ffmpeg -y -v error -ss {src_pos} -t 0.3 -i "{audio_path}" '
               f'-af "rubberband=pitch={ratio:.4f}:formant=preserved,'
               f'afade=t=in:st=0:d={FADE},afade=t=out:st={fade_out:.4f}:d={FADE}" '
               f'-t {dur} /tmp/ytpmv2_a_{count}.wav')
        rc, _, _ = run(cmd)
        if rc != 0: continue

        # Video: use position proportional to pitch (high notes = later in video)
        # This creates a "singing" effect where the character's expression changes
        vid_pos = (count * 0.15) % max(1, video_dur - 0.5)
        pts = max(0.3, min(dur / 0.5, 3.0))
        cmd = (f'ffmpeg -y -v error -ss {vid_pos:.2f} -t 0.5 -i "{video_path}" '
               f'-vf "setpts={pts:.4f}*PTS" -an -c:v libx264 -preset fast -crf 28 /tmp/ytpmv2_v_{count}.mp4')
        rc, _, _ = run(cmd)
        if rc != 0: continue

        segments.append({'start': start, 'dur': dur, 'idx': count, 'note': note, 'ratio': ratio})
        print(f"  [{count}] t={start:.3f}s dur={dur:.3f}s note={note} ratio={ratio:.4f} src_t={src_pos:.2f}s")

    if not segments: print("ERROR: No segments!"); sys.exit(1)
    print(f"Built {len(segments)} segments")

    # Build video with crossfades
    print("Building video...")
    if len(segments) >= 2:
        vid_ins = ""; vid_flt = ""; offset = 0
        for i, seg in enumerate(segments):
            idx = seg['idx']; vid_ins += f" -i /tmp/ytpmv2_v_{idx}.mp4"
            if i == 0: offset = seg['dur'] - XFADE
            elif i == len(segments) - 1:
                vid_flt += f"[v{i-1}][{i}:v]xfade=transition=fade:duration={XFADE}:offset={offset:.4f}[vout]"
            else:
                vid_flt += f"[v{i-1}][{i}:v]xfade=transition=fade:duration={XFADE}:offset={offset:.4f}[v{i}];"
                offset += seg['dur'] - XFADE
        run(f'ffmpeg -y -v error{vid_ins} -filter_complex "{vid_flt}" -map "[vout]" /tmp/ytpmv2_video.mp4')
    else:
        run(f'cp /tmp/ytpmv2_v_0.mp4 /tmp/ytpmv2_video.mp4')

    # Mix audio
    print("Mixing audio...")
    aud_ins = ""; aud_flt = ""
    for seg in segments:
        idx = seg['idx']; ms = int(seg['start'] * 1000)
        aud_ins += f" -i /tmp/ytpmv2_a_{idx}.wav"
        aud_flt += f"[{idx}:a]adelay={ms}|{ms}[a{idx}];"
    mix_inputs = ''.join(f'[a{seg["idx"]}]' for seg in segments)
    aud_flt += f"{mix_inputs}amix=inputs={len(segments)}:duration=longest[aout]"
    run(f'ffmpeg -y -v error{aud_ins} -filter_complex "{aud_flt}" -map "[aout]" -acodec aac -t {int(total_dur+1)} /tmp/ytpmv2_audio.m4a')

    # Merge
    print("Merging...")
    run(f'ffmpeg -y -v error -i /tmp/ytpmv2_video.mp4 -i /tmp/ytpmv2_audio.m4a -c:v copy -c:a aac -shortest "{output_path}"')

    for f in os.listdir('/tmp'):
        if f.startswith('ytpmv2_'): os.remove(f'/tmp/{f}')

    if os.path.exists(output_path):
        size = os.path.getsize(output_path)
        dur = float(run(f'ffprobe -v error -show_entries format=duration -of csv=p=0 "{output_path}"')[1].strip())
        print(f"=== Done: {output_path} ({size/1024:.0f}KB, {dur:.1f}s) ===")
    else:
        print("ERROR!"); sys.exit(1)

if __name__ == '__main__':
    main()
