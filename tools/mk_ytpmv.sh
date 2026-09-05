#!/bin/bash
# mk_ytpmv.sh — YTPMV Production Script (R125)
# Complete pipeline with epilepsy safety, crossfades, multi-sample
AUDIO="$1"; VIDEO="$2"; MIDI="$3"; OUTPUT="$4"
[ -z "$OUTPUT" ] && { echo "Usage: $0 <audio.wav> <video.mp4> <midi.mid> <output.mp4>"; exit 1; }

echo "=== YTPMV Producer v125 ==="

MAX_NOTES=64; MIN_CLIP_DUR=0.5; FADE_DUR=0.005; XFADE_DUR=0.03

# Step 1: Parse MIDI
python3 /tmp/parse_midi.py "$MIDI" > /tmp/ytpmv_notes.tsv 2>/dev/null
NOTES=$(wc -l < /tmp/ytpmv_notes.tsv | tr -d ' ')
echo "Parsed $NOTES notes"

# Step 2: Extract vowel samples
echo "Extracting vowel samples..."
python3 /tmp/find_vowels.py "$AUDIO" > /tmp/ytpmv_samples.tsv
SAMPLE_COUNT=$(wc -l < /tmp/ytpmv_samples.tsv | tr -d ' ')
echo "Found $SAMPLE_COUNT samples"

# Read samples
SAMPLE_STARTS=""; SAMPLE_DURS=""; SAMPLE_PITCHES=""
while IFS=$'\t' read -r S D P M; do
    SAMPLE_STARTS="$SAMPLE_STARTS $S"
    SAMPLE_DURS="$SAMPLE_DURS $D"
    SAMPLE_PITCHES="$SAMPLE_PITCHES $P"
done < /tmp/ytpmv_samples.tsv

# Extract sample WAVs
IDX=0
for S in $SAMPLE_STARTS; do
    D=$(echo $SAMPLE_DURS | awk "{print \$$((IDX+1))}")
    ffmpeg -y -v error -ss "$S" -t "$D" -i "$AUDIO" -acodec pcm_s16le -ar 44100 -ac 1 "/tmp/ytpmv_s_$IDX.wav" 2>/dev/null
    IDX=$((IDX+1))
done

# Step 3: Process notes
echo "Processing notes..."
COUNT=0; TOTAL_DUR=0

while IFS=$'\t' read -r START DUR NOTE VEL; do
    [ "$COUNT" -ge "$MAX_NOTES" ] && break
    [ -z "$START" ] && continue
    
    # Clamp duration for epilepsy safety
    DUR=$(python3 -c "d=$DUR; print(f'{max($MIN_CLIP_DUR, min(d, 1.5)):.4f}')")
    NOTE_END=$(python3 -c "print($START + $DUR)")
    TOTAL_DUR=$(python3 -c "print(max($TOTAL_DUR, $NOTE_END))")
    
    # Select best sample
    BEST_IDX=0; BEST_DIST=999
    for ((s=0; s<SAMPLE_COUNT; s++)); do
        SPITCH=$(echo $SAMPLE_PITCHES | awk "{print \$$((s+1))}")
        DIST=$(python3 -c "
import math
note_freq=440*(2**(($NOTE-69)/12))
sample_freq=$SPITCH
dist=abs(12*math.log2(note_freq/sample_freq))
print(f'{dist:.1f}')
" 2>/dev/null || echo "999")
        if (( $(echo "$DIST < $BEST_DIST" | bc -l) )); then
            BEST_DIST=$DIST; BEST_IDX=$s
        fi
    done
    
    # Pitch ratio
    BEST_PITCH=$(echo $SAMPLE_PITCHES | awk "{print \$$((BEST_IDX+1))}")
    RATIO=$(python3 -c "note_freq=440*(2**(($NOTE-69)/12)); r=note_freq/$BEST_PITCH; print(f'{max(0.5,min(r,2.0)):.4f}')")
    
    # Audio with envelope
    SHIFT="/tmp/ytpmv_a_${COUNT}.wav"
    FADE_OUT=$(python3 -c "print(max(0, $DUR - $FADE_DUR))")
    ffmpeg -y -v error -i "/tmp/ytpmv_s_$BEST_IDX.wav" \
        -af "rubberband=pitch=$RATIO:formant=preserved,afade=t=in:st=0:d=$FADE_DUR,afade=t=out:st=$FADE_OUT:d=$FADE_DUR" \
        -t "$DUR" "$SHIFT" 2>/dev/null
    [ ! -f "$SHIFT" ] && continue
    
    # Video: extract clip and speed to match
    VID_POS=$(python3 -c "print(f'{($COUNT * 0.3) % 8.0:.2f}')")
    PTS=$(python3 -c "f=$DUR/0.6; print(f'{max(0.25,min(f,4.0)):.4f}')")
    VID="/tmp/ytpmv_v_${COUNT}.mp4"
    ffmpeg -y -v error -ss "$VID_POS" -t 0.6 -i "$VIDEO" -vf "setpts=${PTS}*PTS" -an -c:v libx264 -preset fast -crf 28 "$VID" 2>/dev/null
    [ ! -f "$VID" ] && continue
    
    echo "  [$COUNT] t=${START}s dur=${DUR}s note=$NOTE ratio=$RATIO sample=$BEST_IDX"
    COUNT=$((COUNT + 1))
done < /tmp/ytpmv_notes.tsv

[ "$COUNT" -eq 0 ] && { echo "No segments!"; exit 1; }

echo "Building output with $COUNT segments..."

# Step 4: Build video with crossfades (xfade chain)
# This creates smooth transitions between clips instead of hard cuts
echo "Building video with crossfades..."

# Build xfade chain
XFADE_CMD=""
OFFSET=0
for ((i=0; i<COUNT; i++)); do
    DUR=$(sed -n "$((i+1))p" /tmp/ytpmv_notes.tsv | cut -f2)
    DUR=$(python3 -c "d=$DUR; print(f'{max($MIN_CLIP_DUR, min(d, 1.5)):.4f}')")
    
    if [ $i -eq 0 ]; then
        XFADE_CMD="$XFADE_CMD -i /tmp/ytpmv_v_$i.mp4"
        OFFSET=$(python3 -c "print($OFFSET + $DUR - $XFADE_DUR)")
    elif [ $i -eq $((COUNT-1)) ]; then
        XFADE_CMD="$XFADE_CMD -i /tmp/ytpmv_v_$i.mp4"
        XFADE_FILTER="${XFADE_FILTER}[v$((i-1))][$i:v]xfade=transition=fade:duration=$XFADE_DUR:offset=$OFFSET[vout]"
    else
        XFADE_CMD="$XFADE_CMD -i /tmp/ytpmv_v_$i.mp4"
        XFADE_FILTER="${XFADE_FILTER}[v$((i-1))][$i:v]xfade=transition=fade:duration=$XFADE_DUR:offset=$OFFSET[v$((i))];"
        OFFSET=$(python3 -c "print($OFFSET + $DUR - $XFADE_DUR)")
    fi
done

# For 2+ clips, use xfade; for 1 clip, just copy
if [ "$COUNT" -ge 2 ]; then
    # Fix the filter chain - replace last [vN] with [vout]
    XFADE_FILTER=$(echo "$XFADE_FILTER" | sed "s/\[v$((COUNT-1))\]\[$((COUNT-1)):v\]/[v$((COUNT-2))][$((COUNT-1)):v]/")
    ffmpeg -y -v error $XFADE_CMD -filter_complex "$XFADE_FILTER" -map "[vout]" /tmp/ytpmv_video.mp4 2>/dev/null
else
    cp /tmp/ytpmv_v_0.mp4 /tmp/ytpmv_video.mp4
fi

# Step 5: Mix audio with delays
echo "Mixing audio..."
AUD_INS=""; AUD_FX=""
for ((i=0; i<COUNT; i++)); do
    START=$(sed -n "$((i+1))p" /tmp/ytpmv_notes.tsv | cut -f1)
    MS=$(python3 -c "print(int($START*1000))")
    AUD_INS="$AUD_INS -i /tmp/ytpmv_a_$i.wav"
    AUD_FX="${AUD_FX}[$i:a]adelay=${MS}|${MS}[a$i];"
done
AUD_FX="${AUD_FX}$(printf '[a%d]' $(seq 0 $((COUNT-1))))amix=inputs=$COUNT:duration=longest[aout]"
OUT_DUR=$(python3 -c "print(int($TOTAL_DUR+1))")
ffmpeg -y -v error $AUD_INS -filter_complex "$AUD_FX" -map "[aout]" -acodec aac -t "$OUT_DUR" /tmp/ytpmv_audio.m4a 2>/dev/null

# Step 6: Merge
echo "Merging..."
ffmpeg -y -v error -i /tmp/ytpmv_video.mp4 -i /tmp/ytpmv_audio.m4a -c:v copy -c:a aac -shortest "$OUTPUT" 2>/dev/null

rm -f /tmp/ytpmv_*.wav /tmp/ytpmv_*.mp4 /tmp/ytpmv_*.m4a /tmp/ytpmv_*.tsv /tmp/ytpmv_*.txt
echo "=== Done: $OUTPUT ==="
ls -lh "$OUTPUT"
