#!/usr/bin/env python3
"""YTPMV Producer v3 — Uses pre-built chromatic instrument.
Usage: python3 tools/ytpmv_produce_v3.py <midi.mid> <output.mp4> [--instrument DIR] [--video MP4]
"""
import subprocess, sys, os, math

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

def main():
    if len(sys.argv) < 3:
        print("Usage: ytpmv_produce_v3.py <midi.mid> <output.mp4> [--instrument DIR] [--video MP4]")
        sys.exit(1)
    
    midi_path = sys.argv[1]
    output_path = sys.argv[2]
    instrument_dir = "/tmp/sb_chromatic"
    video_path = "/tmp/spongebob_ready.mp4"
    
    i = 3
    while i < len(sys.argv):
        if sys.argv[i] == '--instrument' and i+1 < len(sys.argv): instrument_dir = sys.argv[i+1]; i += 2
        elif sys.argv[i] == '--video' and i+1 < len(sys.argv): video_path = sys.argv[i+1]; i += 2
        else: i += 1

    print(f"=== YTPMV Producer v3 (Chromatic Instrument) ===")
    print(f"  Instrument: {instrument_dir}")
    print(f"  Video: {video_path}")

    events = parse_midi(midi_path)[:20]  # First 20 notes only
    print(f"Parsed {len(events)} notes")

    # Check which notes we have available
    available_notes = set()
    for f in os.listdir(instrument_dir):
        if f.startswith("note_") and f.endswith(".wav"):
            midi = int(f.split("_")[1].split(".")[0])
            available_notes.add(midi)
    
    if not available_notes:
        print(f"ERROR: No instrument files in {instrument_dir}")
        sys.exit(1)
    
    min_note = min(available_notes)
    max_note = max(available_notes)
    print(f"  Available notes: MIDI {min_note}-{max_note}")

    # Transpose MIDI to fit instrument range if needed
    # Find the MIDI note range of the melody
    melody_min = min(e['note'] for e in events)
    melody_max = max(e['note'] for e in events)
    print(f"  Melody range: MIDI {melody_min}-{melody_max}")

    # Calculate transpose to center melody in instrument range
    instr_center = (min_note + max_note) // 2
    melody_center = (melody_min + melody_max) // 2
    transpose = instr_center - melody_center
    
    # Clamp transpose to keep melody in range
    if melody_min + transpose < min_note:
        transpose = min_note - melody_min
    if melody_max + transpose > max_note:
        transpose = max_note - melody_max
    
    print(f"  Transpose: {transpose} semitones")

    # Process each note
    segments = []
    total_dur = 0
    FADE = 0.005
    XFADE = 0.02

    for count, evt in enumerate(events):
        note = evt['note'] + transpose
        start = evt['start']
        dur = max(0.15, min(evt['dur'], 1.0))
        total_dur = max(total_dur, start + dur)

        # Clamp to available range
        note = max(min_note, min(note, max_note))
        
        # Use pre-built note file
        note_file = f"{instrument_dir}/note_{int(note)}.wav"
        if not os.path.exists(note_file):
            print(f"  Missing note file: {note_file}")
            continue

        # Copy note with fade envelope
        fade_out = max(0, dur - FADE)
        out_file = f"/tmp/ytpmv3_a_{count}.wav"
        cmd = (f'ffmpeg -y -i "{note_file}" '
               f'-af "afade=t=in:st=0:d={FADE},afade=t=out:st={fade_out:.4f}:d={FADE}" '
               f'-t {dur} "{out_file}"')
        rc, _, _ = run(cmd)
        if rc != 0: continue

        # Video: cycle through different positions
        vid_dur_res = run(f'ffprobe -v error -show_entries format=duration -of csv=p=0 "{video_path}"')
        video_dur = float(vid_dur_res[1].strip()) if vid_dur_res[0] == 0 else 30.0
        vid_pos = (count * 0.2) % max(1, video_dur - 0.5)
        pts = max(0.3, min(dur / 0.5, 3.0))
        vid_file = f"/tmp/ytpmv3_v_{count}.mp4"
        cmd = (f'ffmpeg -y -v error -ss {vid_pos:.2f} -t 0.5 -i "{video_path}" '
               f'-vf "setpts={pts:.4f}*PTS-STARTPTS" -an -c:v libx264 -preset fast -crf 28 -video_track_timescale 30000 "{vid_file}"')
        rc, _, _ = run(cmd)
        if rc != 0: continue

        segments.append({'start': start, 'dur': dur, 'idx': count, 'note': int(note), 'orig_note': evt['note']})
        print(f"  [{count}] t={start:.3f}s dur={dur:.3f}s MIDI {evt['note']}→{int(note)}")

    if not segments:
        print("ERROR: No segments!")
        sys.exit(1)

    print(f"Built {len(segments)} segments")

    # Build video — use concat filter for timestamp handling
    print("Building video...")
    vid_ins = "".join(f" -i /tmp/ytpmv3_v_{seg['idx']}.mp4" for seg in segments)
    vid_flt = "".join(f"[{i}:v]" for i in range(len(segments)))
    vid_flt += f"concat=n={len(segments)}:v=1:a=0[vout]"
    cmd = f'ffmpeg -y -v error{vid_ins} -filter_complex "{vid_flt}" -map "[vout]" -c:v libx264 -preset fast -crf 28 /tmp/ytpmv3_video.mp4'
    print(f"  Video filter: {vid_flt[:80]}...")
    rc, _, err = run(cmd)
    if rc != 0:
        print(f"Video build failed: {err[-300:]}")
    else:
        import subprocess as sp
        r = sp.run('ffprobe -v error -show_entries format=duration -of csv=p=0 /tmp/ytpmv3_video.mp4', shell=True, capture_output=True, text=True)
        print(f"  Video duration: {r.stdout.strip()}s")

    # Mix audio with delays — batch to avoid ffmpeg input limits
    print("Mixing audio...")
    # Mix audio with delays — use adelay filter
    print("Mixing audio...")
    BATCH_SIZE = 16
    batch_outputs = []
    
    for batch_start in range(0, len(segments), BATCH_SIZE):
        batch_end = min(batch_start + BATCH_SIZE, len(segments))
        batch = segments[batch_start:batch_end]
        aud_ins = ""; aud_flt = ""
        for seg_idx, seg in enumerate(batch):
            ms = int(seg['start'] * 1000)
            aud_ins += f" -i /tmp/ytpmv3_a_{seg['idx']}.wav"
            aud_flt += f"[{seg_idx}:a]adelay={ms}|{ms}[b{seg_idx}];"
        mix_inputs = ''.join(f'[b{seg_idx}]' for seg_idx in range(len(batch)))
        out_label = f"mix{batch_start//BATCH_SIZE}"
        aud_flt += f"{mix_inputs}amix=inputs={len(batch)}:duration=longest[{out_label}]"
        batch_outputs.append(f"/tmp/ytpmv3_{out_label}.m4a")
        rc, _, err = run(f'ffmpeg -y -v error{aud_ins} -filter_complex "{aud_flt}" -map "[{out_label}]" /tmp/ytpmv3_{out_label}.m4a')
        if rc != 0:
            print(f"Batch {out_label} failed: {err[-200:]}")
            sys.exit(1)
    
    # Mix batches together
    if len(batch_outputs) > 1:
        final_ins = "".join(f" -i {f}" for f in batch_outputs)
        final_flt = ''.join(f'[{i}:a]' for i in range(len(batch_outputs)))
        final_flt += f"amix=inputs={len(batch_outputs)}:duration=longest,loudnorm=I=-16:TP=-1.5:LRA=11[aout]"
        run(f'ffmpeg -y -v error{final_ins} -filter_complex "{final_flt}" -map "[aout]" -acodec aac -t {int(total_dur+1)} /tmp/ytpmv3_audio.m4a')
    elif len(batch_outputs) == 1:
        run(f'ffmpeg -y -v error -i {batch_outputs[0]} -af "loudnorm=I=-16:TP=-1.5:LRA=11" -acodec aac -t {int(total_dur+1)} /tmp/ytpmv3_audio.m4a')
    
    for f in batch_outputs:
        if os.path.exists(f): os.remove(f)

    # Merge
    print("Merging...")
    rc, _, err = run(f'ffmpeg -y -i /tmp/ytpmv3_video.mp4 -i /tmp/ytpmv3_audio.m4a -c:v libx264 -preset fast -crf 23 -c:a aac -b:a 192k -shortest "{output_path}"')
    if rc != 0:
        print(f"Merge failed: {err[-300:]}")

    # Cleanup (skip on debug)
    if '--debug' not in sys.argv:
        for f in os.listdir('/tmp'):
            if f.startswith('ytpmv3_'):
                os.remove(f'/tmp/{f}')

    if os.path.exists(output_path):
        size = os.path.getsize(output_path)
        dur = float(run(f'ffprobe -v error -show_entries format=duration -of csv=p=0 "{output_path}"')[1].strip())
        
        # Check volume
        vol = run(f'ffmpeg -i "{output_path}" -af "volumedetect" -f null /dev/null 2>&1')
        for line in vol[1].split('\n'):
            if 'mean_volume' in line:
                print(f"  Volume: {line.strip()}")
        print(f"=== Done: {output_path} ({size/1024:.0f}KB, {dur:.1f}s) ===")
    else:
        print("ERROR!"); sys.exit(1)

if __name__ == '__main__':
    main()
