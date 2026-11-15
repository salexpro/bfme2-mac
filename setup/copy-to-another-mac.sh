#!/bin/bash
# Run on a Mac that is already set up: copy BFME II and this whole folder to another Mac over SSH and register the game
# in that Mac's Steam as a non-Steam shortcut on the NotProton FEX runner.
#   copy-to-another-mac.sh <user@host> <ergc-key> files      copy the game (6.3 GB) and the tools
#   copy-to-another-mac.sh <user@host> <ergc-key> steam      add the Steam shortcut + compatibility tool + artwork (quits and restarts Steam there)
#   copy-to-another-mac.sh <user@host> <ergc-key> prefix     first launch (creates the prefix; shows Wine's About box), then set the prefix up
# The other Mac needs: Steam signed in, NotProton with the notproton-fex-41073 tool, Remote Login with this Mac's key.
# Settings (environment or local.conf in this folder's parent, see env.sh): BFME_GAME = the game folder on this Mac;
# optional BFME_APPID (the shortcut's appid, any id Steam does not use yet) and BFME_ICON (a .png for the shortcut).
set -u
DEST="${1:?user@host}"; KEY="${2:?ergc key}"; STAGE="${3:?files|steam|prefix}"
TOOLS="$(cd "$(dirname "$0")/.." && pwd)"                       # this repository
[ -f "$TOOLS/local.conf" ] && . "$TOOLS/local.conf"
SRC_GAME="${BFME_GAME:?set BFME_GAME (the game folder on this Mac) in local.conf}"
APPID="${BFME_APPID:-2596813934}"
GAMEID=$(printf '%u' $(( (APPID << 32) | 0x02000000 )))          # Steam's game id of a non-Steam shortcut
ICON="${BFME_ICON:-}"
S='ssh -o BatchMode=yes -o ConnectTimeout=10'
RHOME=$($S "$DEST" 'printf %s "$HOME"') || { echo "cannot reach $DEST"; exit 1; }
RT="$RHOME${TOOLS#"$HOME"}"                                     # the same place under the other Mac's home
RGAME="$RHOME/Library/Application Support/Steam/steamapps/common/BFME2"
RSTEAM="$RHOME/Library/Application Support/Steam"
q() { printf "'%s'" "$(printf %s "$1" | sed "s/'/'\\\\''/g")"; }   # quote for the remote shell

case "$STAGE" in
files)
  $S "$DEST" "mkdir -p $(q "$RGAME") $(q "$RT") && ls $(q "$RHOME/Library/Application Support/Steam/compatibilitytools.d") | grep -c notproton-fex-41073" | sed 's/^/notproton-fex-41073 tool present: /'
  echo "copying the game ..."
  tar -C "$SRC_GAME" --exclude './letterbox.log' --exclude './letterbox.cmd' --exclude './SAGEUnlocked.log' --exclude './*.bmp' -cf - . | $S "$DEST" "tar -C $(q "$RGAME") -xf -" || exit 1
  echo "copying the tools ..."
  tar -C "$TOOLS" --exclude './.git' --exclude './shots' --exclude '*.bak*' --exclude '*.log' --exclude './setup/ergc-*' --exclude './.last-launch' \
      --exclude './display-override.conf' --exclude './local.conf' --exclude './widescreen-art/custom' \
      --exclude './sage-unlocked/source-fexfix' --exclude './sage-unlocked/source-fexfix2' --exclude './sage-unlocked/source-fexfix3' \
      -cf - . | $S "$DEST" "tar -C $(q "$RT") -xf -" || exit 1
  [ -f "$ICON" ] && cat "$ICON" | $S "$DEST" "cat > $(q "$RT/shortcut-icon.png")"
  # the art and HUD set for the other Mac's display, built here (it may have no uv) and sent along
  read -r rw rh _ <<< "$($S "$DEST" "$(q "$RT/bfme2-fex.sh") --selftest" | sed -n '1s/.*->  *//p')"
  if [ -n "${rh:-}" ] && [ "$rh" -gt 0 ] 2>/dev/null; then
    k=$(( rw * 1000 / rh )); if [ "$k" -ge 1765 ] && [ "$k" -le 1790 ]; then key=16x9; else key="a$k"; fi
    [ -f "$TOOLS/widescreen-art/out/$key/!widescreen-art.big" ] || uv run -q --with pillow python "$TOOLS/widescreen-art/make-art.py" "$SRC_GAME" "$TOOLS/widescreen-art/out/$key" "$rw/$rh"
    [ -d "$TOOLS/widescreen-art/out/$key" ] && tar -C "$TOOLS" -cf - "widescreen-art/out/$key" | $S "$DEST" "tar -C $(q "$RT") -xf -" && echo "art set $key (${rw}x${rh}) sent"
  fi
  printf 'BFME_GAME="%s"\nBFME_COMPAT="%s"\nBFME_URL="steam://rungameid/%s"\n' "$RGAME" "$RSTEAM/steamapps/compatdata/$APPID" "$GAMEID" \
    | $S "$DEST" "cat > $(q "$RT/local.conf")"                  # settings for the test scripts over there
  $S "$DEST" "du -sh $(q "$RGAME") | cut -f1; ls $(q "$RT"); xattr -dr com.apple.quarantine $(q "$RT") 2>/dev/null; true"
  ;;
steam)
  UD=$($S "$DEST" "ls -d $(q "$RSTEAM")/userdata/*/config/shortcuts.vdf 2>/dev/null | head -5")
  [ "$(printf '%s\n' "$UD" | grep -c .)" = 1 ] || { echo "expected exactly one shortcuts.vdf on the target, found: $UD"; exit 1; }
  CFG="${UD%/shortcuts.vdf}"
  $S "$DEST" 'pgrep -fl "wine-preloader|wineserver|\.exe" | grep -v grep | head -3' | grep -q . && { echo "a Windows game seems to be running on the target; not touching Steam"; exit 1; }
  echo "quitting Steam on the target ..."
  $S "$DEST" 'open "steam://exit" 2>/dev/null; for i in $(seq 1 40); do pgrep -x steam_osx >/dev/null || break; sleep 1; done; pgrep -x steam_osx >/dev/null && echo STILL-RUNNING'
  W=$(mktemp -d -t bfme2push) || exit 1
  scp -q "$DEST:$(printf %q "$UD")" "$W/shortcuts.vdf" || scp -q "$DEST:\"$UD\"" "$W/shortcuts.vdf" || exit 1
  scp -q "$DEST:$(printf %q "$RSTEAM/config/config.vdf")" "$W/config.vdf" || scp -q "$DEST:\"$RSTEAM/config/config.vdf\"" "$W/config.vdf" || exit 1
  cp "$W/shortcuts.vdf" "$W/shortcuts.vdf.orig"; cp "$W/config.vdf" "$W/config.vdf.orig"
  uv run -q --with vdf python -I - "$W" "$RHOME" "$APPID" "$RT" "$([ -f "$ICON" ] && echo 1 || echo 0)" <<'PY' || exit 1
import sys, vdf, re, struct
w, rhome, appid, rt, icon = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5] == '1'
signed = struct.unpack('<i', struct.pack('<I', appid))[0]
p = w + '/shortcuts.vdf'
d = vdf.binary_load(open(p, 'rb'))
sc = d.setdefault('shortcuts', {})
game = rhome + '/Library/Application Support/Steam/steamapps/common/BFME2'
entry = {'appid': signed, 'AppName': 'The Battle for Middle-earth II', 'Exe': '"%s/lotrbfme2.exe"' % game, 'StartDir': '"%s/"' % game,
         'icon': rt + '/shortcut-icon.png' if icon else '', 'ShortcutPath': '',
         'LaunchOptions': '"%s/bfme2-fex.sh" %%command%%' % rt,
         'IsHidden': 0, 'AllowDesktopConfig': 1, 'AllowOverlay': 1, 'OpenVR': 0, 'Devkit': 0, 'DevkitGameID': '', 'DevkitOverrideAppID': 0,
         'LastPlayTime': 0, 'FlatpakAppID': '', 'sortas': '', 'tags': {}}
key = next((k for k, e in sc.items() if (e.get('appid', 0) & 0xffffffff) == appid), None)
if key is None: key = str(max([int(k) for k in sc] + [-1]) + 1)
sc[key] = entry
open(p, 'wb').write(vdf.binary_dumps(d))
print('shortcut entry', key, 'of', len(sc))
p = w + '/config.vdf'
s = open(p, encoding='utf-8', errors='surrogateescape').read()
m = re.search(r'"CompatToolMapping"\s*\{', s)
if not m: sys.exit('no CompatToolMapping block in config.vdf')
if re.search(r'"%d"\s*\{\s*"name"\s*"notproton-fex-41073"' % appid, s[m.end():m.end() + 20000]): print('compat tool mapping already there')
else:
    ind = '\t\t\t\t\t'
    ins = '\n%s"%d"\n%s{\n%s\t"name"\t\t"notproton-fex-41073"\n%s\t"config"\t\t""\n%s\t"priority"\t\t"250"\n%s}' % (ind, appid, ind, ind, ind, ind, ind)
    s = s[:m.end()] + ins + s[m.end():]
    open(p, 'w', encoding='utf-8', errors='surrogateescape').write(s); print('compat tool mapping added')
PY
  $S "$DEST" "cp -p $(q "$UD") $(q "$UD.bak-bfme2"); cp -p $(q "$RSTEAM/config/config.vdf") $(q "$RSTEAM/config/config.vdf.bak-bfme2"); mkdir -p $(q "$CFG/grid")"
  cat "$W/shortcuts.vdf" | $S "$DEST" "cat > $(q "$UD")" && cat "$W/config.vdf" | $S "$DEST" "cat > $(q "$RSTEAM/config/config.vdf")" || exit 1
  G=$(ls -d "$HOME/Library/Application Support/Steam/userdata"/*/config/grid | head -1)
  for f in "$G"/${APPID}*.png; do [ -f "$f" ] && cat "$f" | $S "$DEST" "cat > $(q "$CFG/grid/$(basename "$f")")"; done
  $S "$DEST" 'open -a Steam; sleep 8; pgrep -x steam_osx >/dev/null && echo "Steam restarted"'
  echo "working copies kept in $W"
  ;;
prefix)
  $S "$DEST" "open 'steam://rungameid/$GAMEID'; for i in \$(seq 1 120); do [ -f $(q "$RSTEAM/steamapps/compatdata/$APPID/pfx/system.reg") ] && pgrep -f winver >/dev/null && break; sleep 1; done; sleep 4; pkill -f winver; sleep 6; tail -3 $(q "$RT/bfme2-fex.log")"
  $S "$DEST" "$(q "$RT/setup/setup-prefix.sh") $APPID $(q "$KEY")"
  ;;
*) echo "stage?"; exit 2 ;;
esac
