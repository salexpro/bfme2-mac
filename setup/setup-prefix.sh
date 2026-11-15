#!/bin/bash
# Prepare the NotProton FEX prefix of the game's Steam shortcut. Run it once, after the shortcut's first launch has
# created the prefix (that first launch only shows Wine's "About" box: the launch wrapper refuses to start the game
# in an unprepared prefix).
#
#   setup-prefix.sh [--check] [appid|auto] [serial|auto] [game folder]
#
# With nothing given it takes
#   - the prefix and the game folder of the last launch (the wrapper notes them in .last-launch),
#   - the serial that the All in One Launcher wrote into the prefix it installed the game in,
#   - Microsoft's d3dx9_27.dll from a prefix that has the DirectX 9 runtime (the launcher installs it), unless a copy
#     lies next to this script. Units are invisible with Wine's own.
# Two Macs that are to play each other on a LAN need different serials: give one on the second Mac.
# Does: stop the prefix's wineserver, import prefix-setup.reg, install the DLL, link C:\BFME2 to the game folder.
# --check only says what it would use.
set -u
CHECK=0; [ "${1:-}" = "--check" ] && { CHECK=1; shift; }
HERE="$(cd "$(dirname "$0")" && pwd)"; TOOLS="$(cd "$HERE/.." && pwd)"
[ -f "$TOOLS/.last-launch" ] && . "$TOOLS/.last-launch"            # LAST_COMPAT, LAST_GAME
APPID="${1:-auto}"; KEY="${2:-auto}"; GAME="${3:-${LAST_GAME:-}}"
STEAMAPPS="$HOME/Library/Application Support/Steam/steamapps"
if [ "$APPID" = auto ]; then
  [ -n "${LAST_COMPAT:-}" ] || { echo "start the game from Steam once first, or give the shortcut's appid"; exit 1; }
  PFX="$LAST_COMPAT/pfx"
else PFX="$STEAMAPPS/compatdata/$APPID/pfx"; fi
[ -n "$GAME" ] || GAME="$STEAMAPPS/common/BFME2"
R="${BFME_RUNNER:-$HOME/Library/Application Support/notproton/runners/crossover-27.0.0.41073-fex/CrossOver}"   # NotProton's FEX runner
W="$R/lib/wine/aarch64-unix/wine.app/Contents/MacOS/wine"
[ -f "$PFX/system.reg" ] || { echo "no prefix at $PFX yet (launch the shortcut once first)"; exit 1; }
[ -f "$GAME/game.dat" ] || { echo "no game at $GAME"; exit 1; }
[ -x "$W" ] || { echo "runner not found: $W"; exit 1; }

# other prefixes to look in: the one the game was installed into first, then every Steam prefix and CrossOver bottle
others=(); case "$GAME" in */drive_c/*) others+=("${GAME%%/drive_c/*}") ;; esac
for p in "$STEAMAPPS/compatdata"/*/pfx "$HOME/Library/Application Support/CrossOver/Bottles"/*; do [ -d "$p" ] && [ "$p" != "$PFX" ] && others+=("$p"); done
others+=("$PFX")
ergc_of() {
  /usr/bin/awk '/^\[Software\\\\Wow6432Node\\\\Electronic Arts\\\\Electronic Arts\\\\The Battle for Middle-earth II\\\\ergc\]/{f=1;next} /^\[/{f=0}
                f && /^@="/{v=$0; sub(/^@="/,"",v); sub(/".*$/,"",v); print v; exit}' "$1/system.reg" 2>/dev/null
}
is_ms() { [ -f "$1" ] && [ "$(wc -c < "$1")" -gt 1000000 ] && ! LC_ALL=C grep -q -a "Wine builtin DLL" "$1"; }
if [ "$KEY" = auto ]; then
  KEY=""
  for p in "${others[@]}"; do KEY=$(ergc_of "$p"); [ -n "$KEY" ] && { echo "serial: taken from $p"; break; }; done
  [ -n "$KEY" ] || { echo "no serial found in any prefix (install the game with the launcher first, or give one)"; exit 1; }
fi
DX="$HERE/d3dx9_27.dll"
if ! is_ms "$DX"; then
  DX=""
  for p in "${others[@]}"; do f="$p/drive_c/windows/syswow64/d3dx9_27.dll"; is_ms "$f" && { DX="$f"; break; }; done
  [ -n "$DX" ] || { echo "Microsoft's d3dx9_27.dll not found: let the launcher install the DirectX 9 runtime, or put the DLL next to this script"; exit 1; }
fi
echo "prefix: $PFX"; echo "game:   $GAME"; echo "d3dx9_27.dll: $DX"
[ "$CHECK" = 1 ] && exit 0

export WINEPREFIX="$PFX" WINESERVER="$R/bin/wineserver-arm64" WINELOADER="$W" CX_ROOT="$R" WINEDEBUG=-all
"$WINESERVER" -k 2>/dev/null; sleep 2
TMPREG="$(mktemp -t bfme2reg).reg"
sed "s/@ERGC@/$KEY/" "$HERE/prefix-setup.reg" > "$TMPREG"
for f in user.reg system.reg; do [ -e "$PFX/$f.before-bfme2" ] || cp -p "$PFX/$f" "$PFX/$f.before-bfme2" 2>/dev/null; done
"$W" regedit /S "$TMPREG"; rc=$?
"$WINESERVER" -w 2>/dev/null; "$WINESERVER" -k 2>/dev/null
rm -f "$TMPREG"
SYS="$PFX/drive_c/windows/syswow64"
if ! cmp -s "$DX" "$SYS/d3dx9_27.dll"; then
  [ -e "$SYS/d3dx9_27.dll.wine-builtin" ] || cp -p "$SYS/d3dx9_27.dll" "$SYS/d3dx9_27.dll.wine-builtin"
  cp "$DX" "$SYS/d3dx9_27.dll"
fi
[ -L "$PFX/drive_c/BFME2" ] && rm -f "$PFX/drive_c/BFME2"
[ -e "$PFX/drive_c/BFME2" ] || ln -s "$GAME" "$PFX/drive_c/BFME2"
echo "regedit exit $rc"
grep -c '"d3dx9_27"="native,builtin"' "$PFX/user.reg" | sed 's/^/d3dx9_27 override lines: /'
grep -c 'The Battle for Middle-earth II\\\\ergc' "$PFX/system.reg" | sed 's/^/ergc key sections: /'
ls -la "$PFX/drive_c/BFME2" | sed 's/.* BFME2/C:\\BFME2/'
