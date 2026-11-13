#!/usr/bin/env python3
"""Generates SkyrimPainter.esp (ESL-flagged, master Skyrim.esm) and src/Forms.h.

Records (local form IDs, found by the plugin with TESDataHandler::LookupForm(id, "SkyrimPainter.esp")):
  MISC  0x800 Blank Canvas, 0x801 Easel, 0x802 Painter's Palette, 0x803 Paints, 0x804 Painting
  ACTI  0x810 standing easel (with canvas), 0x820 + i framed painting style i
  COBJ  0x830.. recipes at the tanning rack: 1 Leather each (test values)

Run tools/meshes.py first (it writes tools/bounds.json).
Usage: python3 tools/make_esp.py
"""
import json, math, os, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from meshes import FRAME_STYLES

PLUGIN = "SkyrimPainter.esp"
OWN = 0x01000000          # one master (Skyrim.esm) -> our own records are 01xxxxxx in the file
LEATHER = 0x000DB5D2      # Leather01
TANNING_RACK = 0x0007866A  # CraftingTanningRack keyword (the plugin double-checks it at runtime)

ID_CANVAS, ID_EASEL_ITEM, ID_PALETTE, ID_PAINTS, ID_PAINTING = 0x800, 0x801, 0x802, 0x803, 0x804
ID_EASEL_STAND = 0x810
ID_FRAME_FIRST = 0x820
ID_RECIPE_FIRST = 0x830

LICENSE_HEADER = """// SkyrimPainter - paint your own pictures in Skyrim Special Edition
// Copyright (C) 2026 evodube
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with this program
// (LICENSE.txt). If not, see <https://www.gnu.org/licenses/>.
"""


def sub(tag, data): return tag.encode() + struct.pack("<H", len(data)) + data
def zstr(s): return s.encode("cp1252") + b"\0"
def record(tag, formid, payload, flags=0):
    return tag.encode() + struct.pack("<IIIIHH", len(payload), flags, formid, 0, 44, 0) + payload
def group(label, payload):
    return b"GRUP" + struct.pack("<I4sIIHH", 24 + len(payload), label.encode(), 0, 0, 0, 0) + payload


def obnd(bounds, stem):
    b = bounds[stem]
    return sub("OBND", struct.pack("<6h", *[int(math.floor(v)) for v in b["min"]], *[int(math.ceil(v)) for v in b["max"]]))


def main():
    bounds = json.load(open(os.path.join(ROOT, "tools", "bounds.json")))
    count = 0

    acti = b""
    acti += record("ACTI", OWN | ID_EASEL_STAND,
                   sub("EDID", zstr("SP_EaselStand")) + obnd(bounds, "Easel") + sub("FULL", zstr("Easel")) +
                   sub("MODL", zstr("SkyrimPainter\\Easel.nif")) + sub("RNAM", zstr("Paint")))
    count += 1
    for i, (stem, name, _) in enumerate(FRAME_STYLES):
        acti += record("ACTI", OWN | (ID_FRAME_FIRST + i),
                       sub("EDID", zstr("SP_Painting_" + stem)) + obnd(bounds, "Painting_" + stem) +
                       sub("FULL", zstr("Painting")) + sub("MODL", zstr(f"SkyrimPainter\\Painting_{stem}.nif")) +
                       sub("RNAM", zstr("Examine")))
        count += 1

    items = [
        (ID_CANVAS, "SP_Canvas", "Blank Canvas", "Item_Canvas", 8, 1.0),
        (ID_EASEL_ITEM, "SP_Easel", "Easel", "Item_Easel", 15, 5.0),
        (ID_PALETTE, "SP_Palette", "Painter's Palette", "Item_Palette", 10, 0.5),
        (ID_PAINTS, "SP_Paints", "Paints", "Item_Paint", 20, 0.5),
        (ID_PAINTING, "SP_Painting", "Painting", "Painting_DarkOak", 50, 2.0),
    ]
    misc = b""
    for fid, edid, name, model, value, weight in items:
        misc += record("MISC", OWN | fid,
                       sub("EDID", zstr(edid)) + obnd(bounds, model) + sub("FULL", zstr(name)) +
                       sub("MODL", zstr(f"SkyrimPainter\\{model}.nif")) + sub("DATA", struct.pack("<If", value, weight)))
        count += 1

    cobj = b""
    for i, (fid, edid, _, _, _, _) in enumerate(items[:4]):  # everything but the finished painting
        cobj += record("COBJ", OWN | (ID_RECIPE_FIRST + i),
                       sub("EDID", zstr("SP_Recipe" + edid[3:])) + sub("COCT", struct.pack("<I", 1)) +
                       sub("CNTO", struct.pack("<Ii", LEATHER, 1)) + sub("CNAM", struct.pack("<I", OWN | fid)) +
                       sub("BNAM", struct.pack("<I", TANNING_RACK)) + sub("NAM1", struct.pack("<H", 1)))
        count += 1

    next_id = ID_RECIPE_FIRST + 4
    tes4 = (sub("HEDR", struct.pack("<fiI", 1.70, count + 3, next_id)) + sub("CNAM", zstr("evodube")) +
            sub("SNAM", zstr("SkyrimPainter - paint on a canvas at your own easel (needs SkyrimPainter.dll)")) +
            sub("MAST", zstr("Skyrim.esm")) + sub("DATA", b"\0" * 8))
    esp = record("TES4", 0, tes4, flags=0x200) + group("ACTI", acti) + group("MISC", misc) + group("COBJ", cobj)
    open(os.path.join(ROOT, PLUGIN), "wb").write(esp)

    L = [LICENSE_HEADER, "#pragma once", "// Generated by tools/make_esp.py - keep in sync with SkyrimPainter.esp.", "",
         "namespace Forms", "{",
         f'\tinline constexpr std::string_view kPlugin = "{PLUGIN}"sv;',
         f"\tinline constexpr std::uint32_t kCanvas = 0x{ID_CANVAS:X};",
         f"\tinline constexpr std::uint32_t kEaselItem = 0x{ID_EASEL_ITEM:X};",
         f"\tinline constexpr std::uint32_t kPalette = 0x{ID_PALETTE:X};",
         f"\tinline constexpr std::uint32_t kPaints = 0x{ID_PAINTS:X};",
         f"\tinline constexpr std::uint32_t kPainting = 0x{ID_PAINTING:X};",
         f"\tinline constexpr std::uint32_t kEaselStand = 0x{ID_EASEL_STAND:X};",
         f"\tinline constexpr std::uint32_t kFrameFirst = 0x{ID_FRAME_FIRST:X};",
         f"\tinline constexpr std::uint32_t kRecipeFirst = 0x{ID_RECIPE_FIRST:X};",
         "\tinline constexpr int           kRecipeCount = 4;",
         f"\tinline constexpr RE::FormID    kLeather = 0x{LEATHER:08X};",
         f"\tinline constexpr RE::FormID    kTanningRackKeyword = 0x{TANNING_RACK:08X};",
         "",
         "\t// Painting on the wall: canvas size in game units (4:3, the painting is 1024 x 768).",
         f"\tinline constexpr float kCanvasW = 40.0f;",
         f"\tinline constexpr float kCanvasH = 30.0f;",
         "",
         f"\tinline constexpr int kFrameCount = {len(FRAME_STYLES)};",
         "\tinline constexpr const char* kFrameNames[kFrameCount] = {",
         ] + [f'\t\t"{name}",' for _, name, _ in FRAME_STYLES] + ["\t};", "}", ""]
    open(os.path.join(ROOT, "src", "Forms.h"), "w").write("\n".join(L))
    print(f"wrote {PLUGIN} ({len(esp)} bytes, {count} records) and src/Forms.h")


if __name__ == "__main__":
    main()
