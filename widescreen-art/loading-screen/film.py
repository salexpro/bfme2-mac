"""Step 1: find where the film frame(s) the loading picture was made from sit in it (SIFT matches -> scale + offset),
put them on a 16:9 canvas around the picture and grade them like the picture (tone curve + colour matrix, both learnt
from where film and picture overlap).
   film.py <loading texture> <out dir> <film frame> [more frames of the same shot ...]
Writes film-full.png (the graded film over the whole canvas) and film-valid.png (where it exists)."""
import sys
import numpy as np, cv2
W, H = 1368, 768                # 16:9 canvas at the picture's own height (the picture is the top 1024x768 of the texture)
art = cv2.imread(sys.argv[1])[:768, :1024]; out = sys.argv[2]
pad = (W - 1024) // 2
gray = lambda im: cv2.createCLAHE(clipLimit=3.0, tileGridSize=(8, 8)).apply(cv2.cvtColor(im, cv2.COLOR_BGR2GRAY))
lum = lambda a: a[..., 0] * 0.114 + a[..., 1] * 0.587 + a[..., 2] * 0.299          # BGR
sift = cv2.SIFT_create(nfeatures=8000, contrastThreshold=0.02)
ka, da = sift.detectAndCompute(gray(art), None)

def place(frm):
    kf, df = sift.detectAndCompute(gray(frm), None)
    good = [a for a, b in cv2.BFMatcher(cv2.NORM_L2).knnMatch(df, da, k=2) if a.distance < 0.75 * b.distance]
    src = np.float32([kf[x.queryIdx].pt for x in good]); dst = np.float32([ka[x.trainIdx].pt for x in good])
    M, inl = cv2.estimateAffinePartial2D(src, dst, method=cv2.RANSAC, ransacReprojThreshold=3.0, maxIters=5000)
    print('  %d matches, %d agree, scale %.4f, at %.1f %.1f' % (len(good), int(inl.sum()), np.hypot(M[0, 0], M[1, 0]), M[0, 2], M[1, 2]))
    M[0, 2] += pad
    warp = cv2.warpAffine(frm, M, (W, H), flags=cv2.INTER_LANCZOS4).astype(np.float32)
    valid = cv2.warpAffine(np.full(frm.shape[:2], 255, np.uint8), M, (W, H), flags=cv2.INTER_NEAREST)
    return warp, cv2.erode(valid, np.ones((5, 5), np.uint8))

def smooth(v, n):
    return np.convolve(np.pad(v, n // 2, mode='edge'), np.ones(n, np.float32) / n, mode='valid')[:len(v)]

def grade_from(warp, valid):
    """learn the picture's look from where film and picture overlap: a tone curve, then a colour matrix"""
    a = art.astype(np.float32); f = warp[:, pad:pad + 1024]; ok = valid[:, pad:pad + 1024] > 0
    reg = np.zeros_like(ok); reg[110:600, 150:874] = True; reg &= ok          # away from the vignette and the legal text
    fl = np.clip(lum(f[reg]), 0, 255); al = lum(a[reg])
    curve = np.zeros(256, np.float32); have = np.zeros(256, bool); b = fl.astype(np.int32)
    for v in range(256):
        s = al[b == v]
        if len(s) > 50: curve[v] = np.median(s); have[v] = True               # the median ignores logo and text pixels
    xs = np.arange(256); curve = smooth(np.interp(xs, xs[have], curve[have]), 11)
    def tone(x):
        l = np.clip(lum(x), 1, 255); return x * (curve[l.astype(np.int32)] / l)[..., None]
    X = tone(f[reg]); Y = a[reg]
    Xh = np.concatenate([X, np.ones((len(X), 1), np.float32)], 1); w = np.ones(len(X), np.float32)
    for _ in range(6):                                                        # robust fit: drop what does not follow
        A, *_ = np.linalg.lstsq(Xh * w[:, None], Y * w[:, None], rcond=None)
        r = np.abs(Xh @ A - Y).sum(1); w = (r < np.percentile(r, 70)).astype(np.float32)
    return lambda x: np.clip(np.concatenate([tone(x), np.ones(x.shape[:-1] + (1,), np.float32)], -1) @ A, 0, 255)

canvas = np.zeros((H, W, 3), np.float32); have = np.zeros((H, W), np.uint8)
for p in sys.argv[3:]:
    print(p.split('/')[-1]); warp, valid = place(cv2.imread(p))
    g = grade_from(warp, valid)(warp)
    # a further frame fills what the earlier ones do not have; where both exist the earlier one fades out towards its edge
    w_old = np.clip(cv2.distanceTransform(have, cv2.DIST_L2, 3) / 24.0, 0, 1) if have.any() else np.zeros((H, W), np.float32)
    w_old = np.where(valid > 0, w_old, 1.0) * (have > 0)
    canvas = canvas * w_old[..., None] + g * (1 - w_old[..., None]) * (valid > 0)[..., None]
    have = np.maximum(have, valid)
have = (have > 0).astype(np.float32)
cv2.imwrite(out + '/film-full.png', np.clip(canvas + 0.5, 0, 255).astype(np.uint8)); cv2.imwrite(out + '/film-valid.png', (have * 255).astype(np.uint8))
print('film placed; rows %d..%d of %d have film' % (np.where(have.any(1))[0].min(), np.where(have.any(1))[0].max(), H))
