#!/usr/bin/env python3
"""Converts the third-party models (see CREDITS.txt) into SkyrimPainter meshes and textures.

Source files are not kept in the repository. To re-run:
  1. Unzip "easel-easel-with-painting" (Every 3D, CC BY 4.0) and "cc0-paint-brush-3" (plaggy, CC BY 4.0).
  2. Export their FBX files to OBJ with transforms applied (assimp):
       assimp export source/easel3.fbx easel.obj -ptv
       assimp export source/PaintBrush3.fbx brush.obj -ptv
  3. python3 tools/convert_models.py <easel.obj> <easel textures dir> <brush.obj> <brush textures dir>

Writes Data/meshes/SkyrimPainter/{Easel,Item_Easel,Item_Paint}.nif, their textures in
Data/textures/SkyrimPainter/ and updates tools/bounds.json.

The easel file holds three easels; the two side ones carry a canvas. The left one (Cube.036) is used.
Its canvas front (one quad) becomes the shape "SPCanvas" with 0..1 UVs, so the plugin can put the
painting on it. Ambient occlusion is baked into the colour; normal maps aren't used (flat normal).
"""
import io, json, math, os, struct, sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from meshes import Shape, write_mesh, TEX, OUT, ROOT

CM_TO_UNITS = 0.7  # Skyrim: 1 unit ~ 1.43 cm


def load_obj(path):
    V, VT, VN, groups, cur = [], [], [], {}, None
    for line in open(path):
        t = line.split()
        if not t:
            continue
        if t[0] == "v":
            V.append(tuple(map(float, t[1:4])))
        elif t[0] == "vt":
            VT.append(tuple(map(float, t[1:3])))
        elif t[0] == "vn":
            VN.append(tuple(map(float, t[1:4])))
        elif t[0] in ("o", "g"):
            cur = t[1]
        elif t[0] == "f":
            face = []
            for c in t[1:]:
                p = c.split("/")
                face.append((int(p[0]) - 1, int(p[1]) - 1 if len(p) > 1 and p[1] else -1, int(p[2]) - 1 if len(p) > 2 and p[2] else -1))
            groups.setdefault(cur, []).append(face)
    return np.array(V), np.array(VT), np.array(VN), groups


def to_skyrim(p):
    """OBJ (Y up, front +Z, cm) -> Skyrim (Z up, front +Y): a proper rotation, no mirroring."""
    x, y, z = p
    return (-x, z, y)


def build_shape(name, texture, V, VT, VN, faces, xform, uv_override=None):
    s = Shape(name, texture)
    tri_tangents = []
    for face in faces:
        for k in range(1, len(face) - 1):  # fan-triangulate
            tri = [face[0], face[k], face[k + 1]]
            P = [xform(V[a]) for a, _, _ in tri]
            if uv_override:
                UV = [uv_override(V[a]) for a, _, _ in tri]
            else:
                UV = [(VT[b][0], 1.0 - VT[b][1]) for _, b, _ in tri]  # OBJ v is up, DDS v is down
            N = [to_skyrim(VN[c]) if c >= 0 else None for _, _, c in tri]
            e1, e2 = np.subtract(P[1], P[0]), np.subtract(P[2], P[0])
            fn = np.cross(e1, e2)
            fn = fn / (np.linalg.norm(fn) or 1.0)
            du1, dv1 = UV[1][0] - UV[0][0], UV[1][1] - UV[0][1]
            du2, dv2 = UV[2][0] - UV[0][0], UV[2][1] - UV[0][1]
            det = du1 * dv2 - du2 * dv1
            if abs(det) > 1e-12:
                tg = (e1 * dv2 - e2 * dv1) / det
                bt = (e2 * du1 - e1 * du2) / det
            else:
                tg, bt = np.array([1.0, 0, 0]), np.array([0, 0, -1.0])
            tg = tg / (np.linalg.norm(tg) or 1.0)
            bt = bt / (np.linalg.norm(bt) or 1.0)
            base = len(s.v)
            for i in range(3):
                n = N[i] if N[i] is not None else tuple(fn)
                n = tuple(np.array(n) / (np.linalg.norm(n) or 1.0))
                s.v.append((tuple(P[i]), n, tuple(tg), tuple(bt), UV[i]))
            # keep the winding facing the same way as the normals
            if np.dot(fn, np.mean([s.v[base + i][1] for i in range(3)], axis=0)) < 0:
                s.t.append((base, base + 2, base + 1))
            else:
                s.t.append((base, base + 1, base + 2))
    return s


def save_dds_dxt1(img, path, max_size):
    """DXT1 DDS with the full mip chain (Pillow writes one level; the levels are stacked here)."""
    img = img.convert("RGB")
    if max(img.size) > max_size:
        img = img.resize((max_size, max_size * img.size[1] // img.size[0]), Image.LANCZOS)
    w, h = img.size
    levels, data = [], b""
    cur = img
    while True:
        levels.append(cur)
        if cur.size == (1, 1) or min(cur.size) < 4:
            break
        cur = cur.resize((max(1, cur.size[0] // 2), max(1, cur.size[1] // 2)), Image.BOX)
    for lv in levels:
        buf = io.BytesIO()
        lv.save(buf, format="DDS", pixel_format="DXT1")
        data += buf.getvalue()[128:]
    hdr = b"DDS " + struct.pack("<7I", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, h, w, max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 8, 0, len(levels))
    hdr += b"\0" * 44
    hdr += struct.pack("<2I", 32, 0x4) + b"DXT1" + b"\0" * 20
    hdr += struct.pack("<5I", 0x1000 | 0x8 | 0x400000, 0, 0, 0, 0)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(hdr + data)


def bake_ao(color_path, ao_path):
    col = np.asarray(Image.open(color_path).convert("RGB"), np.float32)
    if ao_path and os.path.exists(ao_path):
        ao = np.asarray(Image.open(ao_path).convert("L").resize((col.shape[1], col.shape[0])), np.float32) / 255.0
        col = col * (0.35 + 0.65 * ao[..., None])
    return Image.fromarray(np.clip(col, 0, 255).astype(np.uint8))


def bounds(shapes):
    P = [v[0] for s in shapes for v in s.v]
    return {"min": [min(p[i] for p in P) for i in range(3)], "max": [max(p[i] for p in P) for i in range(3)]}


def easel(obj, texdir):
    V, VT, VN, groups = load_obj(obj)
    faces = groups["Cube.036"]
    # the canvas front: the faces whose UVs sit in the canvas area of the atlas (u < 0.31, v < 0.44)
    def is_canvas(face):
        c = np.mean([VT[b] for _, b, _ in face], axis=0)
        return c[0] < 0.31 and c[1] < 0.44
    canvas = [f for f in faces if is_canvas(f)]
    wood = [f for f in faces if not is_canvas(f)]
    assert len(canvas) >= 1, "canvas faces not found"
    # origin: floor, middle of the easel's footprint
    used = np.array([V[a] for f in faces for a, _, _ in f])
    cx, cz = (used[:, 0].min() + used[:, 0].max()) / 2, (used[:, 2].min() + used[:, 2].max()) / 2
    y0 = used[:, 1].min()
    def xf(p):
        q = to_skyrim(((p[0] - cx) * CM_TO_UNITS, (p[1] - y0) * CM_TO_UNITS, (p[2] - cz) * CM_TO_UNITS))
        return q
    cp = np.array([V[a] for f in canvas for a, _, _ in f])
    x0, x1, yb, yt = cp[:, 0].min(), cp[:, 0].max(), cp[:, 1].min(), cp[:, 1].max()
    def canvas_uv(p):
        # seen from the front the picture's left is OBJ -x (Skyrim +X); v = 0 at the top
        return ((p[0] - x0) / (x1 - x0), (yt - p[1]) / (yt - yb))
    s_wood = build_shape("SPEasel", TEX + "easel.dds", V, VT, VN, wood, xf)
    s_canvas = build_shape("SPCanvas", TEX + "canvas_blank.dds", V, VT, VN, canvas, xf, canvas_uv)
    print(f"easel: {len(wood)} wood faces, {len(canvas)} canvas faces; canvas {((x1 - x0) * CM_TO_UNITS):.0f} x {((yt - yb) * CM_TO_UNITS):.0f} units, "
          f"centre {((yb + yt) / 2 - y0) * CM_TO_UNITS:.0f} units up")
    save_dds_dxt1(bake_ao(os.path.join(texdir, "EaselBaseColor.jpg"), os.path.join(texdir, "EaselAOwithoutThePicture.png")),
                  os.path.join(ROOT, "Data", "textures", "SkyrimPainter", "easel.dds"), 2048)
    shapes = [s_wood, s_canvas]
    write_mesh(os.path.join(OUT, "Easel.nif"), "SkyrimPainter_Easel", shapes)
    write_mesh(os.path.join(OUT, "Item_Easel.nif"), "SkyrimPainter_Item_Easel", shapes)
    return bounds(shapes)


def brush(obj, texdir):
    V, VT, VN, groups = load_obj(obj)
    faces = groups.get("PaintBrush3") or next(iter(groups.values()))
    used = np.array([V[a] for f in faces for a, _, _ in f])
    c = (used.min(0) + used.max(0)) / 2
    size = (used.max(0) - used.min(0)).max()
    scale = 15.0 / size  # about 15 units (~21 cm) long
    # lie the brush down along X, resting on the floor
    def xf(p):
        q = (np.array(p) - c) * scale
        return (q[1], -q[0], q[2] - (used[:, 2].min() - c[2]) * scale)
    s = build_shape("SPBrush", TEX + "brush.dds", V, VT, VN, faces, xf)
    save_dds_dxt1(bake_ao(os.path.join(texdir, "PaintBrush3Albedo.png"), os.path.join(texdir, "PaintBrushAO.png")),
                  os.path.join(ROOT, "Data", "textures", "SkyrimPainter", "brush.dds"), 1024)
    write_mesh(os.path.join(OUT, "Item_Paint.nif"), "SkyrimPainter_Item_Paint", [s])
    print(f"brush: {len(faces)} faces")
    return bounds([s])


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    b = json.load(open(os.path.join(ROOT, "tools", "bounds.json")))
    b["Easel"] = b["Item_Easel"] = easel(sys.argv[1], sys.argv[2])
    b["Item_Paint"] = brush(sys.argv[3], sys.argv[4])
    json.dump(b, open(os.path.join(ROOT, "tools", "bounds.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
