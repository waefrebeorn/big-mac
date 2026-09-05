#!/usr/bin/env python3
"""YTPMV Production Script (R131) — Complete pipeline.
Usage: python3 tools/ytpmv_produce.py <audio.wav> <video.mp4> <midi.mid> <output.mp4>
       [--max-notes N] [--vfx] [--wide-pitch] [--min-dur 0.4]
"""
import subprocess, sys, os, struct, wave, math

def run(cmd, **kwargs):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, **kwargs)
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

def find_vowels(path, sr=44100):
    w = wave.open(path, 'r')
    n = w.getnframes(); raw = w.readframes(n); w.close()
    samples = struct.unpack(f'<{n}h', raw)
    ws = int(sr * 0.02); hop = ws // 2
    candidates = []
    for i in range(0, n - ws, hop):
        window = samples[i:i+ws]
        energy = sum(s*s for s in window) / ws
        zcr = sum(1 for j in range(1, len(window)) if (window[j] >= 0) != (window[j-1] >= 0)) / ws
        if energy > 1e6 and 0.03 < zcr < 0.35:
            pitch = sr * zcr / 2
            if 80 < pitch < 1000:
                candidates.append({'time': i/sr, 'energy': energy, 'zcr': zcr, 'pitch': pitch})
    segments = []
    i = 0
    while i < len(candidates):
        start = candidates[i]['time']; end = start + 0.02; pitches = [candidates[i]['pitch']]
        j = i + 1
        while j < len(candidates) and candidates[j]['time'] - end < 0.05:
            end = candidates[j]['time'] + 0.02; pitches.append(candidates[j]['pitch']); j += 1
        if end - start > 0.05:
            avg_pitch = sum(pitches) / len(pitches)
            midi_note = int(69 + 12 * math.log2(avg_pitch / 440) + 0.5)
            segments.append({'start': start, 'dur': end - start, 'pitch': avg_pitch, 'midi': midi_note})
        i = j
    segments.sort(key=lambda s: s['pitch'])
    result = []
    if segments:
        result.append(segments[0])  # Lowest pitch
        result.append(segments[len(segments)//2])  # Middle
        result.append(segments[-1])  # Highest
    return result

def main():
    if len(sys.argv) < 5:
        print("Usage: ytpmv_produce.py <audio.wav> <video.mp4> <midi.mid> <output.mp4> [--max-notes N] [--vfx] [--wide-pitch]")
        sys.exit(1)
    audio_path, video_path, midi_path, output_path = sys.argv[1:5]
    max_notes = 64; enable_vfx = False; wide_pitch = False
    min_clip = 0.4; fade = 0.005; xfade = 0.02
    i = 5
    while i < len(sys.argv):
        if sys.argv[i] == '--max-notes' and i+1 < len(sys.argv): max_notes = int(sys.argv[i+1]); i += 2
        elif sys.argv[i] == '--vfx': enable_vfx = True; i += 1
        elif sys.argv[i] == '--wide-pitch': wide_pitch = True; i += 1
        elif sys.argv[i] == '--min-dur' and i+1 < len(sys.argv): min_clip = float(sys.argv[i+1]); i += 2
        else: i += 1

    pitch_min, pitch_max = (0.33, 3.0) if wide_pitch else (0.5, 2.0)

    print(f"=== YTPMV Producer R131 | vfx={enable_vfx} max={max_notes} pitch=[{pitch_min},{pitch_max}] ===")
    events = parse_midi(midi_path)[:max_notes]
    print(f"Parsed {len(events)} notes")
    samples = find_vowels(audio_path)
    print(f"Found {len(samples)} vowel samples")
    for s in samples:
        print(f"  t={s['start']:.3f}s dur={s['dur']:.3f}s pitch={s['pitch']:.1f}Hz midi={s['midi']}")

    for idx, s in enumerate(samples):
        run(f'ffmpeg -y -v error -ss {s["start"]} -t {s["dur"]} -i "{audio_path}" -acodec pcm_s16le -ar 44100 -ac 1 /tmp/ytpmv_s_{idx}.wav')

    segments = []; total_dur = 0
    for count, evt in enumerate(events):
        note = evt['note']; start = evt['start']
        dur = max(min_clip, min(evt['dur'], 1.5))
        total_dur = max(total_dur, start + dur)
        best_idx = 0; best_dist = 999
        for idx, s in enumerate(samples):
            note_freq = 440 * (2 ** ((note - 69) / 12))
            dist = abs(12 * math.log2(note_freq / s['pitch']))
            if dist < best_dist: best_dist = dist; best_idx = idx
        note_freq = 440 * (2 ** ((note - 69) / 12))
        ratio = max(pitch_min, min(note_freq / samples[best_idx]['pitch'], pitch_max))
        fade_out = max(0, dur - fade)
        cmd = (f'ffmpeg -y -v error -i /tmp/ytpmv_s_{best_idx}.wav '
               f'-af "rubberband=pitch={ratio:.4f}:formant=preserved,'
               f'afade=t=in:st=0:d={fade},afade=t=out:st={fade_out:.4f}:d={fade}" '
               f'-t {dur} /tmp/ytpmv_a_{count}.wav')
        rc, _, _ = run(cmd)
        if rc != 0: continue
        vid_pos = (count * 0.3) % 8.0
        pts = max(0.25, min(dur / 0.6, 4.0))
        cmd = (f'ffmpeg -y -v error -ss {vid_pos:.2f} -t 0.6 -i "{video_path}" '
               f'-vf "setpts={pts:.4f}*PTS" -an -c:v libx264 -preset fast -crf 28 /tmp/ytpmv_v_{count}.mp4')
        rc, _, _ = run(cmd)
        if rc != 0: continue
        segments.append({'start': start, 'dur': dur, 'idx': count, 'note': note, 'ratio': ratio})
        print(f"  [{count}] t={start:.3f}s dur={dur:.3f}s note={note} ratio={ratio:.4f} smp={best_idx}")

    if not segments: print("ERROR: No segments!"); sys.exit(1)
    print(f"Built {len(segments)} segments")

    print("Building video...")
    if len(segments) >= 2:
        vid_ins = ""; vid_flt = ""; offset = 0
        for i, seg in enumerate(segments):
            idx = seg['idx']; vid_ins += f" -i /tmp/ytpmv_v_{idx}.mp4"
            if i == 0: offset = seg['dur'] - xfade
            elif i == len(segments) - 1:
                vid_flt += f"[v{i-1}][{i}:v]xfade=transition=fade:duration={xfade}:offset={offset:.4f}[vout]"
            else:
                vid_flt += f"[v{i-1}][{i}:v]xfade=transition=fade:duration={xfade}:offset={offset:.4f}[v{i}];"
                offset += seg['dur'] - xfade
        if enable_vfx:
            vid_flt += ";[vout]zoompan=z='1.0+0.05*sin(2*PI*t*4)':d=1:x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)'[vfx]"
            map_v = "[vfx]"
        else: map_v = "[vout]"
        run(f'ffmpeg -y -v error{vid_ins} -filter_complex "{vid_flt}" -map "{map_v}" /tmp/ytpmv_video.mp4')
    else: run(f'cp /tmp/ytpmv_v_0.mp4 /tmp/ytpmv_video.mp4')

    print("Mixing audio...")
    aud_ins = ""; aud_flt = ""
    for seg in segments:
        idx = seg['idx']; ms = int(seg['start'] * 1000)
        aud_ins += f" -i /tmp/ytpmv_a_{idx}.wav"
        aud_flt += f"[{idx}:a]adelay={ms}|{ms}[a{idx}];"
    mix_inputs = ''.join(f'[a{seg["idx"]}]' for seg in segments)
    aud_flt += f"{mix_inputs}amix=inputs={len(segments)}:duration=longest[aout]"
    run(f'ffmpeg -y -v error{aud_ins} -filter_complex "{aud_flt}" -map "[aout]" -acodec aac -t {int(total_dur+1)} /tmp/ytpmv_audio.m4a')

    print("Merging...")
    run(f'ffmpeg -y -v error -i /tmp/ytpmv_video.mp4 -i /tmp/ytpmv_audio.m4a -c:v copy -c:a aac -shortest "{output_path}"')

    for f in os.listdir('/tmp'):
        if f.startswith('ytpmv_'): os.remove(f'/tmp/{f}')

    if os.path.exists(output_path):
        size = os.path.getsize(output_path)
        dur = float(subprocess.run(f'ffprobe -v error -show_entries format=duration -of csv=p=0 "{output_path}"', shell=True, capture_output=True, text=True).stdout.strip())
        print(f"=== Done: {output_path} ({size/1024:.0f}KB, {dur:.1f}s) ===")
    else:
        print("ERROR: Failed!"); sys.exit(1)

if __name__ == '__main__':
    main()
