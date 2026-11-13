#!/usr/bin/env python3
"""Builds SkyrimPainter's meshes (SSE NIF, bsver 100) from simple boxes and beams into
Data/meshes/SkyrimPainter/, and writes tools/bounds.json (object bounds for make_esp.py).

Conventions (same as Hearthframe): Z up, the front of a canvas faces +Y.
- Easel.nif / Item_Easel.nif / Item_Paint.nif: made by tools/convert_models.py from third-party
  models (see CREDITS.txt). The easel's canvas front is the shape "SPCanvas".
- Painting_<Style>.nif: framed painting for walls, origin at the centre of the frame's back,
  painting surface "SPCanvas".
- Placeholder item models: Item_Canvas, Item_Palette.
No collision on purpose: the plugin finds easels and paintings by aim itself, and dropped items
are handled by the plugin straight away.

Usage: python3 tools/meshes.py
"""
import json, math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nif import write_nif

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "Data", "meshes", "SkyrimPainter")
TEX = "textures\\SkyrimPainter\\"
NORMAL = TEX + "flat_n.dds"
DESC = 0x0003B00007650408  # pos+bitanX(f32) uv(f16) normal+bitanY(u8) tangent+bitanZ(u8) colour(u8) - 32 bytes
SF1 = 0x80000301  # ZBuffer_Test | Cast_Shadows | Recieve_Shadows | Specular
SF2 = 0x00008031  # EnvMap_Light_Fade | Vertex_Colors | Double_Sided | ZBuffer_Write

FRAME_STYLES = [("DarkOak", "Dark Oak", "frame_darkoak"), ("HoneyOak", "Honey Oak", "frame_honeyoak"),
                ("Walnut", "Walnut", "frame_walnut"), ("Ebony", "Ebony", "frame_ebony"),
                ("Birch", "Birch", "frame_birch"), ("Gilded", "Gilded", "frame_gilded")]
# painting on the wall: 4:3 canvas (matches the 1024 x 768 painting)
CANVAS_W, CANVAS_H = 40.0, 30.0


def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def mul(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def norm(a):
    l = math.sqrt(dot(a, a)) or 1.0
    return (a[0] / l, a[1] / l, a[2] / l)


class Shape:
    def __init__(self, name, texture):
        self.name, self.texture = name, texture
        self.v = []  # (pos, normal, tangent, bitangent, uv)
        self.t = []

    def quad(self, p0, p1, p2, p3, uv=((0, 1), (1, 1), (1, 0), (0, 0)), outside=None):
        """p0 bottom-left, p1 bottom-right, p2 top-right, p3 top-left as seen from the front
        (counter-clockwise). a_outside: a point behind the face (e.g. the solid's centre); the
        winding is flipped if the face would point towards it."""
        n = norm(cross(sub(p1, p0), sub(p3, p0)))
        if outside is not None:
            c = mul(add(add(p0, p1), add(p2, p3)), 0.25)
            if dot(n, sub(c, outside)) < 0:
                p0, p1, p2, p3 = p1, p0, p3, p2
                uv = (uv[1], uv[0], uv[3], uv[2])
                n = mul(n, -1)
        tg = norm(sub(p1, p0))  # +u
        bt = norm(sub(p0, p3))  # +v runs downwards (v = 0 at the top of a DDS)
        base = len(self.v)
        for p, t in zip((p0, p1, p2, p3), uv):
            self.v.append((p, n, tg, bt, t))
        self.t += [(base, base + 1, base + 2), (base, base + 2, base + 3)]

    def box(self, mn, mx, tile=48.0, faces="xXyYzZ"):
        x0, y0, z0 = mn
        x1, y1, z1 = mx
        c = ((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2)
        def uvr(a, b): return ((0, b / tile), (a / tile, b / tile), (a / tile, 0), (0, 0))
        dx, dy, dz = x1 - x0, y1 - y0, z1 - z0
        # each face: bottom-left, bottom-right, top-right, top-left seen from outside
        if "Y" in faces: self.quad((x1, y1, z0), (x0, y1, z0), (x0, y1, z1), (x1, y1, z1), uvr(dx, dz), c)  # front (+Y)
        if "y" in faces: self.quad((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1), uvr(dx, dz), c)  # back
        if "X" in faces: self.quad((x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1), uvr(dy, dz), c)
        if "x" in faces: self.quad((x0, y1, z0), (x0, y0, z0), (x0, y0, z1), (x0, y1, z1), uvr(dy, dz), c)
        if "Z" in faces: self.quad((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1), uvr(dx, dy), c)
        if "z" in faces: self.quad((x0, y1, z0), (x1, y1, z0), (x1, y0, z0), (x0, y0, z0), uvr(dx, dy), c)

    def beam(self, a, b, w, d=None, tile=48.0):
        """Square-ish timber from a to b (w wide, d deep), wood grain along its length."""
        d = d or w
        ax = norm(sub(b, a))
        up = (0, 0, 1) if abs(ax[2]) < 0.9 else (1, 0, 0)
        u = norm(cross(ax, up))
        v = norm(cross(u, ax))
        L = math.sqrt(dot(sub(b, a), sub(b, a)))
        def c(p, su, sv): return add(add(p, mul(u, su * w / 2)), mul(v, sv * d / 2))
        A = [c(a, -1, -1), c(a, 1, -1), c(a, 1, 1), c(a, -1, 1)]
        B = [c(b, -1, -1), c(b, 1, -1), c(b, 1, 1), c(b, -1, 1)]
        mid = mul(add(a, b), 0.5)
        for i in range(4):
            j = (i + 1) % 4
            uvs = ((0, L / tile), (w / tile, L / tile), (w / tile, 0), (0, 0))
            self.quad(A[i], A[j], B[j], B[i], uvs, mid)
        self.quad(A[3], A[2], A[1], A[0], ((0, 0.1), (0.1, 0.1), (0.1, 0), (0, 0)), mid)
        self.quad(B[0], B[1], B[2], B[3], ((0, 0.1), (0.1, 0.1), (0.1, 0), (0, 0)), mid)


# ------------------------------------------------------------------ NIF writing
def half(x): return struct.pack("<e", x)
def nb(v): return max(0, min(255, int(round((v + 1.0) * 127.5))))


def avobj(name_idx, children=None):
    b = struct.pack("<iIi", name_idx, 0, -1)          # name, no extra data, no controller
    b += struct.pack("<I", 14)                        # flags
    b += struct.pack("<3f", 0, 0, 0)                  # translation
    b += struct.pack("<9f", 1, 0, 0, 0, 1, 0, 0, 0, 1)  # rotation
    b += struct.pack("<f", 1.0)                       # scale
    b += struct.pack("<i", -1)                        # collision
    return b


def tri_shape(name_idx, shader_idx, s):
    n = len(s.v)
    vd = bytearray()
    for p, nr, tg, bt, uv in s.v:
        vd += struct.pack("<4f", *p, bt[0])
        vd += half(uv[0]) + half(uv[1])
        vd += bytes([nb(nr[0]), nb(nr[1]), nb(nr[2]), nb(bt[1])])
        vd += bytes([nb(tg[0]), nb(tg[1]), nb(tg[2]), nb(bt[2])])
        vd += bytes([255, 255, 255, 255])
    tris = b"".join(struct.pack("<3H", *t) for t in s.t)
    P = [v[0] for v in s.v]
    cx, cy, cz = (sum(p[i] for p in P) / n for i in range(3))
    rad = max(math.dist((cx, cy, cz), p) for p in P)
    b = avobj(name_idx)
    b += struct.pack("<4f", cx, cy, cz, rad)
    b += struct.pack("<iii", -1, shader_idx, -1)      # skin, shader, alpha
    b += struct.pack("<QHHI", DESC, len(s.t), n, len(vd) + len(tris))
    b += bytes(vd) + tris + struct.pack("<I", 0)      # particle data size
    return b


def lighting_shader(texset_idx):
    b = struct.pack("<IiIi", 0, -1, 0, -1)            # shader type Default, name, no extra, no controller
    b += struct.pack("<II", SF1, SF2)
    b += struct.pack("<4f", 0, 0, 1, 1)               # uv offset, uv scale
    b += struct.pack("<i", texset_idx)
    b += struct.pack("<3ff", 0, 0, 0, 1.0)            # emissive colour, multiple
    b += struct.pack("<I", 3)                         # texture clamp: WRAP_S_WRAP_T
    b += struct.pack("<fff", 1.0, 1.0, 30.0)          # alpha, refraction, glossiness
    b += struct.pack("<3ff", 1, 1, 1, 0.3)            # specular colour, strength
    b += struct.pack("<ff", 0.3, 2.0)                 # lighting effect 1, 2
    return b


def texture_set(diffuse):
    paths = [diffuse, NORMAL, "", "", "", "", "", "", ""]
    b = struct.pack("<i", len(paths))
    for p in paths:
        b += struct.pack("<I", len(p)) + p.encode("ascii")
    return b


def write_mesh(path, root_name, shapes):
    strings = [root_name] + [s.name for s in shapes]
    blocks = [None]
    children = []
    for i, s in enumerate(shapes):
        si = len(blocks)
        blocks += [None, None, None]
        blocks[si] = ["BSTriShape", tri_shape(1 + i, si + 1, s)]
        blocks[si + 1] = ["BSLightingShaderProperty", lighting_shader(si + 2)]
        blocks[si + 2] = ["BSShaderTextureSet", texture_set(s.texture)]
        children.append(si)
    root = avobj(0) + struct.pack("<I", len(children)) + b"".join(struct.pack("<i", c) for c in children) + struct.pack("<I", 0)
    blocks[0] = ["BSFadeNode", root]
    h = {"line": b"Gamebryo File Format, Version 20.2.0.7\n", "ver": 0x14020007, "endian": 1, "user": 12, "bsver": 100,
         "author": b"SkyrimPainter\x00", "proc": b"\x00", "exp": b"\x00"}
    os.makedirs(os.path.dirname(path), exist_ok=True)
    write_nif(path, h, blocks, strings, [0])
    P = [v[0] for s in shapes for v in s.v]
    return [min(p[i] for p in P) for i in range(3)], [max(p[i] for p in P) for i in range(3)]


# ------------------------------------------------------------------ models
def painting(tex):
    w, h = CANVAS_W / 2, CANVAS_H / 2
    b, depth, lip = 3.5, 3.0, 0.8
    frame = Shape("SPFrame", TEX + tex + ".dds")
    # four mitred-looking boards (boxes) around the canvas; back at y = 0
    frame.box((-w - b, 0, h), (w + b, depth, h + b), tile=32.0)        # top
    frame.box((-w - b, 0, -h - b), (w + b, depth, -h), tile=32.0)      # bottom
    frame.box((-w - b, 0, -h), (-w, depth, h), tile=32.0)              # left
    frame.box((w, 0, -h), (w + b, depth, h), tile=32.0)                # right
    # inner bevel lip stepping down to the canvas
    frame.box((-w, depth - lip, h - 0.8), (w, depth, h), tile=32.0, faces="Yz")
    frame.box((-w, depth - lip, -h), (w, depth, -h + 0.8), tile=32.0, faces="YZ")
    frame.box((-w, depth - lip, -h + 0.8), (-w + 0.8, depth, h - 0.8), tile=32.0, faces="YX")
    frame.box((w - 0.8, depth - lip, -h + 0.8), (w, depth, h - 0.8), tile=32.0, faces="Yx")
    back = Shape("SPBacking", TEX + "canvas_blank.dds")
    back.box((-w, 0.0, -h), (w, 0.6, h), tile=64.0, faces="y")
    canvas = Shape("SPCanvas", TEX + "canvas_blank.dds")
    y = depth - lip - 0.05
    canvas.quad((w, y, -h), (-w, y, -h), (-w, y, h), (w, y, h))  # seen from +Y, the picture's left is +X
    return [frame, back, canvas]


def item_canvas():
    edge = Shape("SPCanvasEdge", TEX + "canvas_blank.dds")
    edge.box((-14, -0.75, 0), (14, 0.73, 21), tile=64.0, faces="xXyzZ")
    front = Shape("SPCanvas", TEX + "canvas_blank.dds")
    front.quad((14, 0.75, 0), (-14, 0.75, 0), (-14, 0.75, 21), (14, 0.75, 21))
    return [edge, front]


def item_palette():
    pal = Shape("SPPalette", TEX + "palette.dds")
    w, d, t = 13.0, 10.0, 0.8
    pal.box((-w, -d, 0), (w, d, t), tile=1.0, faces="xXyYz")
    pal.quad((-w, -d, t), (w, -d, t), (w, d, t), (-w, d, t), ((0, 1), (1, 1), (1, 0), (0, 0)), (0, 0, 0))
    return [pal]


def main():
    path = os.path.join(ROOT, "tools", "bounds.json")
    bounds = json.load(open(path)) if os.path.exists(path) else {}  # keeps the converted models' entries
    # Easel, Item_Easel and Item_Paint come from the third-party models (tools/convert_models.py)
    models = {"Item_Canvas": item_canvas(), "Item_Palette": item_palette()}
    for stem, _, tex in FRAME_STYLES:
        models["Painting_" + stem] = painting(tex)
    for stem, shapes in models.items():
        mn, mx = write_mesh(os.path.join(OUT, stem + ".nif"), "SkyrimPainter_" + stem, shapes)
        bounds[stem] = {"min": mn, "max": mx}
    json.dump(bounds, open(path, "w"), indent=1)
    print(f"{len(models)} meshes written to {OUT}")


if __name__ == "__main__":
    main()
