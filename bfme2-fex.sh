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
#    and written out: Resolution in the game's Options.ini (the display's own shape when that lies between 4:3 and
#    16:9, so a MacBook's 16:10 is used in full; 16:9 with black sides on anything wider), fps= for SAGE Unlocked
#    (refresh rate rounded down to a multiple of 15, 30..120), topinset= for the proxy (a MacBook's camera housing).
#    The art and HUD overrides exist once per picture shape (widescreen-art/out/<shape>/, built by make-art.py; a
#    missing one is built on the spot when uv is installed) and the set for the current shape is installed. Without a
#    set for a shape other than 16:9 the picture is 16:9 (the patch's own HUD is right only there). So the same install
#    follows whatever screen is connected. Also IPAddress (LAN lobby) is kept on this Mac's current LAN address.
#    To pin values instead, create display-override.conf next to this file with any of:
#        RES="1920 1080"     FPS=60     INSET=0     VALIGN=top   (picture right under the notch instead of centred)
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"          # this folder
LOG="$T/bfme2-fex.log"
PFX="${STEAM_COMPAT_DATA_PATH:-/nonexistent}/pfx"
REG="$PFX/user.reg"
say() { echo "$(date '+%F %T') $*" >> "$LOG"; }

# The game folder is wherever the lotrbfme2.exe that Steam is starting lives.
GAME=""
for a in "$@"; do case "$a" in */lotrbfme2.exe) GAME="${a%/*}" ;; esac; done
if [ -z "$GAME" ] || [ ! -d "$GAME" ]; then                   # not in the command: BFME_GAME from local.conf (see env.sh)
  [ -f "$T/local.conf" ] && . "$T/local.conf"; GAME="${BFME_GAME:-}"
fi
[ "${1:-}" = "--selftest" ] || [ -d "$GAME" ] || { say "no game folder (expected .../lotrbfme2.exe in the launch command); not starting"; exit 1; }
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
  # The usable area itself when its shape lies between 4:3 and 16:9; 16:9 inside it when it is wider. (The HUD has to
  # match the shape: see "art set" below. The proxy adds exactly this size to the game's mode list.)
  rw=$((ah * 16 / 9)); [ "$rw" -lt "$w" ] || rw=$w
  rw=$((rw & ~1)); rh=$((ah & ~1))
  [ $((rw * 3)) -ge $((rh * 4)) ] || rh=$(( (rw * 3 / 4) & ~1 ))
  fps=$((hz / 15 * 15)); [ "$fps" -ge 30 ] || fps=30; [ "$fps" -le 120 ] || fps=120
  echo "$rw $rh $fps $inset"
}
shape_key() {  # shape_key <width> <height>  ->  the name of the art set for that shape: 16x9, or a<width*1000/height>
  local k=$(( $1 * 1000 / $2 ))
  if [ "$k" -ge 1765 ] && [ "$k" -le 1790 ]; then echo 16x9; else echo "a$k"; fi
}
set_opt() {  # set_opt <key> <value> <Options.ini>   (the file has CRLF line ends)
  local cr=$'\r'
  if grep -q "^$1 = " "$3" 2>/dev/null; then /usr/bin/sed -i '' -E "s/^$1 = [^$cr]*/$1 = $2/" "$3"
  else printf '%s = %s\r\n' "$1" "$2" >> "$3"; fi
}
get_opt() { /usr/bin/sed -n "s/^$1 = //p" "$2" 2>/dev/null | tr -d '\r' | head -1; }

if [ "${1:-}" = "--selftest" ]; then   # bfme2-fex.sh --selftest : show what would be chosen, change nothing
  echo "this display: $(read_display)  ->  $(pick $(read_display))"
  for t in "3840 1620 120 0" "1728 1117 120 32" "1512 982 120 32" "1920 1080 60 0" "2560 1440 60 0" "3840 2160 60 0" "1920 1200 60 0" "1470 956 60 32" "5120 2160 75 0" "1280 1024 60 0"; do
    read -r a b c d <<< "$(pick $t)"; k=$(shape_key "$a" "$b")
    echo "  display $t  ->  $a $b at $c fps, inset $d, art set $k$([ -f "$T/widescreen-art/out/$k/!widescreen-art.big" ] && echo "" || echo " (not built)")"
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

# ---- art set: the overrides built for this picture shape ------------------------------------------------------
ART=""
if [ -n "$RES" ] && [ ! -e "$T/widescreen-art/disabled" ]; then
  read -r rw rh <<< "$RES"
  KEY=$(shape_key "$rw" "$rh"); ART="$T/widescreen-art/out/$KEY"
  if [ ! -f "$ART/!widescreen-art.big" ] && [ "$KEY" != 16x9 ]; then     # a set built for almost the same shape will do
    k=${KEY#a}                                                           # (a MacBook's scaled modes differ by a few 1/1000)
    for d in "$T/widescreen-art/out"/a[0-9]*/; do
      n=$(basename "$d"); n=${n#a}
      case "$n" in *[!0-9]*|"") continue ;; esac
      if [ -f "${d}!widescreen-art.big" ] && [ $((n - k)) -ge -12 ] && [ $((n - k)) -le 12 ]; then ART="${d%/}"; KEY="a$n"; break; fi
    done
  fi
  if [ ! -f "$ART/!widescreen-art.big" ]; then
    UV=$(command -v uv 2>/dev/null) || UV=""
    for c in "$HOME/.local/bin/uv" /opt/homebrew/bin/uv /usr/local/bin/uv; do [ -n "$UV" ] || { [ -x "$c" ] && UV="$c"; }; done
    if [ -n "$UV" ]; then
      say "building the art set $KEY for ${rw}x${rh}"
      "$UV" run -q --with pillow python "$T/widescreen-art/make-art.py" "$GAME" "$ART" "$rw/$rh" >> "$LOG" 2>&1
    fi
  fi
  if [ ! -f "$ART/!widescreen-art.big" ] && [ "$KEY" != 16x9 ]; then     # no HUD for this shape: 16:9 inside it
    rh=$(( (rw * 9 / 16) & ~1 )); RES="$rw $rh"; ART="$T/widescreen-art/out/16x9"
    say "no art set $KEY (run widescreen-art/make-art.py); using 16:9 instead: resolution $RES"
  fi
fi

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
  grep -v -E '^(topinset|refresh|valign|gamefps|gamew|gameh)=' "$T/letterbox/letterbox.ini" > "$TMP/letterbox.ini"
  gfps=30; [ -e "$T/sage-unlocked/disabled" ] || gfps="${FPS:-30}"
  printf 'topinset=%s\r\nrefresh=%s\r\nvalign=%s\r\ngamefps=%s\r\ngamew=%s\r\ngameh=%s\r\n' "${INSET:-0}" "${HZ:-0}" "$([ "${VALIGN:-}" = top ] && echo 1 || echo 0)" "$gfps" "${RES% *}" "${RES#* }" >> "$TMP/letterbox.ini"
  put "$TMP/letterbox.ini" letterbox.ini
else
  put "$T/letterbox/letterbox.ini" letterbox.ini
fi
# Art and HUD for this picture shape (see "art set" above): the main-menu logo squeezed so it is not stretched, the
# loading and title pictures fitted to the shape, the in-game HUD for shapes other than 16:9, the enhanced pre-start
# splash. With no set for the shape, ours are taken out again so that nothing made for another shape stays behind.
# An empty file widescreen-art/disabled goes without all of it.
if [ -n "$ART" ] && [ -f "$ART/!widescreen-art.big" ]; then
  put "$ART/!widescreen-art.big" "!widescreen-art.big"
  for f in "$ART/lang"/*.big; do [ -f "$f" ] && put "$f" "lang/$(basename "$f")"; done
  for f in "$ART/root"/*; do [ -f "$f" ] && put "$f" "$(basename "$f")"; done
else
  for f in "$GAME/!widescreen-art.big" "$GAME"/lang/*zz-widescreen-art.big; do
    [ -f "$f" ] && rm -f "$f" && say "removed $(basename "$f") (no art set for this picture shape)"
  done
fi
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
