#!/bin/bash
# mk_ytpmv.sh — YTPMV Production Script (R120)
# Single sample pitch mapping + video segments on timeline
AUDIO="$1"; VIDEO="$2"; MIDI="$3"; OUTPUT="$4"
[ -z "$OUTPUT" ] && { echo "Usage: $0 <audio.wav> <video.mp4> <midi.mid> <output.mp4>"; exit 1; }

echo "=== YTPMV Producer v120 ==="

# Step 1: Parse MIDI
python3 /tmp/parse_midi.py "$MIDI" > /tmp/ytpmv_notes.tsv
NOTES=$(wc -l < /tmp/ytpmv_notes.tsv | tr -d ' ')
echo "Parsed $NOTES notes"

# Step 2: Extract vowel
VOWEL="/tmp/ytpmv_vowel.wav"
ffmpeg -y -v error -ss 1.7 -t 0.15 -i "$AUDIO" -acodec pcm_s16le -ar 44100 -ac 1 "$VOWEL" 2>/dev/null
[ ! -f "$VOWEL" ] && { echo "Vowel extract failed!"; exit 1; }

VOWEL_PITCH=$(python3 -c "
import struct, wave
w = wave.open('$VOWEL', 'r')
frames = w.readframes(w.getnframes())
samples = struct.unpack('<' + 'h' * w.getnframes(), frames)
crossings = sum(1 for i in range(1, len(samples)) if (samples[i] >= 0) != (samples[i-1] >= 0))
sr = w.getframerate()
freq = crossings * sr / (2 * len(samples))
print(f'{max(min(freq, 800), 80):.0f}')
w.close()
" 2>/dev/null)
[ -z "$VOWEL_PITCH" ] && VOWEL_PITCH=800
echo "Vowel pitch: ${VOWEL_PITCH}Hz"

# Step 3: Process each note
echo "Processing notes..."
MAX_NOTES=40; COUNT=0; TOTAL_DUR=0

SEG_STARTS=""
SEG_VIDS=""
SEG_AUDIOS=""

while IFS=$'\t' read -r START DUR NOTE VEL; do
    [ "$COUNT" -ge "$MAX_NOTES" ] && break
    [ -z "$START" ] && continue
    
    DUR=$(python3 -c "d=$DUR; print(max(0.05, min(d, 0.8)))")
    NOTE_END=$(python3 -c "print($START + $DUR)")
    TOTAL_DUR=$(python3 -c "t=$TOTAL_DUR; e=$NOTE_END; print(max(t,e))")
    
    RATIO=$(python3 -c "note=$NOTE; target=440.0*(2.0**((note-69.0)/12.0)); src=$VOWEL_PITCH; print(f'{max(min(target/src,3.0),0.33):.4f}')")
    
    # Audio: pitch-shift vowel
    SHIFT="/tmp/ytpmv_a_${COUNT}.wav"
    ffmpeg -y -v error -i "$VOWEL" -af "rubberband=pitch=$RATIO:formant=preserved" "$SHIFT" 2>/dev/null
    [ ! -f "$SHIFT" ] && continue
    SHIFT_TRIM="/tmp/ytpmv_at_${COUNT}.wav"
    ffmpeg -y -v error -i "$SHIFT" -t "$DUR" -acodec pcm_s16le "$SHIFT_TRIM" 2>/dev/null
    [ ! -f "$SHIFT_TRIM" ] && continue
    
    # Video: extract clip and speed to match note duration
    VID_SRC_POS=$(python3 -c "print(f'{($COUNT * 0.4) % 10.0:.2f}')")
    VID_SRC_DUR=0.4
    # setpts factor: <1 = faster, >1 = slower
    # To make VID_SRC_DUR fit into DUR: factor = DUR / VID_SRC_DUR
    PTS_FACTOR=$(python3 -c "f=$DUR/$VID_SRC_DUR; print(max(0.25, min(f, 4.0)))")
    
    VID="/tmp/ytpmv_v_${COUNT}.mp4"
    ffmpeg -y -v error -ss "$VID_SRC_POS" -t "$VID_SRC_DUR" -i "$VIDEO" \
        -vf "setpts=${PTS_FACTOR}*PTS" -an -c:v libx264 -preset fast -crf 28 "$VID" 2>/dev/null
    [ ! -f "$VID" ] && continue
    
    SEG_STARTS="$SEG_STARTS $START"
    SEG_VIDS="$SEG_VIDS $VID"
    SEG_AUDIOS="$SEG_AUDIOS $SHIFT_TRIM"
    
    echo "  [$COUNT] t=${START}s dur=${DUR}s note=$NOTE ratio=$RATIO pts=$PTS_FACTOR"
    COUNT=$((COUNT + 1))
done < /tmp/ytpmv_notes.tsv

[ "$COUNT" -eq 0 ] && { echo "No segments!"; exit 1; }

echo "Building timeline with $COUNT segments..."

# Step 4: Create black background
BG_DUR=$(python3 -c "print(int($TOTAL_DUR + 1))")
ffmpeg -y -v error -f lavfi -i "color=c=black:s=854x480:d=$BG_DUR" /tmp/ytpmv_bg.mp4 2>/dev/null

# Step 5: Build overlay chain for video
# Start with background, overlay each video segment at its time
FILTER=""
for ((i=0; i<COUNT; i++)); do
    START=$(echo $SEG_STARTS | awk "{print \$$((i+1))}")
    END=$(python3 -c "print($START + $(echo $SEG_AUDIOS | awk "{print \$$((i+1))}" | sed 's/.*_//; s/.wav//' ) 0.2)")
    # Get duration from the note data
    NOTE_DUR=$(sed -n "$((i+1))p" /tmp/ytpmv_notes.tsv | cut -f2)
    END=$(python3 -c "print($START + $NOTE_DUR)")
    
    INPUT_NUM=$((i+1))
    if [ $i -eq 0 ]; then
        FILTER="[0:v][${INPUT_NUM}:v]overlay=enable='between(t\\,${START}\\,${END})':x=0:y=0"
    else
        FILTER="${FILTER}[v${i}];[v${i}][${INPUT_NUM}:v]overlay=enable='between(t\\,${START}\\,${END})':x=0:y=0"
    fi
    if [ $i -lt $((COUNT-1)) ]; then
        FILTER="${FILTER}[v$((i+1))]"
    fi
done
FILTER="${FILTER}[vout]"

echo "Building video..."
ffmpeg -y -v error -i /tmp/ytpmv_bg.mp4 $(for f in $SEG_VIDS; do echo "-i $f"; done) \
    -filter_complex "$FILTER" -map "[vout]" /tmp/ytpmv_video.mp4 2>/dev/null

# Step 6: Mix audio with delays
echo "Mixing audio..."
AUDIO_DELAY=""
AUDIO_MIX=""
for ((i=0; i<COUNT; i++)); do
    START=$(echo $SEG_STARTS | awk "{print \$$((i+1))}")
    MS=$(python3 -c "print(int($START * 1000))")
    AUDIO_DELAY="${AUDIO_DELAY}[${i}:a]adelay=${MS}|${MS}[a${i}];"
    AUDIO_MIX="${AUDIO_MIX}[a${i}]"
done
MIX="${AUDIO_DELAY}${AUDIO_MIX}amix=inputs=${COUNT}:duration=longest[aout]"

ffmpeg -y -v error $(for f in $SEG_AUDIOS; do echo "-i $f"; done) \
    -filter_complex "$MIX" -map "[aout]" -acodec aac -t "$BG_DUR" /tmp/ytpmv_audio.m4a 2>/dev/null

# Step 7: Merge
echo "Merging..."
ffmpeg -y -v error -i /tmp/ytpmv_video.mp4 -i /tmp/ytpmv_audio.m4a -c:v copy -c:a aac -shortest "$OUTPUT" 2>/dev/null

rm -f /tmp/ytpmv_*.wav /tmp/ytpmv_*.mp4 /tmp/ytpmv_*.m4a
echo "=== Done: $OUTPUT ==="
ls -lh "$OUTPUT"
