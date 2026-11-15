"""Enhance a picture with Real-ESRGAN (x4plus weights), in tiles.   upscale.py <weights .pth> <in> <out png> [tile, default 384]"""
import sys, torch
import numpy as np
from PIL import Image
from spandrel import ModelLoader, ImageModelDescriptor
wpath, src, dst = sys.argv[1:4]; TILE = int(sys.argv[4]) if len(sys.argv) > 4 else 384; OVL = 24
model = ModelLoader().load_from_file(wpath); assert isinstance(model, ImageModelDescriptor)
dev = 'mps' if torch.backends.mps.is_available() else 'cpu'
model.to(dev).eval(); sc = model.scale
im = np.asarray(Image.open(src).convert('RGB'), dtype=np.float32) / 255.0
H, W, _ = im.shape
out = np.zeros((H * sc, W * sc, 3), np.float32); wsum = np.zeros((H * sc, W * sc, 1), np.float32)
def ramp(n, lo, hi):                    # feather only where a tile has a neighbour
    w = np.ones(n, np.float32); k = OVL * sc
    if lo: w[:k] = np.linspace(0.02, 1, k)
    if hi: w[-k:] = np.linspace(1, 0.02, k)
    return w
ys = list(range(0, max(H - TILE, 0) + 1, TILE - OVL)); xs = list(range(0, max(W - TILE, 0) + 1, TILE - OVL))
if ys[-1] + TILE < H: ys.append(H - TILE)
if xs[-1] + TILE < W: xs.append(W - TILE)
with torch.no_grad():
    for y in ys:
        for x in xs:
            t = torch.from_numpy(im[y:y + TILE, x:x + TILE].transpose(2, 0, 1)[None]).to(dev)
            r = model(t).clamp(0, 1)[0].permute(1, 2, 0).float().cpu().numpy()
            wy = ramp(r.shape[0], y > 0, y + TILE < H); wx = ramp(r.shape[1], x > 0, x + TILE < W)
            w = (wy[:, None] * wx[None, :])[..., None]
            out[y * sc:y * sc + r.shape[0], x * sc:x * sc + r.shape[1]] += r * w
            wsum[y * sc:y * sc + r.shape[0], x * sc:x * sc + r.shape[1]] += w
Image.fromarray(np.clip(out / wsum * 255 + 0.5, 0, 255).astype(np.uint8)).save(dst)
print('x%d ->' % sc, out.shape[1], out.shape[0])
