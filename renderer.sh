#!/bin/bash
# Test aid: Wine Direct3D settings for game.dat in the game's prefix.
#   renderer.sh vulkan | gl | default      which renderer Wine's Direct3D uses (default = remove the setting)
#   renderer.sh csmt 0 | 1 | default       0 = no command-stream helper thread (all OpenGL work on the game's thread)
#   renderer.sh show                       print the section as it is
# Edits <prefix>/user.reg directly, so the game and its wine server must not be running. The first run keeps a
# copy of the file as user.reg.before-renderer-tests. Other values in the section are kept.
. "$(dirname "$0")/env.sh"; need_steam
REG="$CD/pfx/user.reg"
[ -f "$REG" ] || { echo "no $REG"; exit 1; }
if [ "${1:-}" != show ]; then
  pgrep -x game.dat >/dev/null && { echo "the game is running; close it first"; exit 1; }
  pgrep -f "wineserver" >/dev/null && { echo "a wine server is still running; wait for it to exit"; exit 1; }
  [ -f "$REG.before-renderer-tests" ] || cp "$REG" "$REG.before-renderer-tests"
fi
/usr/bin/python3 - "$REG" "${1:-default}" "${2:-}" <<'PYEOF'
import sys, re, time
path, what, arg = sys.argv[1], sys.argv[2], sys.argv[3]
t = open(path, encoding='latin1').read()
head = r'[Software\\Wine\\AppDefaults\\game.dat\\Direct3D]'
# the section: its header line, then every line up to the next line that starts with "[" (or the end of the file)
m = re.search(r'(?m)^\[Software\\\\Wine\\\\AppDefaults\\\\game\.dat\\\\Direct3D\][^\n]*\n((?:(?!\[)[^\n]*\n)*)', t)
vals = {}
if m:
    for line in m.group(1).splitlines():
        k = re.match(r'"([^"]+)"=(.*)$', line)
        if k: vals[k.group(1)] = k.group(2)
if what == 'show':
    print('game.dat Direct3D settings:', vals or 'none'); sys.exit(0)
if what in ('vulkan', 'gl'): vals['renderer'] = '"%s"' % what
elif what == 'default': vals.pop('renderer', None)
elif what == 'csmt' and arg in ('0', '1'): vals['csmt'] = 'dword:%08x' % int(arg)
elif what == 'csmt' and arg == 'default': vals.pop('csmt', None)
else: sys.exit('usage: renderer.sh vulkan|gl|default | csmt 0|1|default | show')
if m: t = t[:m.start()] + t[m.end():]            # drop the old section
if vals:
    anchor = r'[Software\\Wine\\AppDefaults\\game.dat\\DllOverrides]'
    i = t.find('\n' + anchor)
    if i < 0: sys.exit('no DllOverrides section for game.dat in the registry file')
    stamp = int(time.time()); filetime = (stamp + 11644473600) * 10000000
    body = ''.join('"%s"=%s\n' % (k, v) for k, v in sorted(vals.items()))
    t = t[:i + 1] + '%s %d\n#time=%x\n%s\n' % (head, stamp, filetime, body) + t[i + 1:]
open(path, 'w', encoding='latin1').write(t)
print('game.dat Direct3D settings now:', vals or 'none')
PYEOF
