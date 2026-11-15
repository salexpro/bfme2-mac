#!/bin/bash
# Test automation: (re)start BFME II from Steam and click through to a running skirmish on the default map.
# Click positions are fractions of the game's resolution (read from the letterbox log), so any resolution works.
. "$(dirname "$0")/env.sh"; need_steam
pkill -f "game.dat"; pkill -f lotrbfme2
for i in $(seq 1 30); do tail -1 "$CD/notproton-run.log" 2>/dev/null | grep -q "session ending" && break; sleep 1; done
rm -f "${G:?}/letterbox.log" "${G:?}/letterbox.cmd"
open "$URL"
game_up 90
read -r W H <<< "$(game_size)"
[ -n "$W" ] || { echo "the game did not come up (see $G/letterbox.log)"; exit 1; }
at() { echo "$(( W * $1 / 10000 )) $(( H * $2 / 10000 ))"; }   # fractions in 1/10000
sleep "${MENU_WAIT:-45}"                       # logos + shell map
"$T/drive.sh" cmd "click $(at 2708 9241)" "wait ${CLICK_WAIT:-90}" "click $(at 2708 6426)" "wait $(( ${CLICK_WAIT:-90} * 8 / 3 ))" "click $(at 8813 9556)"
sleep "${LOAD_WAIT:-45}"
"$T/drive.sh" shot ingame
