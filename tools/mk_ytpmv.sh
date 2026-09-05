#!/bin/bash
# mk_ytpmv.sh — YTPMV Production Script (R116)
AUDIO="$1"; VIDEO="$2"; MIDI="$3"; OUTPUT="$4"
[ -z "$OUTPUT" ] && { echo "Usage: $0 <audio.wav> <video.mp4> <midi.mid> <output.mp4>"; exit 1; }

echo "=== YTPMV Producer ==="

# Step 1: Parse MIDI
echo "Parsing MIDI..."
python3 /tmp/parse_midi.py "$MIDI" > /tmp/ytpmv_notes.tsv
NOTES=$(wc -l < /tmp/ytpmv_notes.tsv | tr -d ' ')
echo "Parsed $NOTES notes"

# Step 2: Audio info
AUDIO_DUR=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$AUDIO")
echo "Audio: ${AUDIO_DUR}s"

# Step 3: Process notes
echo "Processing..."
MAX_NOTES=32; COUNT=0; FC_INPUTS=""; FC_DELAY=""; FC_MIX=""; TOTAL_DUR=0

while IFS=$'\t' read -r START DUR NOTE VEL; do
    [ "$COUNT" -ge "$MAX_NOTES" ] && break
    [ -z "$START" ] && continue
    
    # Clamp duration
    DUR=$(python3 -c "d=$DUR; print(max(0.05, min(d, 1.0)))")
    
    NOTE_END=$(python3 -c "print($START + $DUR)")
    TOTAL_DUR=$(python3 -c "t=$TOTAL_DUR; e=$NOTE_END; print(max(t,e))")
    
    # Pitch ratio
    RATIO=$(python3 -c "note=$NOTE; t=440.0*(2.0**((note-69.0)/12.0)); print(f'{max(min(t/250.0,3.0),0.33):.4f}')")
    
    # Source position
    SRC_POS=$(python3 -c "print(f'{($COUNT * 0.7) % ($AUDIO_DUR * 0.6):.4f}')")
    
    SEG="/tmp/ytpmv_s_${COUNT}.wav"
    SHIFT="/tmp/ytpmv_x_${COUNT}.wav"
    
    ffmpeg -y -v error -ss "$SRC_POS" -t "$DUR" -i "$AUDIO" -acodec pcm_s16le -ar 44100 -ac 1 "$SEG" 2>/dev/null
    [ ! -f "$SEG" ] && continue
    
    ffmpeg -y -v error -i "$SEG" -af "rubberband=pitch=$RATIO:formant=preserved" "$SHIFT" 2>/dev/null
    [ ! -f "$SHIFT" ] && continue
    
    FC_INPUTS="$FC_INPUTS -i $SHIFT"
    DELAY_MS=$(python3 -c "print(int($START * 1000))")
    FC_DELAY="${FC_DELAY}[${COUNT}:a]adelay=${DELAY_MS}|${DELAY_MS}[d${COUNT}];"
    FC_MIX="${FC_MIX}[d${COUNT}]"
    
    echo "  [$COUNT] t=${START}s dur=${DUR}s note=$NOTE ratio=$RATIO"
    COUNT=$((COUNT + 1))
done < /tmp/ytpmv_notes.tsv

[ "$COUNT" -eq 0 ] && { echo "No segments!"; exit 1; }

echo "Mixing $COUNT segments..."

# Step 4: Mix with delays
FC="${FC_DELAY}${FC_MIX}amix=inputs=${COUNT}:duration=longest:dropout_transition=0[outa]"
OUT_DUR=$(python3 -c "print(int($TOTAL_DUR + 2))")

ffmpeg -y -v error $FC_INPUTS -filter_complex "$FC" -map "[outa]" -acodec aac -t "$OUT_DUR" /tmp/ytpmv_mixed.m4a 2>/dev/null

# Step 5: Merge
echo "Merging..."
ffmpeg -y -v error -i "$VIDEO" -i /tmp/ytpmv_mixed.m4a -c:v copy -c:a aac -map 0:v:0 -map 1:a:0 -shortest "$OUTPUT" 2>/dev/null

rm -f /tmp/ytpmv_s_*.wav /tmp/ytpmv_x_*.wav /tmp/ytpmv_mixed.m4a
echo "=== Done: $OUTPUT ==="
ls -lh "$OUTPUT"
