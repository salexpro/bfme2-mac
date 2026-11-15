#!/bin/bash
# Test automation for BFME II through the letterbox d3d9.dll (see letterbox.c "test automation").
#   drive.sh cmd "click 400 1500" "wait 30" ...   send command lines
#   drive.sh shot <name>                           take a frame, convert to <scratch>/<name>.jpg, print the path
G="${BFME_GAME:-$HOME/Library/Application Support/Steam/steamapps/compatdata/3308318774/pfx/drive_c/BFME2}"
OUT="${BFME_SHOTS:-$(cd "$(dirname "$0")" && pwd)/shots}"
mkdir -p "$OUT"
case "$1" in
  cmd) shift; for i in $(seq 1 100); do [ -e "$G/letterbox.cmd" ] || break; sleep 0.2; done
       printf '%s\n' "$@" > "$G/letterbox.cmd.tmp" && mv "$G/letterbox.cmd.tmp" "$G/letterbox.cmd" ;;
  shot) n="$2"; rm -f "$G/$n.bmp"; "$0" cmd "shot $n"
        for i in $(seq 1 100); do [ -s "$G/$n.bmp" ] && break; sleep 0.2; done; sleep 0.4
        [ -s "$G/$n.bmp" ] || { echo "no screenshot (is the game running and drawing?)"; exit 1; }
        sips -s format jpeg -s formatOptions 80 "$G/$n.bmp" --out "$OUT/$n.jpg" >/dev/null 2>&1 && rm -f "$G/$n.bmp"; echo "$OUT/$n.jpg" ;;
  *) echo "usage: drive.sh cmd <lines...> | shot <name>"; exit 2 ;;
esac
