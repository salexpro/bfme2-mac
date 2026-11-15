#!/bin/bash
# Steam launch wrapper for "The Battle for Middle-earth II" (NotProton FEX runner).
# Launch options in Steam:  "<this file>" %command%
#
# 1. Display safety. The game must never make a display change mode (on the Mac Studio + Dell U4025QW a real
#    mode switch can kernel-panic the machine: DCP dual_pipe.c:321). That is guaranteed by the letterbox d3d9.dll
#    proxy (letterbox/): it creates the real Direct3D device at the current desktop mode and fits the game's own
#    resolution inside it. Wine's EmulateModeset is NOT used: with it wined3d-OpenGL switches to an experimental
#    framebuffer surface that deadlocks under FEX.
#    Interlock: the game only starts when the proxy is in place, Wine is told to load it, and EmulateModeset is
#    off. Otherwise Wine's harmless "About Wine" box runs instead of the game.
# 2. Files. The All in One Launcher removes files it does not know from the game folder, so ours are put back
#    at every start: letterbox d3d9.dll + letterbox.ini, SAGE Unlocked (dinput8.dll, CnCFpsUnlocker.dll, ini).
# 3. Display adaptation. Before every start the main display is measured (size in points, refresh rate, notch)
#    and written out: Resolution in the game's Options.ini (the largest 16:9 size that fits), fps= for
#    SAGE Unlocked (refresh rate rounded down to a multiple of 15, 30..120), topinset= for the proxy (a MacBook's
#    camera housing). So the same install follows whatever screen is connected. Also IPAddress (LAN lobby) is kept
#    on this Mac's current LAN address.
#    To pin values instead, create display-override.conf next to this file with any of:
#        RES="1920 1080"     FPS=60     INSET=0     VALIGN=top   (picture right under the notch instead of centred)
T="$HOME/Library/Application Support/ClaudeGameTools/bfme2"
LOG="$T/bfme2-fex.log"
PFX="${STEAM_COMPAT_DATA_PATH:-/nonexistent}/pfx"
REG="$PFX/user.reg"
say() { echo "$(date '+%F %T') $*" >> "$LOG"; }

# The game folder is wherever the lotrbfme2.exe that Steam is starting lives.
GAME=""
for a in "$@"; do case "$a" in */lotrbfme2.exe) GAME="${a%/*}" ;; esac; done
[ -n "$GAME" ] && [ -d "$GAME" ] || GAME="$HOME/Library/Application Support/Steam/steamapps/compatdata/3308318774/pfx/drive_c/BFME2"
HARMLESS="$PFX/drive_c/windows/system32/winver.exe"
OPTDIR="$PFX/drive_c/users/crossover/AppData/Roaming/My Battle for Middle-earth II Files"

put() {  # put <source> <name in the game folder>
  [ -f "$1" ] || return 1
  cmp -s "$1" "$GAME/$2" 2>/dev/null || { cp "$1" "$GAME/$2" && say "installed $2"; }
}

# ---- display adaptation -------------------------------------------------------------------------------------
read_display() {  # prints: width height refresh-rate top-inset   (main display, in points)
  /usr/bin/osascript -l JavaScript -e '
    ObjC.import("AppKit");
    var s = $.NSScreen.screens.objectAtIndex(0), f = s.frame, hz = 60, a = 0, b = 0;
    try { hz = s.maximumFramesPerSecond } catch (e) {}
    try { a = s.auxiliaryTopLeftArea.size.height } catch (e) {}
    try { b = s.safeAreaInsets.top } catch (e) {}
    [Math.round(f.size.width), Math.round(f.size.height), Math.round(hz), Math.round(Math.max(a, b))].join(" ")' 2>/dev/null
}
pick() {  # pick <width> <height> <hz> <inset>  ->  prints: res-width res-height fps inset
  local w=$1 h=$2 hz=$3 inset=$4 ah rw rh fps
  [ "$inset" -lt $((h / 4)) ] || inset=0
  ah=$((h - inset))                                   # usable height below the camera housing
  # The largest 16:9 picture that fits. The game lays its widescreen picture and HUD out for exactly 16:9: at any
  # other shape (16:10 on a MacBook) everything is squeezed and the minimap turns into an oval.
  rw=$((ah * 16 / 9)); [ "$rw" -lt "$w" ] || rw=$w
  rw=$((rw & ~1)); rh=$(( (rw * 9 / 16) & ~1 ))        # (the proxy adds exactly this size to the game's mode list)
  fps=$((hz / 15 * 15)); [ "$fps" -ge 30 ] || fps=30; [ "$fps" -le 120 ] || fps=120
  echo "$rw $rh $fps $inset"
}
set_opt() {  # set_opt <key> <value> <Options.ini>   (the file has CRLF line ends)
  local cr=$'\r'
  if grep -q "^$1 = " "$3" 2>/dev/null; then /usr/bin/sed -i '' -E "s/^$1 = [^$cr]*/$1 = $2/" "$3"
  else printf '%s = %s\r\n' "$1" "$2" >> "$3"; fi
}
get_opt() { /usr/bin/sed -n "s/^$1 = //p" "$2" 2>/dev/null | tr -d '\r' | head -1; }

if [ "${1:-}" = "--selftest" ]; then   # bfme2-fex.sh --selftest : show what would be chosen, change nothing
  echo "this display: $(read_display)  ->  $(pick $(read_display))"
  for t in "3840 1620 120 0" "1728 1117 120 37" "1512 982 120 32" "1920 1080 60 0" "2560 1440 60 0" "3840 2160 60 0" "1470 956 60 32" "5120 2160 75 0"; do
    echo "  display $t  ->  $(pick $t)"
  done
  exit 0
fi

RES=""; FPS=""; INSET=""; HZ=""; VALIGN=""
D=$(read_display)
if [[ "$D" =~ ^[0-9]+\ [0-9]+\ [0-9]+\ [0-9]+$ ]]; then
  read -r dw dh dhz dinset <<< "$D"
  if [ "$dw" -ge 640 ] && [ "$dh" -ge 480 ]; then
    read -r rw rh FPS INSET <<< "$(pick "$dw" "$dh" "$dhz" "$dinset")"
    RES="$rw $rh"; HZ="$dhz"
    say "display ${dw}x${dh} @${dhz} Hz, top inset $dinset -> resolution $RES, fps $FPS"
  fi
else
  say "could not read the display ('$D'); leaving resolution and fps as they are"
fi
[ -f "$T/display-override.conf" ] && { . "$T/display-override.conf"; say "display-override.conf: RES='$RES' FPS='$FPS' INSET='$INSET'"; }

mkdir -p "$OPTDIR" 2>/dev/null
[ -f "$OPTDIR/Options.ini" ] || { [ -f "$T/options-template.ini" ] && cp "$T/options-template.ini" "$OPTDIR/Options.ini" && say "created Options.ini from the template"; }
if [ -n "$RES" ] && [ -f "$OPTDIR/Options.ini" ] && [ "$(get_opt Resolution "$OPTDIR/Options.ini")" != "$RES" ]; then
  set_opt Resolution "$RES" "$OPTDIR/Options.ini" && say "Options.ini: Resolution = $RES"
fi
# LAN play (Multiplayer > Network): the lobby uses the address in "IPAddress"; keep it on this Mac's current LAN
# address (the one on the default route). "GameSpyIPAddress" is the separate setting for online play; left alone.
lanif=$(/sbin/route -n get default 2>/dev/null | /usr/bin/awk '/interface:/{print $2}')
lanip=""; [ -n "$lanif" ] && lanip=$(/usr/sbin/ipconfig getifaddr "$lanif" 2>/dev/null)
if [[ "$lanip" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] && [ -f "$OPTDIR/Options.ini" ] && [ "$(get_opt IPAddress "$OPTDIR/Options.ini")" != "$lanip" ]; then
  set_opt IPAddress "$lanip" "$OPTDIR/Options.ini" && say "Options.ini: IPAddress = $lanip ($lanif)"
fi

# ---- our files in the game folder ---------------------------------------------------------------------------
TMP=$(mktemp -d -t bfme2) || TMP=""
put "$T/letterbox/d3d9.dll" d3d9.dll
if [ -n "$TMP" ] && [ -f "$T/letterbox/letterbox.ini" ]; then
  grep -v -E '^(topinset|refresh|valign|gamefps)=' "$T/letterbox/letterbox.ini" > "$TMP/letterbox.ini"
  gfps=30; [ -e "$T/sage-unlocked/disabled" ] || gfps="${FPS:-30}"
  printf 'topinset=%s\r\nrefresh=%s\r\nvalign=%s\r\ngamefps=%s\r\n' "${INSET:-0}" "${HZ:-0}" "$([ "${VALIGN:-}" = top ] && echo 1 || echo 0)" "$gfps" >> "$TMP/letterbox.ini"
  put "$TMP/letterbox.ini" letterbox.ini
else
  put "$T/letterbox/letterbox.ini" letterbox.ini
fi
# 16:9 art (widescreen-art/make-art.py builds these from the game's own files): the main-menu logo squeezed so it is
# not stretched, and the full-screen loading/title pictures fitted to the picture's shape. Remove widescreen-art/out
# to go without.
for f in "$T/widescreen-art/out/!widescreen-art.big"; do [ -f "$f" ] && put "$f" "$(basename "$f")"; done
for f in "$T/widescreen-art/out/lang"/*.big; do [ -f "$f" ] && put "$f" "lang/$(basename "$f")"; done
# SAGE Unlocked (frame-rate unlock). An empty file sage-unlocked/disabled switches it off for A/B tests.
if [ -e "$T/sage-unlocked/disabled" ]; then
  for f in dinput8.dll CnCFpsUnlocker.dll SAGEUnlocked.ini; do [ -e "$GAME/$f" ] && rm -f "$GAME/$f" && say "removed $f (SAGE Unlocked disabled)"; done
elif [ -d "$T/sage-unlocked/files" ]; then
  for f in "$T/sage-unlocked/files"/*; do
    n=$(basename "$f")
    if [ "$n" = SAGEUnlocked.ini ] && [ -n "$FPS" ] && [ -n "$TMP" ]; then
      grep -v '^fps=' "$f" > "$TMP/$n"; printf 'fps=%s\r\n' "$FPS" >> "$TMP/$n"; put "$TMP/$n" "$n"
    else
      put "$f" "$n"
    fi
  done
fi
[ -n "$TMP" ] && [ -d "$TMP" ] && rm -rf "${TMP:?}"

# ---- interlock ----------------------------------------------------------------------------------------------
ok=1
cmp -s "$T/letterbox/d3d9.dll" "$GAME/d3d9.dll" || { ok=0; say "letterbox d3d9.dll is not in the game folder ($GAME)"; }
/usr/bin/awk '/^\[Software\\\\Wine\\\\AppDefaults\\\\game.dat\\\\DllOverrides\]/{f=1;next} /^\[/{f=0} f && /^"d3d9"="native,builtin"/{found=1} END{exit !found}' "$REG" 2>/dev/null \
  || { ok=0; say "no d3d9=native,builtin override for game.dat in $REG"; }
grep -q '"EmulateModeset"="y"' "$REG" 2>/dev/null && { ok=0; say "EmulateModeset is on in $REG (deadlocks under FEX)"; }

if [ "$ok" = 1 ]; then
  say "guard ok, starting: $*"
  exec "$@"
fi
args=()
for a in "$@"; do
  case "$a" in */lotrbfme2.exe) args+=("$HARMLESS") ;; *) args+=("$a") ;; esac
done
say "guard FAILED -- running winver instead of the game"
exec "${args[@]}"
