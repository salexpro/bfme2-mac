"""Blend the 4:3 loading picture and the graded film around it into one 16:9 image.
   blend.py <texture> <dir with film-full.png, film-valid.png> <out png> [--outer 0.5] [--text-y 630 | --text-y 0]
            [--band <first row> <last row> <period in px>]
   --text-y: first row of the legal text at the bottom (0 = the picture has none)
   --band:   rows of an opaque ornament that runs across the whole width with a repeating pattern (the second
             loading picture has one, with the socket for the spinning ring); it is continued into the sides by
             repeating the pattern

The picture is the film frame plus the artist's shading (a vignette with a dark frame along the border, a dark band at
the bottom, glow) and overlays. So:
 1. E  = the graded film on the whole canvas; rows the film does not have (a band at the top and bottom) are mirrored.
 2. R  = picture / E, smoothed: the artist's shading, measured.
 3. R' = the shading the wide picture should have: per row what the picture has a little inside its border (so the
         dark top frame and the bottom band run on into the sides), times a new vignette towards the new outer edges.
 4. The sides are E * R'. In the picture's border zone its old vignette is exchanged for the new one (picture * R'/R),
    and under the outermost part, where the old frame is nearly black, the film shows through instead.
    The rows with the legal text keep the picture right up to its border."""
import argparse
import numpy as np, cv2
ap = argparse.ArgumentParser(); ap.add_argument('art'); ap.add_argument('dir'); ap.add_argument('out')
ap.add_argument('--outer', type=float, default=0.5); ap.add_argument('--text-y', type=int, default=630)
ap.add_argument('--band', type=int, nargs=3, metavar=('Y0', 'Y1', 'PERIOD'))
args = ap.parse_args(); artp, d, outp, outer = args.art, args.dir, args.out, args.outer
W, H, WIDE = 1368, 768, 96
TEXT_Y = args.text_y if args.text_y > 0 else H + 40      # no legal text: no row keeps the picture up to its border
art = cv2.imread(artp)[:768, :1024].astype(np.float32); pad = (W - 1024) // 2
E = cv2.imread(d + '/film-full.png').astype(np.float32)
valid = cv2.erode(cv2.imread(d + '/film-valid.png', 0), np.ones((7, 7), np.uint8)) > 127
lum = lambda a: a[..., 0] * 0.114 + a[..., 1] * 0.587 + a[..., 2] * 0.299

# 1. film on every row and column of the canvas
cols = np.where(valid.any(0))[0]; rows = np.where(valid[:, cols[len(cols) // 2]])[0]
y0, y1, x0, x1 = rows.min(), rows.max(), cols.min(), cols.max()
for y in range(y0 - 1, -1, -1): E[y] = E[min(2 * y0 - y, y1)]
for y in range(y1 + 1, H): E[y] = E[max(2 * y1 - y, y0)]
for x in range(x0 - 1, -1, -1): E[:, x] = E[:, min(2 * x0 - x, x1)]
for x in range(x1 + 1, W): E[:, x] = E[:, max(2 * x1 - x, x0)]
for (a, b, sgn) in ((0, y0, -1), (y1 + 1, H, 1)):                       # soften the mirrored bands a little
    if b > a: E[a:b] = cv2.GaussianBlur(E[a:b], (0, 0), 1.2)

# 1b. an ornament band across the picture: in its rows E is the picture itself, continued sideways by its pattern
if args.band:
    by0, by1, P = args.band; CLEAN = 64                                 # take the pattern from inside the border shading
    src = np.arange(W) - pad                                            # canvas column -> picture column
    lo = src < CLEAN; src[lo] += ((CLEAN - src[lo] + P - 1) // P) * P
    hi = src > 1023 - CLEAN; src[hi] -= ((src[hi] - (1023 - CLEAN) + P - 1) // P) * P
    tiled = art[:, src]                                                 # (rows, W, 3)
    wy = np.clip(np.minimum(np.arange(H) - (by0 - 2), (by1 + 2) - np.arange(H)) / 2.0, 0, 1)     # 1 on the band's rows
    E = E * (1 - wy[:, None, None]) + tiled * wy[:, None, None]
else: wy = np.zeros(H, np.float32)

# 2. the picture's shading relative to the film
A = np.zeros((H, W), np.float32); A[:, pad:pad + 1024] = lum(art); M = np.zeros((H, W), np.float32); M[:, pad:pad + 1024] = 1
M[485:700, pad + 90:pad + 280] = 0                                       # the rider: not in the same place in film and picture
if TEXT_Y < H: M[TEXT_Y:, :] *= (A[TEXT_Y:, :] < 55)                      # the legal text is not shading
El = np.maximum(lum(E), 1.0)
blur = lambda a: cv2.GaussianBlur(a, (0, 0), 18)
shading = lambda m: np.clip(blur(A * m) / np.maximum(blur(El * m), 1e-3), 0.02, 3.0)
onband = (wy > 0)[:, None].astype(np.float32)                           # the band and the picture around it are shaded
Ro, Ri = shading(M * (1 - onband)), shading(M * onband)                 # differently: measured apart, no leaking
R = Ro * (1 - wy[:, None]) + Ri * wy[:, None]

# 3. the shading for the wide picture
xs = np.arange(W, dtype=np.float32); dist = np.minimum(xs - pad, pad + 1024 - 1 - xs)      # > 0 inside the picture
left = xs < W / 2
tt = np.clip((80 - dist) / (pad + 80), 0, 1)
fall = (1 - (1 - outer) * tt ** 1.4) * (1 - 0.6 * np.clip((tt - 0.88) / 0.12, 0, 1) ** 1.5)  # vignette + dark frame at the edge
Rn = np.zeros((H, W), np.float32)
for side in (True, False):
    cols = slice(pad + 100, pad + 140) if side else slice(W - pad - 140, W - pad - 100)
    sm = lambda r: cv2.GaussianBlur(r[:, cols].mean(1).reshape(-1, 1), (0, 0), 6).ravel()
    rref = sm(Ro) * (1 - wy) + sm(Ri) * wy
    sel = left if side else ~left
    Rn[:, sel] = rref[:, None] * fall[None, sel]

# 4. compose
t = np.clip((dist - 14) / (WIDE - 14), 0, 1); wide = t * t * (3 - 2 * t)
narrow = np.clip((dist - 20) / 12, 0, 1)
rowmix = np.clip((TEXT_Y - 6 - np.arange(H, dtype=np.float32)) / 30, 0, 1)                   # 0 on the rows with the legal text
keep = rowmix[:, None] * wide[None, :] + (1 - rowmix[:, None]) * narrow[None, :]
w = np.clip((150 - dist) / 80, 0, 1); w = w * w * (3 - 2 * w)                                # where the old vignette is exchanged
canvas = np.zeros((H, W, 3), np.float32); canvas[:, pad:pad + 1024] = art
g = 1 + (np.clip(Rn / R, 0.4, 2.4) - 1) * w[None, :] * (0.4 + 0.6 * rowmix[:, None])       # gentler on the legal text
res = canvas * g[..., None] * keep[..., None] + E * Rn[..., None] * (1 - keep[..., None])
cv2.imwrite(outp, np.clip(res + 0.5, 0, 255).astype(np.uint8))
