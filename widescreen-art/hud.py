"""The in-game HUD for any picture shape between 4:3 and 16:9.

The game's HUD files (.apt, a Flash-like format) are laid out for 4:3 and stretched over the picture. Patch 1.09 ships
copies for 16:9 (in #bt2dc-v1.09w.big): the same records as the originals, with x scales and x positions multiplied by
0.75 (some elements anchored to the right edge or re-spaced). Both are fixed sets, so at any other shape (16:10 on a
MacBook) the minimap is an oval. Every number the patch changed is a straight line in the squeeze factor
s = (4/3) / (width/height): 1 at 4:3 (original value), 0.75 at 16:9 (patch value). This module writes the files for any s
by moving each changed number along that line; records the patch restructured are left as the patch has them.
"""
import re, struct

FILES = ('Palantir', 'InGameHeroSelect', 'libInGameUI', 'SpellStore')

class Apt:
    """just enough of the format: the movie, its characters, their frames, PlaceObject items and button records"""
    def __init__(self, apt, const):
        if const[:17] != b'Apt constant file': raise ValueError('not an apt const file')
        self.b = apt; self.movie = struct.unpack_from('<I', const, 20)[0]
        if self.u(self.movie) != 9: raise ValueError('no movie where the const file says')
    def u(self, o): return struct.unpack_from('<I', self.b, o)[0]
    def i(self, o): return struct.unpack_from('<i', self.b, o)[0]
    def f(self, o, n): return struct.unpack_from('<%df' % n, self.b, o)
    def s(self, o): return self.b[o:self.b.index(b'\0', o)].decode('latin1') if o else ''
    def characters(self):
        m = self.movie; yield m, 9
        n, ptr = self.u(m + 20), self.u(m + 24)
        for k in range(1, n):
            c = self.u(ptr + 4 * k)
            if c: yield c, self.u(c)
    def matrices(self):
        """every placement matrix (a b c d tx ty): (key that must agree between two versions of a file, file offset)"""
        for c, t in self.characters():
            if t in (5, 9):                                   # sprite, movie: frames of items
                fc, fp = self.u(c + 8), self.u(c + 12)
                for k in range(fc):
                    n, items = self.u(fp + 8 * k), self.u(fp + 8 * k + 4)
                    for j in range(n):
                        it = self.u(items + 4 * j)
                        if self.u(it) == 3:                   # PlaceObject: flags, depth, character, matrix, ...
                            yield ('place', self.u(it + 4), self.i(it + 8), self.i(it + 12), self.s(self.u(it + 52))), it + 16
            elif t == 4:                                      # button: its records carry a matrix each
                n, recs = self.u(c + 44), self.u(c + 48)
                for k in range(n):
                    r = recs + 68 * k
                    yield ('button', self.u(r), self.u(r + 4), self.i(r + 8)), r + 12

def squeeze(aspect): return (4.0 / 3.0) / aspect

def apt_for(orig_apt, orig_const, wide_apt, aspect):
    """the patch's file with every number it changed moved to where it belongs for this shape; (bytes, numbers moved, records left alone)"""
    t = (1.0 - squeeze(aspect)) / 0.25                        # 0 = 4:3 (original), 1 = 16:9 (patch)
    o, w = Apt(orig_apt, orig_const), Apt(wide_apt, orig_const)
    mo, mw = list(o.matrices()), list(w.matrices())
    if len(mo) != len(mw): raise ValueError('the two versions do not have the same records')
    out = bytearray(wide_apt); moved = skipped = 0
    for (ko, oo), (kw, ow) in zip(mo, mw):
        if ko != kw: skipped += 1; continue
        a, b = o.f(oo, 6), w.f(ow, 6)
        for k in range(6):
            if abs(b[k] - a[k]) > 1e-3 * max(1.0, abs(a[k])):
                struct.pack_into('<f', out, ow + 4 * k, a[k] + (b[k] - a[k]) * t); moved += 1
    return bytes(out), moved, skipped

def ru_for(orig_ru, wide_ru, aspect):
    """a geometry file (text: 's' = style with a texture matrix, 't' = triangle) for this shape"""
    s = squeeze(aspect); t = (1.0 - s) / 0.25
    lo, lw = orig_ru.decode('latin1').splitlines(), wide_ru.decode('latin1').splitlines()
    if len(lo) != len(lw): raise ValueError('geometry files differ in structure')
    res = []
    for a, b in zip(lo, lw):
        pa, pb = re.split(r'([ :])', a), re.split(r'([ :])', b)
        if len(pa) != len(pb): raise ValueError('geometry files differ in structure')
        line = []
        for k, (x, y) in enumerate(zip(pa, pb)):
            if x == y: line.append(y); continue
            fx, fy = float(x), float(y)
            # the texture matrix's x scale undoes the stretch (1 -> 4/3 at 16:9): 1/s; everything else is linear
            v = 1.0 / s if a.startswith('s ') and abs(fx - 1.0) < 1e-6 and abs(fy - 4.0 / 3.0) < 1e-3 else fx + (fy - fx) * t
            line.append(('%.6f' % v).rstrip('0').rstrip('.'))
        res.append(''.join(line))
    nl = '\r\n' if b'\r\n' in wide_ru else '\n'
    return (nl.join(res) + (nl if wide_ru.endswith(b'\n') else '')).encode('latin1')
