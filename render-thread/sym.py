# sym.py <hex offsets inside renderthread.dll ...>: names the function each offset is in (symbols from rt_port.obj)
import subprocess, sys, bisect, re
out = subprocess.run(['nm', '-n', 'rt_port.obj'], capture_output=True, text=True).stdout
syms = []
for l in out.splitlines():
    m = re.match(r'([0-9a-f]+) [tT] (.+)', l)
    if m: syms.append((int(m.group(1), 16), m.group(2)))
syms.sort(); keys = [s[0] for s in syms]
def name(off):
    o = off - 0x1000
    i = bisect.bisect_right(keys, o) - 1
    if i < 0 or o > 229935: return '?'
    n = syms[i][1]
    try: n = subprocess.run(['c++filt', '-n', n.lstrip('_') if n.startswith('__Z') else n], capture_output=True, text=True).stdout.strip() or n
    except Exception: pass
    return '%s+%x' % (n[:70], o - syms[i][0])
if __name__ == '__main__':
    for a in sys.argv[1:]: print(a, '->', name(int(a, 16)))
