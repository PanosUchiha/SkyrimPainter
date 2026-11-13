#!/usr/bin/env python3
"""Removes the paint from the palette model's colour texture ("Round Artist Palette" by LUMI-Studio).

The atlas has two islands: the painted top face and the (mostly bare) wooden back. The back sits 490 px
below the top with the same shape, so: clean the paint flecks off the back (filling along the wood grain),
copy it over the top face, pull any leftover paint tint back to the wood's hue, and bleed the result into
the padding so mipmaps don't pick up the old colours.

Usage: python3 tools/clean_palette_texture.py <palette.obj> <T_Palette_02_BaseColor.png> <T_Palette_02_AO.png> <out_base.png> <out_ao.png>
(palette.obj: `assimp export SM_Palette_02a.fbx palette.obj -ptv`). Needs numpy, Pillow, opencv-python-headless.
"""
import sys
import numpy as np, cv2
from PIL import Image, ImageDraw


def island_masks(obj, W, H):
    V, VT, F = [], [], []
    for l in open(obj):
        t = l.split()
        if not t:
            continue
        if t[0] == "v":
            V.append(list(map(float, t[1:4])))
        elif t[0] == "vt":
            VT.append(list(map(float, t[1:3])))
        elif t[0] == "f":
            F.append([(int(a.split("/")[0]) - 1, int(a.split("/")[1]) - 1) for a in t[1:]])
    V, VT = np.array(V), np.array(VT)
    m = {k: Image.new("L", (W, H)) for k in ("top", "bottom", "side")}
    for f in F:
        p = V[[a for a, _ in f]]
        n = np.cross(p[1] - p[0], p[2] - p[0])
        n /= np.linalg.norm(n) + 1e-12
        k = "top" if n[1] > 0.7 else ("bottom" if n[1] < -0.7 else "side")
        ImageDraw.Draw(m[k]).polygon([(VT[b][0] * W, (1 - VT[b][1]) * H) for _, b in f], fill=255)
    return [np.asarray(m[k]) > 0 for k in ("top", "bottom", "side")]


def main(obj, base_path, ao_path, out_base, out_ao):
    base = np.asarray(Image.open(base_path).convert("RGB")).copy()
    ao = np.asarray(Image.open(ao_path).convert("L")).copy()
    H, W = base.shape[:2]
    top, bot, side = island_masks(obj, W, H)
    wood = bot | side
    hsv = cv2.cvtColor(base, cv2.COLOR_RGB2HSV).astype(int)
    h, s, v = hsv[..., 0], hsv[..., 1], hsv[..., 2]
    med = cv2.medianBlur(base, 31).astype(int)
    dev = np.abs(base.astype(int) - med).sum(2)
    hm = cv2.cvtColor(med.astype(np.uint8), cv2.COLOR_RGB2HSV).astype(int)
    huedev = np.minimum(np.abs(h - hm[..., 0]), 180 - np.abs(h - hm[..., 0]))
    paint = wood & (((huedev > 14) & (s > 45)) | ((v > hm[..., 2] + 45) & (s < hm[..., 1] - 40)) | (dev > 110) |
                    ((h > 21) & (h < 120) & (s > 50)) | ((h >= 100) & (s > 30)))
    paint = cv2.dilate(paint.astype(np.uint8), np.ones((5, 5), np.uint8)) > 0
    paint &= cv2.erode(wood.astype(np.uint8), np.ones((5, 5), np.uint8)) > 0
    # fill the flecks along the wood grain (direction from the structure tensor)
    g = cv2.cvtColor(base, cv2.COLOR_RGB2GRAY).astype(np.float32)
    gx, gy = cv2.Sobel(g, cv2.CV_32F, 1, 0, ksize=5), cv2.Sobel(g, cv2.CV_32F, 0, 1, ksize=5)
    m = bot & ~paint
    ang = 0.5 * np.arctan2(2 * (gx * gy)[m].sum(), (gx * gx)[m].sum() - (gy * gy)[m].sum()) + np.pi / 2
    L = 61
    k = np.zeros((L, L), np.float32)
    c = L // 2
    for t in np.linspace(-c, c, 4 * L):
        k[int(round(c + t * np.sin(ang))), int(round(c + t * np.cos(ang)))] = 1
    img = base.astype(np.float32)
    valid = (wood & ~paint).astype(np.float32)
    todo = paint.copy()
    for _ in range(8):
        num = np.stack([cv2.filter2D(img[..., ch] * valid, -1, k) for ch in range(3)], 2)
        den = cv2.filter2D(valid, -1, k)
        ok = todo & (den > 2)
        img[ok] = num[ok] / den[ok][:, None]
        valid = np.maximum(valid, ok.astype(np.float32))
        todo &= ~ok
        if not todo.any():
            break
    clean = np.clip(img, 0, 255).astype(np.uint8)
    if todo.any():
        clean = cv2.inpaint(clean, todo.astype(np.uint8) * 255, 5, cv2.INPAINT_TELEA)
    # the top face gets the cleaned back
    shift = lambda a: np.roll(np.roll(a, -490, 0), 1, 1)
    grow = cv2.dilate(top.astype(np.uint8), np.ones((7, 7), np.uint8)) > 0
    clean[grow] = shift(clean)[grow]
    ao[grow] = shift(ao)[grow]
    # leftover tints -> wood hue; white specks -> wood brightness
    island = top | bot | side
    hsv2 = cv2.cvtColor(clean, cv2.COLOR_RGB2HSV).astype(np.float32)
    Hh, Ss, Vv = hsv2[..., 0], hsv2[..., 1], hsv2[..., 2]
    woodH, woodS = float(np.median(Hh[bot])), float(np.median(Ss[bot]))
    Hh[island & (((Hh > 19) & (Hh < 170)) | (Hh < 8))] = woodH
    Ss[island] = np.clip(Ss[island], woodS * 0.75, min(255, woodS * 1.25))
    lo, hi = np.percentile(Vv[bot], [5, 90])
    Vv[island] = np.clip(Vv[island], lo, hi)
    out = cv2.cvtColor(np.stack([Hh, Ss, Vv], 2).astype(np.uint8), cv2.COLOR_HSV2RGB).astype(np.float32)
    # padding: grow the island colours a few pixels, plain wood colour beyond
    valid = (cv2.dilate(island.astype(np.uint8), np.ones((3, 3), np.uint8)) > 0).astype(np.float32)
    mean = out[bot].mean(0)
    for _ in range(6):
        blur = cv2.blur(out * valid[..., None], (3, 3))
        vb = cv2.blur(valid, (3, 3))
        g2 = (vb > 0) & (valid == 0)
        out[g2] = blur[g2] / vb[g2][:, None]
        valid[g2] = 1
    out[valid == 0] = mean
    Image.fromarray(np.clip(out, 0, 255).astype(np.uint8)).save(out_base)
    Image.fromarray(ao).save(out_ao)
    print(f"cleaned {paint.sum()} paint pixels; top face replaced with the back; wrote {out_base}")


if __name__ == "__main__":
    if len(sys.argv) != 6:
        raise SystemExit(__doc__)
    main(*sys.argv[1:])
