#!/bin/bash
# Test aid: measure the running game's logic tick rate (normal = about 5 per second) through the letterbox d3d9.dll's
# peek/watch commands. Needs SAGE Unlocked active (it finds the engine object through the unlocker's scheduler hook).
. "$(dirname "$0")/env.sh"
L="$G/letterbox.log"
last() { tr -d '\r' < "$L" | grep -i "^peek 0*$1:" | tail -1; }
BFME_GAME="$G" "$T/drive.sh" cmd "peek 632601 2"; sleep 2
ST=$(last 632601 | /usr/bin/awk '{ if (substr($3,7,2) != "e8") exit 1; rel = substr($4,7,2) substr($3,1,6); printf "%s\n", rel }') || { echo "no scheduler hook at 00632601 (SAGE Unlocked off?)"; exit 1; }
ST=$(printf '%x' $(( (0x632606 + 0x$ST) & 0xffffffff )))
BFME_GAME="$G" "$T/drive.sh" cmd "peek $ST 2"; sleep 2
ENGV=$(last "$ST" | /usr/bin/awk '{ print substr($4,5,4) substr($3,1,4) }')
BFME_GAME="$G" "$T/drive.sh" cmd "peek $ENGV 1"; sleep 2
E=$(last "$ENGV" | /usr/bin/awk '{print $3}')
BFME_GAME="$G" "$T/drive.sh" cmd "watch $(printf '%x' $((0x$E + 0x50)))"
n=$(grep -c "^fps" "$L"); while [ "$(grep -c '^fps' "$L")" -lt $((n + 2)) ]; do sleep 1; done
BFME_GAME="$G" "$T/drive.sh" cmd "watch 0"
tr -d '\r' < "$L" | grep "watch .*changes" | tail -1 | /usr/bin/awk '{printf "logic ticks: %.1f per second (normal 5)\n", $3 / 10}'
tr -d '\r' < "$L" | grep "^fps" | tail -1 | cut -c1-70
