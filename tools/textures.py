#!/usr/bin/env python3
"""Generates SkyrimPainter's own textures (the blank canvas and a flat normal map) (uncompressed BGRA DDS with mipmaps) into
Data/textures/SkyrimPainter/. Everything is procedural, so the mod ships no third-party art.

Usage: python3 tools/textures.py
"""
import os, struct
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "Data", "textures", "SkyrimPainter")
rng = np.random.default_rng(1234)


def dds_bytes(img):
    """img: PIL RGBA -> classic DX9 DDS, A8R8G8B8 with the full mip chain."""
    w, h = img.size
    levels = [img]
    while levels[-1].size != (1, 1):
        lw, lh = levels[-1].size
        levels.append(levels[-1].resize((max(1, lw // 2), max(1, lh // 2)), Image.BOX))
    hdr = b"DDS " + struct.pack("<7I", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x8, h, w, w * 4, 0, len(levels))
    hdr += b"\0" * 44
    hdr += struct.pack("<8I", 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    hdr += struct.pack("<5I", 0x1000 | 0x8 | 0x400000, 0, 0, 0, 0)
    assert len(hdr) == 128
    data = b""
    for lv in levels:
        a = np.asarray(lv.convert("RGBA"), dtype=np.uint8)
        data += a[:, :, [2, 1, 0, 3]].tobytes()  # BGRA
    return hdr + data


def save(name, arr):
    arr = np.clip(arr, 0, 255).astype(np.uint8)
    if arr.shape[2] == 3:
        arr = np.concatenate([arr, np.full(arr.shape[:2] + (1,), 255, np.uint8)], axis=2)
    os.makedirs(OUT, exist_ok=True)
    open(os.path.join(OUT, name + ".dds"), "wb").write(dds_bytes(Image.fromarray(arr, "RGBA")))


def smooth_noise(h, w, cells_y, cells_x):
    """Value noise upscaled bicubically (0..1)."""
    small = rng.random((cells_y, cells_x)).astype(np.float32)
    return np.asarray(Image.fromarray((small * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC), np.float32) / 255.0


def main():
    # ---- canvas: plain white primed canvas with a fine weave (1024 x 768 = the painting's 4:3)
    # (keep in step with Studio.cpp MakeBlank, which starts a new painting the same way)
    h, w = 768, 1024
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    weave = np.sin(x * 2.094) * np.sin(y * 2.094) * 3.0          # threads every 3 px
    threads = (rng.random((1, w)) - 0.5) * 3.0 + (rng.random((h, 1)) - 0.5) * 3.0  # uneven threads
    canvas = np.stack([248 + weave + threads, 247 + weave + threads, 244 + weave + threads], axis=2)
    save("canvas_blank", canvas)

    # ---- flat normal map, alpha 0 = no specular (matte)
    flat = np.zeros((4, 4, 4), np.uint8)
    flat[..., 0], flat[..., 1], flat[..., 2], flat[..., 3] = 128, 128, 255, 0
    os.makedirs(OUT, exist_ok=True)
    open(os.path.join(OUT, "flat_n.dds"), "wb").write(dds_bytes(Image.fromarray(flat, "RGBA")))
    print("textures written to", OUT)


if __name__ == "__main__":
    main()
