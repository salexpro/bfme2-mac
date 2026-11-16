#!/bin/bash
# Test automation: load a saved skirmish and let it run, to see whether the game survives.
#   soak.sh [minutes, default 20] [row of the save in the load screen's Skirmish tab, default 1]
# Ends early when the game exits (crash) or stops finishing frames (freeze). Prints one result line, the frame rate
# at the end and, when display-override.conf has "export FEX_SILENTLOG=0", the address of the crash from the
# emulator's log. The game is closed at the end unless KEEP=1. FRESH=1 starts a new skirmish (the setup the game
# remembers) instead of loading a save.
. "$(dirname "$0")/env.sh"; need_steam
MIN="${1:-20}"; ROW="${2:-1}"
WLOG="$HOME/Library/Application Support/notproton/launchers/$(basename "$CD")/notproton-wine.log"
if [ -n "$FRESH" ]; then LOAD_WAIT="${LOAD_WAIT:-80}" "$T/enter-skirmish.sh" >/dev/null 2>&1      # FRESH=1: a new skirmish with the remembered setup instead of a save
else KEEP=1 LOAD_WAIT="${LOAD_WAIT:-80}" "$T/bench.sh" 10 "$ROW" >/dev/null 2>&1; fi
start=$(date +%s); r="clean for $MIN min"; pf=-1
while [ $(( $(date +%s) - start )) -lt $(( MIN * 60 )) ]; do
  pgrep -x game.dat >/dev/null || { r="GAME EXITED after $(( ($(date +%s) - start) / 60 )) min"; break; }
  f=$(tr -d '\r' < "$G/letterbox.log" 2>/dev/null | grep -c '^fps')
  [ "$f" = "$pf" ] && { r="NO FRAMES for 40 s (freeze) after $(( ($(date +%s) - start) / 60 )) min"; break; }
  pf=$f; sleep 40
done
echo "$r | last fps: $(tr -d '\r' < "$G/letterbox.log" | grep '^fps' | tail -3 | awk '{printf "%s ", $2}')| $(tr -d '\r' < "$G/letterbox.log" | grep 'per frame: [0-9]* draws' | tail -1 | sed 's/^ *//')"
[ -f "$WLOG" ] && grep -a "eip: " "$WLOG" | grep -v 7BED61E3 | tail -2 | cut -c1-120
[ -f "$WLOG" ] && { n=$(grep -a -c "self-modifying" "$WLOG"); [ "$n" -gt 20000 ] && echo "emulator log: $n write-fault lines (fault storm), last: $(grep -a 'self-modifying' "$WLOG" | tail -1 | cut -c1-90)"; }
[ -n "$KEEP" ] || { pkill -x game.dat; pkill -f lotrbfme2; }
exit 0
