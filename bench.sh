#!/bin/bash
# Test automation: start the game, load a saved skirmish and measure the frame rate for a while.
#   bench.sh [seconds to measure, default 60] [row of the save in the load screen's Skirmish tab, default 1]
# Environment: MENU_WAIT (s from device creation to the main menu, default 48), LOAD_WAIT (s for the save to load,
# default 80), KEEP=1 (leave the game running afterwards), SHOT=<name> (take a frame at the end), SAMPLE=1 (switch
# the proxy's sampling profiler on for the measured stretch).
# Prints the fps of every 10 s window of the measured stretch, their mean, and the profile lines when
# letterbox.ini has profile=1.
. "$(dirname "$0")/env.sh"; need_steam
SECS="${1:-60}"; ROW="${2:-1}"
pkill -x game.dat; pkill -f lotrbfme2; sleep 3
rm -f "$G/letterbox.log" "$G/letterbox.cmd"
open "$URL"
game_up 150 || { echo "the game did not come up (see $G/letterbox.log)"; exit 1; }
read -r W H <<< "$(game_size)"
at() { echo "$(( W * $1 / 10000 )) $(( H * $2 / 10000 ))"; }
sleep "${MENU_WAIT:-48}"
"$T/drive.sh" cmd "click $(at 2708 9241)" "wait 150"; sleep 4          # Solo Play
"$T/drive.sh" cmd "click $(at 2710 8290)" "wait 150"; sleep 7          # Load Game
"$T/drive.sh" cmd "click $(at 5040 2220)" "wait 150"; sleep 4          # tab: Skirmish
"$T/drive.sh" cmd "click $(at 2000 $(( 3420 + (ROW - 1) * 320 )))" "wait 100" "click $(at 4430 9570)"   # the save, Load
sleep "${LOAD_WAIT:-80}"
[ -n "$SAMPLE" ] && "$T/drive.sh" cmd "sample 1"          # sampling profiler on only now (while a map loads it can hang the load)
n0=$(tr -d '\r' < "$G/letterbox.log" | grep -c '^fps')
sleep "$SECS"; sleep 3
tr -d '\r' < "$G/letterbox.log" | awk -v n0="$n0" '
  /^fps/ { n++; if (n > n0) { v[++k] = $2; s += $2 } next }
  n > n0 && /profile|per frame:/ { p[++m] = $0 }
  END { printf "fps:"; for (i = 1; i <= k; i++) printf " %s", v[i]; if (k) printf "  | mean %.1f over %d windows\n", s / k, k; else print " (no window)";
        for (i = (m > 4 ? m - 3 : 1); i <= m; i++) print p[i] }'
[ -n "$SHOT" ] && "$T/drive.sh" shot "$SHOT"
[ -n "$KEEP" ] || { pkill -x game.dat; pkill -f lotrbfme2; }
exit 0
