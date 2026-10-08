#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lexend for the in-game menus, which ImGui draws (port/app/side_menu.h): three static weights.

    render-menu-fonts.py [OUTPUT_DIR]     default: port/ui/fonts

The launcher draws Lexend from its signed-distance atlas (tools/render-sdf-font.sh); the menus over
a game draw with ImGui, whose TrueType reader sees only a variable font's default instance. So the
variable font (tools/fonts, SIL Open Font License 1.1) is cut at Regular (400), Medium (500) and
SemiBold (600), to the characters the menus ask ImGui for (Latin-1 and the typographic marks), and
without hinting. The files are in the repository, so a build needs no font tools; run this again
only to change them. It needs fontTools (pip install fonttools).
"""

import pathlib
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / "tools/fonts/Lexend[wght].ttf"
WEIGHTS = {"Regular": 400, "Medium": 500, "SemiBold": 600}
# what the menus' glyph ranges ask for (port/app/menu_canvas.h, kGlyphRanges)
UNICODES = list(range(0x20, 0x100)) + list(range(0x2010, 0x2028)) + [0x2039, 0x203A]


def main():
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "port/ui/fonts"
    out.mkdir(parents=True, exist_ok=True)
    for name, weight in WEIGHTS.items():
        font = instancer.instantiateVariableFont(TTFont(SOURCE), {"wght": weight})
        options = subset.Options()
        options.hinting = False
        options.layout_features = ["kern"]
        options.name_IDs = ["*"]
        options.notdef_outline = True
        cut = subset.Subsetter(options)
        cut.populate(unicodes=UNICODES)
        cut.subset(font)
        path = out / f"Lexend-{name}.ttf"
        font.save(path)
        print(f"{path}: {path.stat().st_size // 1024} KiB")


if __name__ == "__main__":
    main()
