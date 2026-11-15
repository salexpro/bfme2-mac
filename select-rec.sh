#!/bin/bash
# Test aid: in a skirmish, record the brightness of the fortress area while selecting/deselecting it three times.
. "$(dirname "$0")/env.sh"
"$T/enter-skirmish.sh" >/dev/null 2>&1
read -r W H <<< "$(game_size)"
F=${1:-120}     # frames per second, for the waits
fx=$((W*49/100)); fy=$((H*47/100)); gx=$((W*80/100)); gy=$((H*25/100))
rm -f "${G:?}/letterbox.rec"
"$T/drive.sh" cmd "move $gx $gy" "wait $F" "rec $((F*13)) $((W*40/100)) $((H*30/100)) $((W*18/100)) $((H*32/100))" "wait $F" \
  "click $fx $fy" "wait $((F*2))" "click $gx $gy" "wait $((F*2))" "click $fx $fy" "wait $((F*2))" "click $gx $gy" "wait $((F*2))" "click $fx $fy" "wait $((F*2))"
for i in $(seq 1 60); do grep -q "rec finished" "$G/letterbox.log" 2>/dev/null && break; sleep 1; done
wc -l < "$G/letterbox.rec"
