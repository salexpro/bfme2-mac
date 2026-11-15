#!/bin/bash
# Test automation: (re)start BFME II and open the LAN lobby (Multiplayer > Network). Prints the game size "W H".
T="$HOME/Library/Application Support/ClaudeGameTools/bfme2"
G="${BFME_GAME:-$HOME/Library/Application Support/Steam/steamapps/compatdata/3308318774/pfx/drive_c/BFME2}"
CD="${BFME_COMPAT:-$HOME/Library/Application Support/Steam/steamapps/compatdata/2596813934}"
URL="${BFME_URL:-steam://rungameid/11153230920360656896}"
pkill -f "game.dat"; pkill -f lotrbfme2
for i in $(seq 1 30); do tail -1 "$CD/notproton-run.log" 2>/dev/null | grep -q "session ending" && break; sleep 1; done
rm -f "${G:?}/letterbox.log" "${G:?}/letterbox.cmd"
open "$URL"
for i in $(seq 1 90); do grep -q "letterbox on" "$G/letterbox.log" 2>/dev/null && break; sleep 1; done
read -r W H <<< "$(tr -d '\r' < "$G/letterbox.log" | sed -n 's/^letterbox on: game \([0-9]*\)x\([0-9]*\) .*/\1 \2/p' | tail -1)"
[ -n "$W" ] || { echo "the game did not come up"; exit 1; }
at() { echo "$(( W * $1 / 10000 )) $(( H * $2 / 10000 ))"; }
sleep "${MENU_WAIT:-50}"
"$T/drive.sh" cmd "move $(at 2708 9241)" "wait 40" "move $(at 4531 9241)" "wait 90" "click $(at 4500 7815)" "wait 200"
sleep "${LOBBY_WAIT:-12}"
"$T/drive.sh" shot lobby >/dev/null
echo "$W $H"
