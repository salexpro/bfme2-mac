#!/usr/bin/env python3
"""Build the 16:9 art overrides for BFME II from the user's own game files (nothing from the game is shipped here).

    uv run --with pillow python make-art.py <game folder> <output folder> [aspect, default 16/9] [language, default english]

Writes into the output folder:
  !widescreen-art.big   a small archive that the game loads before all others ('!' sorts first, first copy of a file
                        wins) with the main-menu logo (art\\compiledtextures\\lo\\logowithshadow.tga), the start-up
                        loading screen (lo\\load_w_ea.jpg) and the title screen (ti\\titlescreenuserinterface.jpg) squeezed horizontally by
                        (4/3)/aspect, so that they have their real proportions again once the game stretches its 4:3
                        layout over a wide picture.
The small splash window shown before the game starts (EnglishSplash.jpg, 800x600) is left alone.
"""
import io, os, shutil, struct, sys
from PIL import Image

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

def write_big(path, files):
    table = b''; data = b''; first = 16 + sum(8 + len(n.encode('latin1')) + 1 for n, _ in files)
    for name, blob in files:
        table += struct.pack('>II', first + len(data), len(blob)) + name.encode('latin1') + b'\0'; data += blob
    with open(path, 'wb') as f:
        f.write(b'BIGF' + struct.pack('<I', first + len(data)) + struct.pack('>II', len(files), first) + table + data)

def main():
    game, out = sys.argv[1], sys.argv[2]
    aspect = eval(sys.argv[3]) if len(sys.argv) > 3 else 16 / 9
    language = (sys.argv[4] if len(sys.argv) > 4 else 'english').lower()     # the game's language (registry "Language")
    k = (4 / 3) / aspect
    os.makedirs(out, exist_ok=True)
    if k < 0.999:
        files = []
        name, blob, src = find_in_bigs(game, 'art\\compiledtextures\\lo\\logowithshadow.tga')
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
            name, blob, src = find_in_bigs(game, wanted)
            if not blob: continue
            im = Image.open(io.BytesIO(blob)).convert('RGB')
            w, h = im.size; nw = max(1, round(w * k))
            canvas = Image.new('RGB', (w, h), (0, 0, 0))
            canvas.paste(im.resize((nw, h), Image.LANCZOS), ((w - nw) // 2, 0))
            buf = io.BytesIO(); canvas.save(buf, format='JPEG', quality=92)
            files.append((name, buf.getvalue()))
            print('%s %dx%d from %s squeezed to %d wide with black sides' % (name.split('\\')[-1], w, h, src, nw))
        if files:
            # Root archives: sorted by name, the first copy of a file wins ('!' sorts first).
            # Language archives (lang\<Language>*.big) are applied on top of those, the LAST one wins: sort after the patch's.
            write_big(os.path.join(out, '!widescreen-art.big'), files)
            os.makedirs(os.path.join(out, 'lang'), exist_ok=True)
            write_big(os.path.join(out, 'lang', language + 'zz-widescreen-art.big'), files)
            print('-> !widescreen-art.big and lang/%szz-widescreen-art.big (%d files each)' % (language, len(files)))

if __name__ == '__main__': main()
