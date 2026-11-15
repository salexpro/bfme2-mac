#!/bin/bash
# Test aid: in a skirmish select the fortress and record the brightness of a patch of the blue ring on the ground
# around it (its pulse), 12 s. Argument: frames per second of the run (for the waits).
T="$HOME/Library/Application Support/ClaudeGameTools/bfme2"
G="${BFME_GAME:-$HOME/Library/Application Support/Steam/steamapps/compatdata/3308318774/pfx/drive_c/BFME2}"
"$T/enter-skirmish.sh" >/dev/null 2>&1
read -r W H <<< "$(tr -d '\r' < "$G/letterbox.log" | sed -n 's/^letterbox on: game \([0-9]*\)x\([0-9]*\) .*/\1 \2/p' | tail -1)"
F=${1:-120}
rm -f "${G:?}/letterbox.rec"
"$T/drive.sh" cmd "move $((W*85/100)) $((H*15/100))" "wait $F" "rec $((F*13)) $((W*365/1000)) $((H*240/1000)) $((W*97/1000)) $((H*50/1000))" "wait $F" "click $((W*49/100)) $((H*47/100))" "wait 4" "move $((W*85/100)) $((H*15/100))" "wait $((F*3))" "shot bring" "wait $((F*9))"
for i in $(seq 1 60); do grep -q "rec finished" "$G/letterbox.log" 2>/dev/null && break; sleep 1; done
wc -l < "$G/letterbox.rec"
