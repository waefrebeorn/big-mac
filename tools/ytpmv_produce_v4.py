#!/usr/bin/env python3
"""YTPMV Producer v4 — Full song support with batched rendering.
Handles hundreds of notes by rendering audio in timeline passes.
"""
import subprocess, sys, os, math

def run(cmd):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr

def parse_midi(path, max_notes=500):
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
            events.append({'start': t, 'dur': 0.15, 'note': msg.note, 'vel': msg.velocity})
        elif msg.type == 'note_off' or (msg.type == 'note_on' and msg.velocity == 0):
            if msg.note in note_ons:
                start, vel = note_ons.pop(msg.note)
                end_t = mido.tick2second(abs_tick, ticks_per_beat, tempo)
                for e in events:
                    if abs(e['start'] - start) < 0.001:
                        e['dur'] = max(0.08, end_t - start)
                        break
    return events[:max_notes]

def main():
    if len(sys.argv) < 3:
        print("Usage: ytpmv_produce_v4.py <midi.mid> <output.mp4> [--instrument DIR] [--video MP4] [--max-notes N]")
        sys.exit(1)
    midi_path = sys.argv[1]; output_path = sys.argv[2]
    instrument_dir = "/tmp/sb_chromatic"; video_path = "/tmp/spongebob_ready.mp4"
    max_notes = 200
    i = 3
    while i < len(sys.argv):
        if sys.argv[i] == '--instrument' and i+1 < len(sys.argv): instrument_dir = sys.argv[i+1]; i += 2
        elif sys.argv[i] == '--video' and i+1 < len(sys.argv): video_path = sys.argv[i+1]; i += 2
        elif sys.argv[i] == '--max-notes' and i+1 < len(sys.argv): max_notes = int(sys.argv[i+1]); i += 2
        else: i += 1

    print(f"=== YTPMV Producer v4 ===")
    events = parse_midi(midi_path, max_notes)
    print(f"Parsed {len(events)} notes")

    # Check available notes
    available = set()
    for f in os.listdir(instrument_dir):
        if f.startswith("note_") and f.endswith(".wav"):
            available.add(int(f.split("_")[1].split(".")[0]))
    min_note = min(available); max_avail = max(available)
    print(f"  Instrument range: MIDI {min_note}-{max_avail}")

    # Calculate transpose
    melody_min = min(e['note'] for e in events)
    melody_max = max(e['note'] for e in events)
    print(f"  Melody range: MIDI {melody_min}-{melody_max}")
    
    transpose = (min_note + max_avail) // 2 - (melody_min + melody_max) // 2
    if melody_min + transpose < min_note: transpose = min_note - melody_min
    if melody_max + transpose > max_avail: transpose = max_avail - melody_max
    print(f"  Transpose: {transpose}")

    # Process notes
    segments = []; total_dur = 0
    FADE = 0.005
    for count, evt in enumerate(events):
        note = max(min_note, min(evt['note'] + transpose, max_avail))
        start = evt['start']; dur = max(0.1, min(evt['dur'], 1.0))
        total_dur = max(total_dur, start + dur)

        note_file = f"{instrument_dir}/note_{int(note)}.wav"
        if not os.path.exists(note_file): continue

        out_file = f"/tmp/v4_a_{count}.wav"
        fade_out = max(0, dur - FADE)
        rc, _, _ = run(f'ffmpeg -y -i "{note_file}" -af "afade=t=in:st=0:d={FADE},afade=t=out:st={fade_out:.4f}:d={FADE}" -t {dur} "{out_file}"')
        if rc != 0: continue

        # Video clip
        vid_dur_res = run(f'ffprobe -v error -show_entries format=duration -of csv=p=0 "{video_path}"')
        video_dur = float(vid_dur_res[1].strip()) if vid_dur_res[0] == 0 else 30.0
        vid_pos = (count * 0.15) % max(1, video_dur - 0.5)
        pts = max(0.3, min(dur / 0.5, 3.0))
        vid_file = f"/tmp/v4_v_{count}.mp4"
        rc, _, _ = run(f'ffmpeg -y -v error -ss {vid_pos:.2f} -t 0.5 -i "{video_path}" -vf "setpts={pts:.4f}*PTS-STARTPTS" -an -c:v libx264 -preset fast -crf 28 "{vid_file}"')
        if rc != 0: continue

        segments.append({'start': start, 'dur': dur, 'idx': count, 'note': int(note)})

    print(f"Built {len(segments)} segments")

    # Build video — use concat demuxer with normalized clips
    print("Building video...")
    # First normalize all clips to same params
    for seg in segments:
        idx = seg['idx']
        run(f'ffmpeg -y -v error -i /tmp/v4_v_{idx}.mp4 -c:v libx264 -preset fast -crf 28 -video_track_timescale 30000 -an /tmp/v4_vn_{idx}.mp4')
    
    with open("/tmp/v4_concat.txt", "w") as f:
        for seg in segments:
            f.write(f"file '/tmp/v4_vn_{seg['idx']}.mp4'\n")
    rc, _, err = run('ffmpeg -y -f concat -safe 0 -i /tmp/v4_concat.txt -c:v libx264 -preset fast -crf 28 /tmp/v4_video.mp4')
    if rc != 0:
        print(f"Video failed: {err[-200:]}")
    else:
        r = run('ffprobe -v error -show_entries format=duration -of csv=p=0 /tmp/v4_video.mp4')
        print(f"  Video: {r[1].strip()}s")

    # Build audio — render in timeline passes
    print("Building audio...")
    # Sort segments by start time
    segments.sort(key=lambda s: s['start'])
    
    # Build audio by mixing batches of 16 notes at a time, then combine passes
    PASS_SIZE = 16
    pass_outputs = []
    
    for pass_idx, pass_start in enumerate(range(0, len(segments), PASS_SIZE)):
        pass_end = min(pass_start + PASS_SIZE, len(segments))
        pass_segs = segments[pass_start:pass_end]
        
        aud_ins = ""; aud_flt = ""
        for seg_idx, seg in enumerate(pass_segs):
            ms = int(seg['start'] * 1000)
            aud_ins += f" -i /tmp/v4_a_{seg['idx']}.wav"
            aud_flt += f"[{seg_idx}:a]adelay={ms}|{ms}[p{seg_idx}];"
        mix_inputs = ''.join(f'[p{seg_idx}]' for seg_idx in range(len(pass_segs)))
        pass_label = f"pass{pass_idx}"
        aud_flt += f"{mix_inputs}amix=inputs={len(pass_segs)}:duration=longest[{pass_label}]"
        
        rc, _, err = run(f'ffmpeg -y -v error{aud_ins} -filter_complex "{aud_flt}" -map "[{pass_label}]" /tmp/v4_{pass_label}.m4a')
        if rc != 0:
            print(f"Pass {pass_idx} failed: {err[-150:]}")
            continue
        pass_outputs.append(f"/tmp/v4_{pass_label}.m4a")
    
    print(f"  {len(pass_outputs)} audio passes")
    
    # Combine passes
    if len(pass_outputs) > 1:
        final_ins = "".join(f" -i {f}" for f in pass_outputs)
        final_flt = ''.join(f'[{i}:a]' for i in range(len(pass_outputs)))
        final_flt += f"amix=inputs={len(pass_outputs)}:duration=longest,loudnorm=I=-16:TP=-1.5:LRA=11[aout]"
        rc, _, err = run(f'ffmpeg -y -v error{final_ins} -filter_complex "{final_flt}" -map "[aout]" -acodec aac -t {int(total_dur+2)} /tmp/v4_audio.m4a')
        if rc != 0:
            print(f"Final mix failed: {err[-200:]}")
    elif len(pass_outputs) == 1:
        run(f'ffmpeg -y -v error -i {pass_outputs[0]} -af "loudnorm=I=-16:TP=-1.5:LRA=11" -acodec aac -t {int(total_dur+2)} /tmp/v4_audio.m4a')

    # Merge
    print("Merging...")
    rc, _, err = run(f'ffmpeg -y -i /tmp/v4_video.mp4 -i /tmp/v4_audio.m4a -c:v libx264 -preset fast -crf 23 -c:a aac -b:a 192k -shortest "{output_path}"')
    if rc != 0:
        print(f"Merge failed: {err[-200:]}")

    # Cleanup
    for f in os.listdir('/tmp'):
        if f.startswith('v4_'):
            try: os.remove(f'/tmp/{f}')
            except: pass

    if os.path.exists(output_path):
        size = os.path.getsize(output_path)
        dur = float(run(f'ffprobe -v error -show_entries format=duration -of csv=p=0 "{output_path}"')[1].strip())
        print(f"=== Done: {output_path} ({size/1024:.0f}KB, {dur:.1f}s) ===")
    else:
        print("ERROR!")

if __name__ == '__main__':
    main()
