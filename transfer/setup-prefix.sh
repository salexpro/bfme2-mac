#!/bin/bash
# Prepare a freshly created NotProton FEX prefix for BFME II (run on the Mac that will play, after the shortcut's
# first launch has created the prefix; that first launch only shows Wine's "About" box because of the interlock).
#   setup-prefix.sh <appid> <ergc-key> [game-folder]
# Does: stop the prefix's wineserver, import prefix-setup.reg, put Microsoft's d3dx9_27.dll (units are invisible with
# Wine's own), link C:\BFME2 to the game folder.
set -u
APPID="${1:?appid}"; KEY="${2:?ergc key}"
HERE="$(cd "$(dirname "$0")" && pwd)"
GAME="${3:-$HOME/Library/Application Support/Steam/steamapps/common/BFME2}"
PFX="$HOME/Library/Application Support/Steam/steamapps/compatdata/$APPID/pfx"
R="$HOME/Library/Application Support/notproton/runners/crossover-27.0.0.41073-fex/CrossOver"
W="$R/lib/wine/aarch64-unix/wine.app/Contents/MacOS/wine"
[ -f "$PFX/system.reg" ] || { echo "no prefix at $PFX yet (launch the shortcut once first)"; exit 1; }
[ -f "$GAME/game.dat" ] || { echo "no game at $GAME"; exit 1; }
[ -x "$W" ] || { echo "runner not found: $W"; exit 1; }
export WINEPREFIX="$PFX" WINESERVER="$R/bin/wineserver-arm64" WINELOADER="$W" CX_ROOT="$R" WINEDEBUG=-all
"$WINESERVER" -k 2>/dev/null; sleep 2
TMPREG="$(mktemp -t bfme2reg).reg"
sed "s/@ERGC@/$KEY/" "$HERE/prefix-setup.reg" > "$TMPREG"
cp -p "$PFX/user.reg" "$PFX/user.reg.before-bfme2" 2>/dev/null; cp -p "$PFX/system.reg" "$PFX/system.reg.before-bfme2" 2>/dev/null
"$W" regedit /S "$TMPREG"; rc=$?
"$WINESERVER" -w 2>/dev/null; "$WINESERVER" -k 2>/dev/null
rm -f "$TMPREG"
SYS="$PFX/drive_c/windows/syswow64"
if ! cmp -s "$HERE/d3dx9_27.dll" "$SYS/d3dx9_27.dll"; then
  [ -e "$SYS/d3dx9_27.dll.wine-builtin" ] || cp -p "$SYS/d3dx9_27.dll" "$SYS/d3dx9_27.dll.wine-builtin"
  cp "$HERE/d3dx9_27.dll" "$SYS/d3dx9_27.dll"
fi
[ -L "$PFX/drive_c/BFME2" ] && rm -f "$PFX/drive_c/BFME2"
[ -e "$PFX/drive_c/BFME2" ] || ln -s "$GAME" "$PFX/drive_c/BFME2"
echo "regedit exit $rc"
grep -c '"d3dx9_27"="native,builtin"' "$PFX/user.reg" | sed 's/^/d3dx9_27 override lines: /'
grep -c 'The Battle for Middle-earth II\\\\ergc' "$PFX/system.reg" | sed 's/^/ergc key sections: /'
ls -la "$PFX/drive_c/BFME2" | sed 's/.* BFME2/C:\\BFME2/'
