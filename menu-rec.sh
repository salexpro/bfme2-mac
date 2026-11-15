#!/bin/bash
# Test aid: start the game, sit in the main menu and record N frames (brightness + frame time) to <game>/letterbox.rec
. "$(dirname "$0")/env.sh"; need_steam
pkill -f "game.dat"; pkill -f lotrbfme2
for i in $(seq 1 20); do tail -1 "$CD/notproton-run.log" 2>/dev/null | grep -q "session ending" && break; sleep 1; done
rm -f "${G:?}/letterbox.log" "${G:?}/letterbox.rec"
open "$URL"
for i in $(seq 1 90); do grep -q "letterbox on" "$G/letterbox.log" 2>/dev/null && break; sleep 1; done
sleep "${MENU_WAIT:-42}"
"$T/drive.sh" cmd "move 1500 600" "rec ${1:-6000}"
for i in $(seq 1 200); do grep -q "rec finished" "$G/letterbox.log" 2>/dev/null && break; sleep 1; done
wc -l < "$G/letterbox.rec"
