#!/bin/bash
# Test aid: in a skirmish select a starting builder and record the brightness around it (the selection ring's throb).
T="$HOME/Library/Application Support/ClaudeGameTools/bfme2"
G="${BFME_GAME:-$HOME/Library/Application Support/Steam/steamapps/compatdata/3308318774/pfx/drive_c/BFME2}"
"$T/enter-skirmish.sh" >/dev/null 2>&1
read -r W H <<< "$(tr -d '\r' < "$G/letterbox.log" | sed -n 's/^letterbox on: game \([0-9]*\)x\([0-9]*\) .*/\1 \2/p' | tail -1)"
F=${1:-120}
bx=$((W*${BX:-6400}/10000)); by=$((H*${BY:-6250}/10000))
rm -f "${G:?}/letterbox.rec"
"$T/drive.sh" cmd "move $((W*80/100)) $((H*20/100))" "wait $F" "rec $((F*11)) $((bx - W*4/100)) $((by - H*5/100)) $((W*8/100)) $((H*11/100))" "wait $F" "click $bx $by" "wait $((F*3))" "shot ring" "wait $((F*6))"
for i in $(seq 1 60); do grep -q "rec finished" "$G/letterbox.log" 2>/dev/null && break; sleep 1; done
wc -l < "$G/letterbox.rec"
