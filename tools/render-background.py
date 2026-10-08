#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draw the launcher's background as a still picture, with nothing but the standard library.

    render-background.py OUTPUT_DIR

writes ui/background-menu.tga (2048x1152) and ui/background.tga (1920x1080). The home screen
tile, its background and the README's banner are drawn on it (render-icons.py,
render-presentation.py and render-banner.py use its particles); the launcher draws it moving,
its bubbles rising (port/frontend/shell/shell.cpp). It is the Wii U Homebrew Launcher's background, as Dimok's
homebrew_launcher draws it (src/menu/MainWindow.cpp, with libgui's GuiParticleImage; both
GPL-3.0-or-later). A blue gradient, (59, 159, 223) at the top to (79, 153, 239) at the bottom,
and 500 white discs of radius up to 30 and alpha 0.05 to 0.65 on its 1280x720 screen, here
scaled to each image and placed from a fixed seed, so every build draws the same picture.
Black at OVERLAY's opacity covers it, so the launcher's white text and artwork stand out.
"""

import math
import os
import random
import struct
import sys

TOP = (59, 159, 223)
BOTTOM = (79, 153, 239)
PARTICLES = 500
MAX_RADIUS = 30.0  # in the Homebrew Launcher's 1280x720 pixels
SEED = 0x5735
OVERLAY = 0.6  # the dark overlay's opacity


def particles():
    """Each disc as (x, y, radius) in units of the screen's width and height, and its alpha."""
    rng = random.Random(SEED)
    out = []
    for _ in range(PARTICLES):
        x, y = rng.random(), rng.random()
        alpha = rng.random() * 0.6 + 0.05
        radius = rng.random() * MAX_RADIUS / 1280.0
        out.append((x, y, radius, alpha))
    return out


def render(width, height):
    """Rows of BGRA pixels, top-down."""
    pixels = bytearray(width * height * 4)
    for y in range(height):
        t = y / (height - 1)
        colour = bytes(round(TOP[2 - i] + (BOTTOM[2 - i] - TOP[2 - i]) * t) for i in range(3)) + b"\xff"
        pixels[y * width * 4:(y + 1) * width * 4] = colour * width
    for x, y, radius, alpha in particles():
        cx, cy, r = x * width, y * height, radius * width
        if r < 0.5:
            continue
        for py in range(max(0, int(cy - r - 1)), min(height, int(cy + r + 2))):
            for px in range(max(0, int(cx - r - 1)), min(width, int(cx + r + 2))):
                # edges anti-aliased over a pixel
                coverage = min(max(r - math.hypot(px + 0.5 - cx, py + 0.5 - cy) + 0.5, 0.0), 1.0)
                if coverage <= 0.0:
                    continue
                a = alpha * coverage
                o = (py * width + px) * 4
                for c in range(3):
                    pixels[o + c] = round(pixels[o + c] + (255 - pixels[o + c]) * a)
    pixels = bytearray(pixels.translate(bytes(round(v * (1.0 - OVERLAY)) for v in range(256))))
    pixels[3::4] = b"\xff" * (width * height)  # still opaque
    return pixels


def write_tga(path, width, height, pixels):
    # uncompressed true colour, 32 bits, 8 of alpha, top-down: what the launcher reads (ui/images.cpp)
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, width, height, 32, 0x28)
    with open(path, "wb") as out:
        out.write(header + bytes(pixels))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = os.path.join(sys.argv[1], "ui")
    os.makedirs(out, exist_ok=True)
    write_tga(os.path.join(out, "background-menu.tga"), 2048, 1152, render(2048, 1152))
    write_tga(os.path.join(out, "background.tga"), 1920, 1080, render(1920, 1080))


if __name__ == "__main__":
    main()
