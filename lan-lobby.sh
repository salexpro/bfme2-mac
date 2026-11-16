#!/bin/bash
# Test automation: (re)start BFME II and open the LAN lobby (Multiplayer > Network). Prints the game size "W H".
. "$(dirname "$0")/env.sh"; need_steam
pkill -x game.dat; pkill -f lotrbfme2
for i in $(seq 1 30); do tail -1 "$CD/notproton-run.log" 2>/dev/null | grep -q "session ending" && break; sleep 1; done
rm -f "${G:?}/letterbox.log" "${G:?}/letterbox.cmd"
open "$URL"
game_up 90
read -r W H <<< "$(game_size)"
[ -n "$W" ] || { echo "the game did not come up"; exit 1; }
at() { echo "$(( W * $1 / 10000 )) $(( H * $2 / 10000 ))"; }
sleep "${MENU_WAIT:-50}"
"$T/drive.sh" cmd "move $(at 2708 9241)" "wait 40" "move $(at 4531 9241)" "wait 90" "click $(at 4500 7815)" "wait 200"
sleep "${LOBBY_WAIT:-12}"
"$T/drive.sh" shot lobby >/dev/null
echo "$W $H"
