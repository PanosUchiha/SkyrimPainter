#!/usr/bin/env python3
"""Converts the third-party models (see CREDITS.txt) into SkyrimPainter's meshes, textures and UI pictures.

Source files are not kept in the repository. To re-run:
  1. Unzip "easel-easel-with-painting" (Every 3D, CC BY 4.0) and "cc0-paint-brush-3" (plaggy, CC BY 4.0);
     the palette is "Round Artist Palette" by LUMI-Studio (CGTrader, royalty free).
  2. Export the FBX files to OBJ with transforms applied (assimp):
       assimp export source/easel3.fbx easel.obj -ptv
       assimp export source/PaintBrush3.fbx brush.obj -ptv
       assimp export SM_Palette_02a.fbx palette.obj -ptv
  3. Clean the paint off the palette: tools/clean_palette_texture.py (see that script).
  4. python3 tools/convert_models.py <easel.obj> <easel textures dir> <brush.obj> <brush textures dir>
                                     <palette.obj> <clean palette colour png> <clean palette ao png>

The easel file holds three easels: the middle one has no canvas, the side ones do. The middle one is the
easel; the canvas board is what the left one has extra (it is the middle one moved 130 cm). The board
becomes the canvas: on the easel (landscape and portrait), on the wall (landscape and portrait) and the
Blank Canvas / Painting items. Its front is the shape "SPCanvas" with 0..1 UVs (the plugin puts the
painting there).

Writes Data/meshes/SkyrimPainter/*.nif, Data/textures/SkyrimPainter/{easel,brush,palette}.dds,
Data/SKSE/Plugins/SkyrimPainter/UI/{palette,brush}.png, src/PaletteSpots.h and tools/bounds.json.
"""
import io, json, math, os, struct, sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nifwriter as nw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MESHES = os.path.join(ROOT, "Data", "meshes", "SkyrimPainter")
TEXTURES = os.path.join(ROOT, "Data", "textures", "SkyrimPainter")
UI = os.path.join(ROOT, "Data", "SKSE", "Plugins", "SkyrimPainter", "UI")
TEX = "textures\\SkyrimPainter\\"
NORMAL = TEX + "flat_n.dds"
CM = 0.7  # Skyrim: 1 unit ~ 1.43 cm


# ------------------------------------------------------------------ reading
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


def unit(v):
    v = np.asarray(v, float)
    n = np.linalg.norm(v)
    return v / n if n > 0 else v


# ------------------------------------------------------------------ building meshes
def make_mesh(name, texture, V, VT, VN, faces, xf, rot, uv_fn=None):
    """xf: OBJ point -> Skyrim point; rot: OBJ direction -> Skyrim direction (rotation only)."""
    m = nw.Mesh(name, texture, NORMAL)
    for face in faces:
        for k in range(1, len(face) - 1):
            tri = [face[0], face[k], face[k + 1]]
            P = [np.asarray(xf(V[a]), float) for a, _, _ in tri]
            UV = [uv_fn(V[a]) if uv_fn else (VT[b][0], 1.0 - VT[b][1]) for a, b, _ in tri]
            e1, e2 = P[1] - P[0], P[2] - P[0]
            fn = unit(np.cross(e1, e2))
            du1, dv1 = UV[1][0] - UV[0][0], UV[1][1] - UV[0][1]
            du2, dv2 = UV[2][0] - UV[0][0], UV[2][1] - UV[0][1]
            det = du1 * dv2 - du2 * dv1
            if abs(det) > 1e-12:
                tg, bt = unit((e1 * dv2 - e2 * dv1) / det), unit((e2 * du1 - e1 * du2) / det)
            else:
                tg, bt = np.array([1.0, 0, 0]), np.array([0, 0, -1.0])
            Ns = [unit(rot(VN[c])) if c >= 0 else fn for _, _, c in tri]
            base = len(m.verts)
            for p, uv, n in zip(P, UV, Ns):
                m.verts.append((tuple(float(x) for x in p), (float(uv[0]), float(uv[1])), tuple(float(x) for x in n),
                                tuple(float(x) for x in tg), tuple(float(x) for x in bt)))
            if np.dot(fn, sum(Ns)) < 0:
                m.tris.append((base, base + 2, base + 1))
            else:
                m.tris.append((base, base + 1, base + 2))
    return m


def bbox(meshes):
    P = np.array([v[0] for m in meshes for v in m.verts])
    return P.min(0), P.max(0)


def collision(meshes, dynamic, mass=1.0, material=nw.MAT_WOOD, pad=0.0):
    mn, mx = bbox(meshes)
    half = tuple(float(max(0.5, (mx[i] - mn[i]) / 2 + pad)) for i in range(3))
    center = tuple(float((mx[i] + mn[i]) / 2) for i in range(3))
    return dict(half=half, center=center, layer=nw.LAYER_CLUTTER if dynamic else nw.LAYER_STATIC, mass=mass,
                material=material, dynamic=dynamic)


BOUNDS = {}
BRUSH_TIP = (1.0, 0.0, 1.0)


def write(stem, meshes, coll=None):
    nw.build(os.path.join(MESHES, stem + ".nif"), "SkyrimPainter_" + stem, meshes, coll)
    mn, mx = bbox(meshes)
    BOUNDS[stem] = {"min": [float(x) for x in mn], "max": [float(x) for x in mx]}
    print(f"  {stem}.nif  size {(mx - mn).round(1)}")


# ------------------------------------------------------------------ textures
def save_dds_dxt1(img, path, max_size):
    """DXT1 DDS with the full mip chain (Pillow writes one level; the levels are stacked here)."""
    img = img.convert("RGB")
    if max(img.size) > max_size:
        s = max_size / max(img.size)
        img = img.resize((max(4, int(img.size[0] * s)), max(4, int(img.size[1] * s))), Image.LANCZOS)
    w, h = img.size
    levels, cur = [], img
    while True:
        levels.append(cur)
        if min(cur.size) <= 4:
            break
        cur = cur.resize((max(1, cur.size[0] // 2), max(1, cur.size[1] // 2)), Image.BOX)
    data = b""
    for lv in levels:
        buf = io.BytesIO()
        lv.save(buf, format="DDS", pixel_format="DXT1")
        data += buf.getvalue()[128:]
    hdr = b"DDS " + struct.pack("<7I", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, h, w, max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 8, 0, len(levels))
    hdr += b"\0" * 44 + struct.pack("<2I", 32, 0x4) + b"DXT1" + b"\0" * 20 + struct.pack("<5I", 0x1000 | 0x8 | 0x400000, 0, 0, 0, 0)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(hdr + data)


def bake_ao(color, ao_path, strength=0.65):
    col = np.asarray(color.convert("RGB") if isinstance(color, Image.Image) else Image.open(color).convert("RGB"), np.float32)
    if ao_path and os.path.exists(ao_path):
        ao = np.asarray(Image.open(ao_path).convert("L").resize((col.shape[1], col.shape[0])), np.float32) / 255.0
        col = col * ((1 - strength) + strength * ao[..., None])
    return Image.fromarray(np.clip(col, 0, 255).astype(np.uint8))


# ------------------------------------------------------------------ simple rasterizer for the UI pictures
def raster(tris, size, tex):
    """tris: list of (3 screen points (x, y, depth), 3 uvs, shade). Returns RGBA (transparent background)."""
    W, H = size
    img = np.zeros((H, W, 4), np.float32)
    zbuf = np.full((H, W), -1e9, np.float32)
    th, tw = tex.shape[:2]
    for pts, uvs, shade in tris:
        p = np.array(pts, float)
        x0, x1 = int(max(0, np.floor(p[:, 0].min()))), int(min(W - 1, np.ceil(p[:, 0].max())))
        y0, y1 = int(max(0, np.floor(p[:, 1].min()))), int(min(H - 1, np.ceil(p[:, 1].max())))
        if x0 > x1 or y0 > y1:
            continue
        ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1].astype(np.float32) + 0.5
        (ax, ay, _), (bx, by, _), (cx, cy, _) = p
        den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(den) < 1e-9:
            continue
        w0 = ((by - cy) * (xs - cx) + (cx - bx) * (ys - cy)) / den
        w1 = ((cy - ay) * (xs - cx) + (ax - cx) * (ys - cy)) / den
        w2 = 1 - w0 - w1
        inside = (w0 >= -1e-4) & (w1 >= -1e-4) & (w2 >= -1e-4)
        z = w0 * p[0, 2] + w1 * p[1, 2] + w2 * p[2, 2]
        sub = zbuf[y0:y1 + 1, x0:x1 + 1]
        ok = inside & (z > sub)
        if not ok.any():
            continue
        u = w0 * uvs[0][0] + w1 * uvs[1][0] + w2 * uvs[2][0]
        v = w0 * uvs[0][1] + w1 * uvs[1][1] + w2 * uvs[2][1]
        tx = np.clip((u % 1.0) * tw, 0, tw - 1).astype(int)
        ty = np.clip(v * th, 0, th - 1).astype(int)
        col = tex[ty, tx, :3] * shade
        region = img[y0:y1 + 1, x0:x1 + 1]
        region[ok, :3] = col[ok]
        region[ok, 3] = 255
        sub[ok] = z[ok]
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGBA")


def ui_picture(mesh, tex_img, out_path, px_per_unit, light=(0.35, 0.45, 0.82)):
    """Renders a mesh seen from above (Skyrim +Z): X to the right, +Y up the picture."""
    P = np.array([v[0] for v in mesh.verts])
    mn, mx = P.min(0), P.max(0)
    W = int((mx[0] - mn[0]) * px_per_unit) + 8
    H = int((mx[1] - mn[1]) * px_per_unit) + 8
    L = unit(light)
    tris = []
    for a, b, c in mesh.tris:
        vs = [mesh.verts[i] for i in (a, b, c)]
        pts = [((v[0][0] - mn[0]) * px_per_unit + 4, (mx[1] - v[0][1]) * px_per_unit + 4, v[0][2]) for v in vs]
        n = unit(sum(np.array(v[2]) for v in vs))
        tris.append((pts, [v[1] for v in vs], 0.55 + 0.45 * max(0.0, float(np.dot(n, L)))))
    img = raster(tris, (W, H), np.asarray(tex_img.convert("RGB"), np.float32))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    img.save(out_path)
    return img


def write_canvas_corners(sets):
    """src/CanvasGeom.h: the painted face's corners in the canvas NIF (Skyrim units), at UV (0,0), (1,0),
    (1,1), (0,1) - so the studio can find the canvas on screen and paint on it where the mouse is."""
    out = ["// Generated by tools/convert_models.py - do not edit.", "#pragma once", "", "namespace CanvasGeom", "{",
           "\t// corners of the painted face in the easel canvas model, at UV (0,0) (1,0) (1,1) (0,1): top-left first, clockwise as seen"]
    for name, meshes in sets.items():
        m = next(x for x in meshes if x.name == "SPCanvas")
        pts = []
        for tu, tv in ((0, 0), (1, 0), (1, 1), (0, 1)):
            best = min(m.verts, key=lambda v: (v[1][0] - tu) ** 2 + (v[1][1] - tv) ** 2)
            pts.append(best[0])
        rows = ", ".join("{ %.3ff, %.3ff, %.3ff }" % p for p in pts)
        out.append(f"\tinline constexpr float k{name}[4][3] = {{ {rows} }};")
    out += ["}", ""]
    open(os.path.join(ROOT, "src", "CanvasGeom.h"), "w").write("\n".join(out))


# ------------------------------------------------------------------ the easel and its canvas
def easel_and_canvas(obj, texdir):
    V, VT, VN, G = load_obj(obj)
    middle, left = G["Cube.035"], G["Cube.036"]
    known = {tuple(np.round(V[a], 2)) for f in middle for a, _, _ in f}
    shift = np.array([-130.0, 0, 0])  # the left easel is the middle one moved 130 cm
    board = [f for f in left if any(tuple(np.round(V[a] - shift, 2)) not in known for a, _, _ in f)]
    print(f"easel: {len(middle)} faces, canvas board: {len(board)} faces")
    pm = np.array([V[a] for f in middle for a, _, _ in f])
    cz, y0 = (pm[:, 2].min() + pm[:, 2].max()) / 2, pm[:, 1].min()

    def local(p):  # OBJ cm (Y up, front +Z), centred on the easel -> Skyrim units (Z up, front +Y), no mirroring
        x, y, z = p[0] * CM, (p[1] - y0) * CM, (p[2] - cz) * CM
        return (-x, z, y)

    rot = lambda d: (-d[0], d[2], d[1])

    atlas = bake_ao(os.path.join(texdir, "EaselBaseColor.jpg"), os.path.join(texdir, "EaselAOwithoutThePicture.png"))
    save_dds_dxt1(atlas, os.path.join(TEXTURES, "easel.dds"), 2048)
    easel = make_mesh("SPEasel", TEX + "easel.dds", V, VT, VN, middle, local, rot)
    write("Easel", [easel], collision([easel], dynamic=False))
    write("Item_Easel", [easel], collision([easel], dynamic=True, mass=5.0))

    # ---- the board's frame: front normal n, right r (the viewer's right = OBJ +x), up u = n x r
    def uvc(f):
        return np.mean([VT[b] for _, b, _ in f], axis=0)
    front = [f for f in board if uvc(f)[0] < 0.31 and uvc(f)[1] < 0.44]
    rest = [f for f in board if not (uvc(f)[0] < 0.31 and uvc(f)[1] < 0.44)]
    fp = np.array([V[a] - shift for f in front for a, _, _ in f])
    n = unit(np.cross(fp[1] - fp[0], fp[2] - fp[0]))
    if n[2] < 0:
        n = -n
    r = unit(np.array([1.0, 0, 0]) - n * n[0])
    u = np.cross(n, r)
    c = fp.mean(0)
    Wd = (fp @ r).max() - (fp @ r).min()
    Ht = (fp @ u).max() - (fp @ u).min()
    c = c - r * (((fp - c) @ r).max() + ((fp - c) @ r).min()) / 2 * 0 - u * 0  # centre of the front quad
    bp = np.array([V[a] - shift for f in board for a, _, _ in f])
    dmin = ((bp - c) @ n).min()
    print(f"canvas board: {Wd:.1f} x {Ht:.1f} cm (aspect {Wd / Ht:.3f}), {(((bp - c) @ n).max() - dmin):.1f} cm deep")

    # A deeper canvas than the model's (a gallery-wrapped stretcher): the back stays where it is, against
    # the easel's mast, and the painted face comes forward by THICKER cm.
    THICKER = 2.5
    depth0 = -dmin
    stretch = (depth0 + THICKER) / depth0

    def coords(p):  # board-local (right, up, depth) in cm
        q = np.asarray(p) - shift - c
        dd = float(q @ n)
        return float(q @ r), float(q @ u), dmin + (dd - dmin) * stretch

    def turn(rr, uu, portrait):
        return (-uu, rr) if portrait else (rr, uu)

    def uv_for(portrait):
        def uv(p):
            rr, uu, _ = coords(p)
            rr, uu = turn(rr, uu, portrait)
            return (rr / Ht + 0.5, 0.5 - uu / Wd) if portrait else (rr / Wd + 0.5, 0.5 - uu / Ht)
        return uv

    # The easel's top clamp holds a landscape canvas's top edge. A portrait canvas is taller: it sits a
    # few mm forward (covering the easel's own clamp) and brings a clamp of its own at its top edge.
    key = lambda a: tuple(np.round(V[a], 3))
    parent = {}

    def root(x):
        while parent.setdefault(x, x) != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for f in middle:
        for a, _, _ in f[1:]:
            ra, rb = root(key(f[0][0])), root(key(a))
            if ra != rb:
                parent[ra] = rb
    lowest = {}
    for f in middle:
        k = root(key(f[0][0]))
        lowest[k] = min(lowest.get(k, 1e9), min(V[a][1] for a, _, _ in f))
    clamp = [f for f in middle if lowest[root(key(f[0][0]))] > 139.0]
    print(f"top clamp: {len(clamp)} faces")
    forward = 0.4  # cm

    def on_easel(portrait):
        lift = (Wd - Ht) / 2 if portrait else 0.0  # the bottom edge stays on the easel's ledge
        fwd = 0.0

        def place(p):
            rr, uu, dd = coords(p)
            rr, uu = turn(rr, uu, portrait)
            return local(c + r * rr + u * (uu + lift) + n * (dd + fwd))


        def drot(d):
            d = np.asarray(d)
            rr, uu = turn(float(d @ r), float(d @ u), portrait)
            return rot(r * rr + u * uu + n * float(d @ n))

        # the canvas's own top clamp, brought forward with the face so its lip still holds the front edge
        # (the easel's own clamp ends up hidden behind / inside the deeper canvas)
        raise_by = u * ((Wd - Ht) if portrait else 0.0) + n * (THICKER + 0.1)
        top = make_mesh("SPCanvasClamp", TEX + "easel.dds", V, VT, VN, clamp, lambda p: local(np.asarray(p) + raise_by), rot)
        return [make_mesh("SPCanvasBoard", TEX + "easel.dds", V, VT, VN, rest, place, drot),
                make_mesh("SPCanvas", TEX + "canvas_blank.dds", V, VT, VN, front, place, drot, uv_for(portrait)), top]

    def flat(portrait):
        def place(p):  # the front faces Skyrim +Y, back at y = 0, centred
            rr, uu, dd = coords(p)
            rr, uu = turn(rr, uu, portrait)
            return (-rr * CM, (dd - dmin) * CM, uu * CM)

        def drot(d):
            d = np.asarray(d)
            rr, uu = turn(float(d @ r), float(d @ u), portrait)
            return (-rr, float(d @ n), uu)

        return [make_mesh("SPCanvasBoard", TEX + "easel.dds", V, VT, VN, rest, place, drot),
                make_mesh("SPCanvas", TEX + "canvas_blank.dds", V, VT, VN, front, place, drot, uv_for(portrait))]

    easel_land, easel_port = on_easel(False), on_easel(True)
    write("EaselCanvas_Landscape", easel_land)
    write("EaselCanvas_Portrait", easel_port)
    write_canvas_corners({"Landscape": easel_land, "Portrait": easel_port})
    land, port = flat(False), flat(True)
    write("WallCanvas_Landscape", land, collision(land, dynamic=False, pad=0.5))
    write("WallCanvas_Portrait", port, collision(port, dynamic=False, pad=0.5))
    write("Item_Canvas", land, collision(land, dynamic=True, mass=1.0))
    return {"canvasW": Wd * CM, "canvasH": Ht * CM, "centreUp": float((c[1] - y0) * CM)}


def brush(obj, texdir):
    V, VT, VN, G = load_obj(obj)
    faces = G.get("PaintBrush3") or next(iter(G.values()))
    P = np.array([V[a] for f in faces for a, _, _ in f])
    c = (P.min(0) + P.max(0)) / 2
    scale = 15.0 / (P.max(0) - P.min(0)).max()  # ~15 units (21 cm) long
    zmin = (P[:, 2].min() - c[2]) * scale

    def xf(p):  # lies along X, on the floor (proper rotation: (x, y, z) -> (y, -x, z))
        q = (np.asarray(p) - c) * scale
        return (q[1], -q[0], q[2] - zmin)

    rot = lambda d: (d[1], -d[0], d[2])
    tex = bake_ao(os.path.join(texdir, "PaintBrush3Albedo.png"), os.path.join(texdir, "PaintBrushAO.png"))
    save_dds_dxt1(tex, os.path.join(TEXTURES, "brush.dds"), 1024)
    m = make_mesh("SPBrush", TEX + "brush.dds", V, VT, VN, faces, xf, rot)
    write("Item_Paint", [m], collision([m], dynamic=True, mass=0.3))
    img = ui_picture(m, tex, os.path.join(UI, "brush.png"), 34.0)
    a = np.asarray(img)[..., 3] > 0
    ys, xs = np.nonzero(a)
    i = int(np.argmax(xs - ys))  # the bristle tip: the top-right end
    global BRUSH_TIP
    BRUSH_TIP = (xs[i] / img.size[0], ys[i] / img.size[1], img.size[0] / img.size[1])
    print(f"  UI/brush.png {img.size}, tip at {BRUSH_TIP[:2]}")


def palette(obj, base_png, ao_png):
    V, VT, VN, G = load_obj(obj)
    faces = [f for g in G.values() for f in g]
    P = np.array([V[a] for f in faces for a, _, _ in f])
    cx, cz, y0 = (P[:, 0].min() + P[:, 0].max()) / 2, (P[:, 2].min() + P[:, 2].max()) / 2, P[:, 1].min()

    def xf(p):  # lies flat, top face up
        x, y, z = (p[0] - cx) * CM, (p[1] - y0) * CM, (p[2] - cz) * CM
        return (-x, z, y)

    rot = lambda d: (-d[0], d[2], d[1])
    tex = bake_ao(base_png, ao_png)
    save_dds_dxt1(tex, os.path.join(TEXTURES, "palette.dds"), 1024)
    m = make_mesh("SPPalette", TEX + "palette.dds", V, VT, VN, faces, xf, rot)
    write("Item_Palette", [m], collision([m], dynamic=True, mass=0.5))
    top = nw.Mesh("top", "", "")
    top.verts, top.tris = m.verts, [t for t in m.tris if np.mean([m.verts[i][2][2] for i in t]) > 0.7]
    img = ui_picture(top, tex, os.path.join(UI, "palette.png"), 22.0, light=(0.2, 0.3, 0.93))
    print(f"  UI/palette.png {img.size}")
    spots(img)


def spots(img):
    """Where the paints sit on the palette picture: evenly round the rim, away from the thumb hole."""
    import cv2
    a = (np.asarray(img)[..., 3] > 0).astype(np.uint8)
    H, W = a.shape
    dist = cv2.distanceTransform(a, cv2.DIST_L2, 5)
    inset = 0.09 * min(W, H)
    ys, xs = np.nonzero(np.abs(dist - inset) < 1.5)
    cy, cx = H / 2, W / 2
    ang = np.arctan2(ys - cy, xs - cx)
    # the thumb hole: the hole contour inside the outline
    cnts, hier = cv2.findContours(a, cv2.RETR_CCOMP, cv2.CHAIN_APPROX_NONE)
    hole_c = None
    if hier is not None:
        holes = [cnts[i] for i in range(len(cnts)) if hier[0][i][3] >= 0]
        if holes:
            M = cv2.moments(max(holes, key=cv2.contourArea))
            if M["m00"]:
                hole_c = (M["m10"] / M["m00"], M["m01"] / M["m00"])
    picks = []
    for target in np.linspace(-math.pi, math.pi, 96, endpoint=False):
        i = int(np.argmin(np.abs(np.angle(np.exp(1j * (ang - target))))))
        x, y = int(xs[i]), int(ys[i])
        if hole_c and math.hypot(x - hole_c[0], y - hole_c[1]) < 0.17 * min(W, H):
            continue
        if all(math.hypot(x - px, y - py) > 0.125 * min(W, H) for px, py in picks):
            picks.append((x, y))
    picks = picks[:12]
    print(f"  {len(picks)} paint spots on the palette (thumb hole at {hole_c})")
    L = ["// Generated by tools/convert_models.py: where the paints sit on UI/palette.png (0..1 of its size).", "#pragma once", "",
         "namespace PaletteSpots", "{", f"\tinline constexpr int kCount = {len(picks)};", "\tinline constexpr float kSpots[kCount][2] = {"]
    L += [f"\t\t{{ {x / W:.4f}f, {y / H:.4f}f }}," for x, y in picks]
    L += ["\t};", f"\tinline constexpr float kAspect = {W / H:.4f}f;  // width / height of the picture",
          f"\t// UI/brush.png: the bristle tip (0..1 of its size) and width / height",
          f"\tinline constexpr float kBrushTip[2] = {{ {BRUSH_TIP[0]:.4f}f, {BRUSH_TIP[1]:.4f}f }};",
          f"\tinline constexpr float kBrushAspect = {BRUSH_TIP[2]:.4f}f;", "}", ""]
    open(os.path.join(ROOT, "src", "PaletteSpots.h"), "w").write("\n".join(L))


def main():
    if len(sys.argv) != 8:
        raise SystemExit(__doc__)
    info = easel_and_canvas(sys.argv[1], sys.argv[2])
    brush(sys.argv[3], sys.argv[4])
    palette(sys.argv[5], sys.argv[6], sys.argv[7])
    json.dump({"bounds": BOUNDS, "canvas": info}, open(os.path.join(ROOT, "tools", "bounds.json"), "w"), indent=1)
    print("canvas:", {k: round(v, 1) for k, v in info.items()})


if __name__ == "__main__":
    main()
