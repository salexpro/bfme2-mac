#!/usr/bin/env python3
"""Build the widescreen art and HUD overrides for BFME II from the user's own game files (nothing from the game is
shipped here), for one picture shape. The launch wrapper keeps one output folder per shape (out/16x9, out/a1594, ...)
and installs the one that matches the display at every start.

    uv run --with pillow python make-art.py <game folder> <output folder> [aspect, default 16/9] [language, default english]
    uv run --with pillow python make-art.py --extract <name inside the archives> <game folder> <output file>

Writes into the output folder:
  !widescreen-art.big   a small archive that the game loads before all others ('!' sorts first, first copy of a file
                        wins) with the main-menu logo (art\\compiledtextures\\lo\\logowithshadow.tga), the start-up
                        loading screen (lo\\load_w_ea.jpg) and the title screen (ti\\titlescreenuserinterface.jpg) squeezed horizontally by
                        (4/3)/aspect, so that they have their real proportions again once the game stretches its 4:3
                        layout over a wide picture. Also the frame of the loading screens (lo\\loadpage_bg.dds: the
                        ornament bands and the round medallion the spinning ring is shown in, also seen at the bottom
                        of the title screen): the bands and the medallion are squeezed the same way and the freed
                        sides are continued with the band's own repeating pattern. (The ring itself is a small video;
                        the d3d9 proxy draws it narrower to match, see letterbox.c.)
  root/<language>splash.jpg   only if custom/<language>splash.jpg exists: an enhanced copy, same size, of the small
                        picture shown before the game starts.
A finished wide loading screen can be supplied as custom/load_w_ea-16x9.png (see loading-screen/); for a shape
narrower than 16:9 its middle is used.
For shapes other than 16:9 the archive also gets the in-game HUD files for that shape (see hud.py); at 16:9 the patch's
own are right.
"""
import io, os, shutil, struct, sys
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hud

def big_entries(path):
    with open(path, 'rb') as f:
        h = f.read(16)
        if h[:4] not in (b'BIGF', b'BIG4'): return
        n = struct.unpack('>I', h[8:12])[0]
        table = f.read(struct.unpack('>I', h[12:16])[0] - 16)
        o = 0
        for _ in range(n):
            off, size = struct.unpack_from('>II', table, o); o += 8
            e = table.index(b'\0', o); name = table[o:e].decode('latin1'); o = e + 1
            yield name, off, size

def find_in_bigs(game, wanted):
    """first copy in the game's own load order: archives sorted by name, root folder first"""
    for b in sorted(x for x in os.listdir(game) if x.lower().endswith('.big') and not x.startswith('!widescreen')):
        p = os.path.join(game, b)
        for name, off, size in big_entries(p):
            if name.lower() == wanted.lower():
                with open(p, 'rb') as f:
                    f.seek(off); return name, f.read(size), b
    return None, None, None

def find_effective(game, wanted, language='english'):
    """the copy the game ends up using: language archives (lang/<language>*.big, the last one wins) go over the root ones"""
    lang = os.path.join(game, 'lang'); hit = (None, None, None)
    if os.path.isdir(lang):
        for b in sorted(x for x in os.listdir(lang) if x.lower().startswith(language) and x.lower().endswith('.big') and 'widescreen' not in x.lower()):
            for name, off, size in big_entries(os.path.join(lang, b)):
                if name.lower() == wanted.lower():
                    with open(os.path.join(lang, b), 'rb') as f:
                        f.seek(off); hit = (name, f.read(size), 'lang/' + b)
    return hit if hit[1] else find_in_bigs(game, wanted)

def read_big(path, wanted):
    norm = lambda n: n.lower().replace('/', '\\')            # the original archives use /, the patch's use \
    for name, off, size in big_entries(path):
        if norm(name) == norm(wanted):
            with open(path, 'rb') as f:
                f.seek(off); return f.read(size)
    return None

def hud_files(game, aspect):
    """the HUD files for this shape: [(name in the archive, bytes)]; nothing at 16:9 or without the patch's 16:9 HUD"""
    res = []
    if abs(hud.squeeze(aspect) - 0.75) < 0.004 or not 4 / 3 - 0.01 <= aspect <= 16 / 9 + 0.01: return res
    for base in hud.FILES:
        src = os.path.join(game, 'apt', base.lower() + '.big')
        name, wide, where = find_in_bigs(game, base + '.apt')              # the patch's 16:9 copy (a root archive)
        if not wide or not os.path.exists(src): print('HUD: no 16:9 copy of %s.apt, left as it is' % base); continue
        orig, const = read_big(src, base + '.apt'), read_big(src, base + '.const')
        try: blob, moved, skipped = hud.apt_for(orig, const, wide, aspect)
        except Exception as e: print('HUD: %s.apt not understood (%s), left as it is' % (base, e)); continue
        res.append((name, blob)); print('HUD: %s.apt for this shape (%d numbers moved, from %s)' % (base, moved, where))
        # geometry files the patch changed for this movie
        for b in sorted(x for x in os.listdir(game) if x.lower().endswith('.big') and not x.startswith('!widescreen')):
            for gname, off, size in big_entries(os.path.join(game, b)):
                if gname.lower().startswith(base.lower() + '_geometry\\'):
                    o = read_big(src, gname); w = read_big(os.path.join(game, b), gname)
                    if o and w and gname.lower() not in [n.lower() for n, _ in res]:
                        try: res.append((gname, hud.ru_for(o, w, aspect)))
                        except Exception as e: print('HUD: %s not understood (%s)' % (gname, e))
    return res

def dxt5(im):
    """one mip level as DXT5 blocks: end points = the block's extremes, each pixel the nearest step between them"""
    w, h = im.size; W = max(4, (w + 3) // 4 * 4); H = max(4, (h + 3) // 4 * 4)
    if (W, H) != (w, h): im = im.resize((W, H), Image.NEAREST)
    raw = im.tobytes(); out = bytearray()
    for by in range(0, H, 4):
        rows = [raw[(by + j) * W * 4:(by + j + 1) * W * 4] for j in range(4)]
        for bx in range(0, W * 4, 16):
            px = rows[0][bx:bx + 16] + rows[1][bx:bx + 16] + rows[2][bx:bx + 16] + rows[3][bx:bx + 16]
            a = px[3::4]; a0 = max(a); a1 = min(a); abits = 0
            if a0 != a1:
                d = a0 - a1
                for i in range(16):
                    lv = (7 * (a0 - a[i]) + d // 2) // d                     # 0 = a0 ... 7 = a1
                    abits |= (0 if lv == 0 else 1 if lv == 7 else lv + 1) << (3 * i)
            r = px[0::4]; g = px[1::4]; b = px[2::4]
            r0, r1, g0, g1, b0, b1 = max(r), min(r), max(g), min(g), max(b), min(b)
            c0 = (r0 >> 3) << 11 | (g0 >> 2) << 5 | (b0 >> 3); c1 = (r1 >> 3) << 11 | (g1 >> 2) << 5 | (b1 >> 3)
            dr, dg, db = r0 - r1, g0 - g1, b0 - b1; dd = dr * dr + dg * dg + db * db; cbits = 0
            if dd and c0 != c1:
                for i in range(16):
                    s = ((r[i] - r1) * dr + (g[i] - g1) * dg + (b[i] - b1) * db) * 6 // dd      # 0 = c1 ... 6 = c0
                    cbits |= (1 if s < 1 else 3 if s < 3 else 2 if s < 5 else 0) << (2 * i)
            out += bytes((a0, a1)) + abits.to_bytes(6, 'little') + struct.pack('<HHI', c0, c1, cbits)
    return bytes(out)

def frame_overlay(blob, k, period=45):
    """The loading screens' frame (1024x1024 DXT5, the picture is its top three quarters) for a wide picture.
    The top band with its emblem (rows 0-98) and the bottom band with the medallion (rows 644-768) are squeezed by k
    about the middle; the freed sides are filled by repeating the band's pattern (one tile is `period` pixels wide).
    The large window frame in between stays as it is: the game draws a picture into it."""
    if blob[:4] != b'DDS ' or blob[84:88] != b'DXT5': raise ValueError('not a DXT5 texture')
    h, w = struct.unpack_from('<II', blob, 12); mips = max(1, struct.unpack_from('<I', blob, 28)[0])
    if (w, h) != (1024, 1024): raise ValueError('unexpected size %dx%d' % (w, h))
    im = Image.open(io.BytesIO(blob)).convert('RGBA'); out = im.copy()
    def band(y0, y1, mask_rows):          # mask_rows: (row, weight 0..255 of the new strip) pairs, linear in between
        sh = y1 - y0; strip = im.crop((0, y0, w, y1))
        we = int(round(w / k / 2)) * 2; m = (we - w) // 2
        wide = Image.new('RGBA', (we, sh)); wide.paste(strip, (m, 0))
        lt = strip.crop((0, 0, period, sh)); rt = strip.crop((w - period, 0, w, sh))
        for j in range(1, m // period + 2):
            wide.paste(lt, (m - j * period, 0)); wide.paste(rt, (m + w + (j - 1) * period, 0))
        new = wide.resize((w, sh), Image.LANCZOS)
        col = []
        for y in range(y0, y1):
            for (ya, wa), (yb, wb) in zip(mask_rows, mask_rows[1:]):
                if ya <= y <= yb: col.append(int(round(wa + (wb - wa) * (y - ya) / max(1, yb - ya)))); break
            else: col.append(mask_rows[-1][1] if y > mask_rows[-1][0] else mask_rows[0][1])
        mask = Image.new('L', (1, sh)); mask.putdata(col)
        out.paste(new, (0, y0), mask.resize((w, sh)))
    band(0, 110, [(0, 255), (98, 255), (110, 0)])
    band(632, 768, [(632, 0), (644, 255), (768, 255)])
    data = b''; level = out
    for i in range(mips):
        if i: level = out.resize((max(1, w >> i), max(1, h >> i)), Image.LANCZOS)
        data += dxt5(level)
    return blob[:128] + data

def write_big(path, files):
    table = b''; data = b''; first = 16 + sum(8 + len(n.encode('latin1')) + 1 for n, _ in files)
    for name, blob in files:
        table += struct.pack('>II', first + len(data), len(blob)) + name.encode('latin1') + b'\0'; data += blob
    with open(path, 'wb') as f:
        f.write(b'BIGF' + struct.pack('<I', first + len(data)) + struct.pack('>II', len(files), first) + table + data)

def main():
    if sys.argv[1] == '--extract':      # make-art.py --extract <file name inside the archives> <game folder> <output file>
        name, blob, src = find_effective(sys.argv[3], sys.argv[2])
        if not blob: sys.exit('not found: ' + sys.argv[2])
        open(sys.argv[4], 'wb').write(blob); print('%s from %s' % (name, src)); return
    game, out = sys.argv[1], sys.argv[2]
    aspect = eval(sys.argv[3]) if len(sys.argv) > 3 else 16 / 9
    language = (sys.argv[4] if len(sys.argv) > 4 else 'english').lower()     # the game's language (registry "Language")
    k = (4 / 3) / aspect
    os.makedirs(out, exist_ok=True)
    if k < 0.999:
        files = []
        name, blob, src = find_effective(game, 'art\\compiledtextures\\lo\\logowithshadow.tga', language)
        if blob:
            im = Image.open(io.BytesIO(blob)).convert('RGBA')
            w, h = im.size; nw = max(1, round(w * k))
            canvas = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            canvas.paste(im.resize((nw, h), Image.LANCZOS), ((w - nw) // 2, 0))
            buf = io.BytesIO(); canvas.save(buf, format='TGA')
            files.append((name, buf.getvalue()))
            print('logo %dx%d from %s squeezed to %d wide' % (w, h, src, nw))
        else: print('logo texture not found in the game archives')
        # full-screen 4:3 pictures the game stretches over the whole picture: the start-up loading screen (the one with
        # the legal text) and the title screen behind the user interface
        for wanted in ('art\\compiledtextures\\lo\\load_w_ea.jpg', 'art\\compiledtextures\\ti\\titlescreenuserinterface.jpg'):
            name, blob, src = find_effective(game, wanted, language)
            if not blob: continue
            im = Image.open(io.BytesIO(blob)).convert('RGB')
            w, h = im.size; nw = max(1, round(w * k))
            # custom/<name>-16x9.png next to this script: a finished wide version of that picture (see loading-screen/).
            # It fills the whole screen; without one the original is squeezed and gets black sides.
            custom = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'custom', os.path.splitext(wanted.split('\\')[-1])[0] + '-16x9.png')
            if os.path.exists(custom) and aspect <= 16 / 9 + 0.02 and w == h:
                c = Image.open(custom).convert('RGB')
                cw = min(c.width, round(c.height * aspect))                                        # narrower shape: the middle
                c = c.crop(((c.width - cw) // 2, 0, (c.width - cw) // 2 + cw, c.height))
                size = int(os.environ.get('BFME_ART_SIZE', 2048 if c.height >= 1536 else 1024))    # texture edge, power of two
                canvas = Image.new('RGB', (size, size), (0, 0, 0))                                 # picture = top three quarters
                canvas.paste(c.resize((size, size * 3 // 4), Image.LANCZOS), (0, 0))
                buf = io.BytesIO(); canvas.save(buf, format='JPEG', quality=93, subsampling=0)
                files.append((name, buf.getvalue()))
                print('%s: custom wide picture %dx%d -> %dx%d texture' % (name.split('\\')[-1], c.width, c.height, size, size))
                continue
            canvas = Image.new('RGB', (w, h), (0, 0, 0))
            canvas.paste(im.resize((nw, h), Image.LANCZOS), ((w - nw) // 2, 0))
            buf = io.BytesIO(); canvas.save(buf, format='JPEG', quality=92)
            files.append((name, buf.getvalue()))
            print('%s %dx%d from %s squeezed to %d wide with black sides' % (name.split('\\')[-1], w, h, src, nw))
        name, blob, src = find_effective(game, 'art\\compiledtextures\\lo\\loadpage_bg.dds', language)
        if blob:
            try:
                new = frame_overlay(blob, k)
                if len(new) != len(blob): raise ValueError('size %d instead of %d' % (len(new), len(blob)))
                files.append((name, new)); print('loading-screen frame from %s: bands and medallion squeezed to %.3f' % (src, k))
            except Exception as e: print('loading-screen frame not understood (%s), left as it is' % e)
        files += hud_files(game, aspect)
        if files:
            # Root archives: sorted by name, the first copy of a file wins ('!' sorts first).
            # Language archives (lang\<Language>*.big) are applied on top of those, the LAST one wins: sort after the patch's.
            write_big(os.path.join(out, '!widescreen-art.big'), files)
            os.makedirs(os.path.join(out, 'lang'), exist_ok=True)
            write_big(os.path.join(out, 'lang', language + 'zz-widescreen-art.big'), files)
            print('-> !widescreen-art.big and lang/%szz-widescreen-art.big (%d files each)' % (language, len(files)))
    # custom/<language>splash.jpg: an enhanced copy of the small picture shown before the game starts. It has to keep
    # the original's pixel size (the window is that size). Goes to out/root/, which the launch wrapper copies to the game.
    here = os.path.dirname(os.path.abspath(__file__)); n = language + 'splash.jpg'
    cs = os.path.join(here, 'custom', n); gs = next((os.path.join(game, f) for f in os.listdir(game) if f.lower() == n), None)
    if os.path.exists(cs) and gs:
        if Image.open(cs).size == Image.open(gs).size:
            os.makedirs(os.path.join(out, 'root'), exist_ok=True); shutil.copyfile(cs, os.path.join(out, 'root', os.path.basename(gs)))
            print('-> root/%s (enhanced splash, same size)' % os.path.basename(gs))
        else: print('custom splash ignored: not the size of the game\'s')

if __name__ == '__main__': main()
