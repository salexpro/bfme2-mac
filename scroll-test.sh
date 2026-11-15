#!/bin/bash
# Test automation: three edge-scroll passes in a running skirmish; prints the frame statistics for each.
T="$HOME/Library/Application Support/ClaudeGameTools/bfme2"
G="$HOME/Library/Application Support/Steam/steamapps/compatdata/3308318774/pfx/drive_c/BFME2"
show() { tr -d '\r' < "$G/letterbox.log" | grep -E "^fps" | tail -${1:-2} | sed -E 's/ \| per frame.*//' | cut -c1-120; }
echo "[idle]"; "$T/drive.sh" cmd "move 1400 700"; sleep 21; show 2
for pass in "2879 800:right" "0 800:left" "2879 800:right again"; do
  echo "[scroll ${pass#*:}]"; n0=$(grep -c "long frame" "$G/letterbox.log")
  "$T/drive.sh" cmd "move ${pass%%:*}"; sleep 21; show 2
  echo "   frames over 250 ms: $(( $(grep -c "long frame" "$G/letterbox.log") - n0 ))"
done
"$T/drive.sh" cmd "move 1400 700"
