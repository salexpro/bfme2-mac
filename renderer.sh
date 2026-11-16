#!/bin/bash
# Test aid: choose which renderer Wine's Direct3D uses for game.dat in the game's prefix.
#   renderer.sh vulkan | gl | default      (default = remove the setting)
# Edits <prefix>/user.reg directly, so the game and its wine server must not be running. The first run keeps a
# copy of the file as user.reg.before-renderer-tests.
. "$(dirname "$0")/env.sh"; need_steam
REG="$CD/pfx/user.reg"
[ -f "$REG" ] || { echo "no $REG"; exit 1; }
pgrep -x game.dat >/dev/null && { echo "the game is running; close it first"; exit 1; }
pgrep -f "wineserver" >/dev/null && { echo "a wine server is still running; wait for it to exit"; exit 1; }
[ -f "$REG.before-renderer-tests" ] || cp "$REG" "$REG.before-renderer-tests"
/usr/bin/python3 - "$REG" "${1:-default}" <<'EOF'
import sys, re, time
path, mode = sys.argv[1], sys.argv[2]
t = open(path, encoding='latin1').read()
key = r'[Software\\Wine\\AppDefaults\\game.dat\\Direct3D]'
t = re.sub(r'\n\[Software\\\\Wine\\\\AppDefaults\\\\game\.dat\\\\Direct3D\][^\n]*\n(?:[^\[\n][^\n]*\n|\n)*', '\n', t)
if mode in ('vulkan', 'gl'):
    anchor = r'[Software\\Wine\\AppDefaults\\game.dat\\DllOverrides]'
    i = t.find('\n' + anchor)
    if i < 0: sys.exit('no DllOverrides section for game.dat in the registry file')
    stamp = int(time.time())
    filetime = (stamp + 11644473600) * 10000000
    t = t[:i + 1] + '%s %d\n#time=%x\n"renderer"="%s"\n\n' % (key, stamp, filetime, mode) + t[i + 1:]
open(path, 'w', encoding='latin1').write(t)
print('renderer for game.dat:', mode)
EOF
