#!/bin/bash
# Test aid: print the engine's phase and drawn-interpolation value for 72 consecutive frames (needs SAGE Unlocked active).
# Smooth = within each tick the value climbs evenly to 1.00 on the last frame, then restarts near 0.
. "$(dirname "$0")/env.sh"
L="$G/letterbox.log"
last() { tr -d '\r' < "$L" | grep -i "^peek 0*$1:" | tail -1; }
"$T/drive.sh" cmd "peek 632601 2"; sleep 2
ST=$(last 632601 | /usr/bin/awk '{ if (substr($3,7,2) != "e8") exit 1; print substr($4,7,2) substr($3,1,6) }') || { echo "no scheduler hook (SAGE Unlocked off?)"; exit 1; }
ST=$(printf '%x' $(( (0x632606 + 0x$ST) & 0xffffffff )))
"$T/drive.sh" cmd "peek $ST 2"; sleep 2; ENGV=$(last "$ST" | /usr/bin/awk '{ print substr($4,5,4) substr($3,1,4) }')
"$T/drive.sh" cmd "peek $ENGV 1"; sleep 2; E=$(last "$ENGV" | /usr/bin/awk '{print $3}'); A=$(printf '%x' $((0x$E + 0x34)))
cmds=(); for i in $(seq 1 72); do cmds+=("peek $A 3"); done
n0=$(wc -l < "$L" | tr -d ' '); "$T/drive.sh" cmd "${cmds[@]}"; sleep 4
tr -d '\r' < "$L" | tail -n +$((n0+1)) | grep "^peek" | /usr/bin/perl -ane 'printf "%d:%.2f%s", hex($F[2]), unpack("f", pack("L", hex($F[4]))), ($. % 12 == 0 ? "\n" : "  "); END { print "\n" }'
