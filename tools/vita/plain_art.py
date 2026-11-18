#!/usr/bin/env python3
"""Draws the plain icon and LiveArea images that the repository and a shared VPK carry (no EA art).

Writes sce_sys/icon0.png (128x128), sce_sys/livearea/contents/bg.png (840x500) and startup.png (280x158) as 8-bit
palette PNGs, plus assets/logo.png (256x256, for the README). The game's name is drawn as text with a built-in 5x7
pixel font on a dark carbon-weave pattern. livearea_art.py makes the logo versions from the player's own game files.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from livearea_art import check_image, quantise, write_png  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))

# 5x7 glyphs, rows top to bottom, '#' = on.
FONT = {
    'A': ['.###.', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'],
    'B': ['####.', '#...#', '#...#', '####.', '#...#', '#...#', '####.'],
    'C': ['.###.', '#...#', '#....', '#....', '#....', '#...#', '.###.'],
    'D': ['####.', '#...#', '#...#', '#...#', '#...#', '#...#', '####.'],
    'E': ['#####', '#....', '#....', '####.', '#....', '#....', '#####'],
    'F': ['#####', '#....', '#....', '####.', '#....', '#....', '#....'],
    'I': ['#####', '..#..', '..#..', '..#..', '..#..', '..#..', '#####'],
    'N': ['#...#', '##..#', '#.#.#', '#..##', '#...#', '#...#', '#...#'],
    'O': ['.###.', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'],
    'P': ['####.', '#...#', '#...#', '####.', '#....', '#....', '#....'],
    'R': ['####.', '#...#', '#...#', '####.', '#.#..', '#..#.', '#...#'],
    'S': ['.####', '#....', '#....', '.###.', '....#', '....#', '####.'],
    'T': ['#####', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'],
    'V': ['#...#', '#...#', '#...#', '#...#', '#...#', '.#.#.', '..#..'],
    ' ': ['.....'] * 7,
}

BACK_DARK = (0x0b, 0x0d, 0x10)
BACK_LIGHT = (0x17, 0x1a, 0x1f)
WHITE = (0xf2, 0xf4, 0xf7)
ORANGE = (0xff, 0x7a, 0x1a)
GREY = (0x9a, 0xa3, 0xad)
SS = 4      # supersampling per axis


class Canvas:
    def __init__(self, width, height, weave):
        self.w, self.h = width, height
        self.px = []
        for y in range(height):
            for x in range(width):
                # Carbon twill: 2x2 blocks of diagonal stripes, alternating direction.
                cell = (x // weave + y // weave) % 2
                stripe = ((x + y) if cell else (x - y)) % weave < weave // 2
                base = BACK_LIGHT if stripe else BACK_DARK
                # Vignette towards the edges.
                dx, dy = (x - width / 2) / width, (y - height / 2) / height
                k = max(0.45, 1.0 - 1.1 * (dx * dx + dy * dy))
                self.px.append(tuple(c * k for c in base))

    def blend(self, x, y, colour, cover):
        i = y * self.w + x
        self.px[i] = tuple(a + (b - a) * cover for a, b in zip(self.px[i], colour))

    def rect(self, x0, y0, x1, y1, colour):
        """Antialiased filled rectangle in float pixel coordinates."""
        for y in range(max(0, int(y0)), min(self.h, int(y1) + 1)):
            for x in range(max(0, int(x0)), min(self.w, int(x1) + 1)):
                hits = 0
                for sy in range(SS):
                    for sx in range(SS):
                        px, py = x + (sx + 0.5) / SS, y + (sy + 0.5) / SS
                        hits += x0 <= px < x1 and y0 <= py < y1
                if hits:
                    self.blend(x, y, colour, hits / (SS * SS))

    def text(self, s, cx, top, cell, colour, slant=0.0, gap=1):
        """Centred text, each font pixel a cell x cell square; slant shifts rows right towards the top. Coverage is
        sampled per sub-pixel against the whole string, so neighbouring font pixels join without seams."""
        advance = 5 + gap
        width = (len(s) * advance - gap) * cell
        left = cx - width / 2
        bottom = top + 7 * cell

        def on(px, py):
            if not top <= py < bottom:
                return False
            # Undo the slant (continuous, so diagonal edges stay straight).
            u = (px - left - slant * (bottom - py)) / cell
            v = int((py - top) / cell)
            if u < 0:
                return False
            index, col = divmod(int(u), advance)
            if index >= len(s) or col >= 5:
                return False
            return FONT[s[index]][v][col] == '#'

        x0 = int(left) - 1
        x1 = int(left + width + slant * 7 * cell) + 2
        for y in range(max(0, int(top)), min(self.h, int(bottom) + 1)):
            for x in range(max(0, x0), min(self.w, x1)):
                hits = sum(on(x + (sx + 0.5) / SS, y + (sy + 0.5) / SS) for sy in range(SS) for sx in range(SS))
                if hits:
                    self.blend(x, y, colour, hits / (SS * SS))

    def save(self, path):
        pixels = [tuple(min(255, max(0, round(c))) for c in p) + (255,) for p in self.px]
        palette, indices = quantise(pixels)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        write_png(path, self.w, self.h, palette, indices)
        problem = check_image(path, self.w, self.h)
        if problem:
            raise SystemExit('%s: %s' % (path, problem))
        print('wrote %s (%dx%d, %d colours)' % (os.path.relpath(path, ROOT), self.w, self.h, len(palette)))


def icon(size, path):
    c = Canvas(size, size, max(4, size // 16))
    cell = size / 44.0
    c.text('NFS', size / 2 - cell * 0.6, size * 0.14, cell * 1.5, GREY, slant=0.15)
    c.text('CARBON', size / 2 - cell * 0.5, size * 0.40, cell * 1.0, WHITE, slant=0.15)
    c.rect(size * 0.16, size * 0.62, size * 0.84, size * 0.62 + cell * 1.1, ORANGE)
    c.text('VITA', size / 2 - cell * 0.6, size * 0.69, cell * 1.5, ORANGE, slant=0.15)
    c.save(path)


def background(path):
    w, h = 840, 500
    c = Canvas(w, h, 20)
    c.text('NEED FOR SPEED', w / 2, 120, 5, GREY, slant=0.15)
    c.text('CARBON', w / 2, 175, 16, WHITE, slant=0.15)
    c.rect(170, 305, 670, 311, ORANGE)
    c.text('PS VITA PORT', w / 2, 335, 6, ORANGE, slant=0.15)
    c.save(path)


def startup(path):
    w, h = 280, 158
    c = Canvas(w, h, 10)
    c.text('CARBON', w / 2, 38, 6, WHITE, slant=0.15)
    c.rect(55, 92, 225, 95, ORANGE)
    c.text('START', w / 2, 106, 3, ORANGE, slant=0.15)
    c.save(path)


def main():
    icon(128, os.path.join(ROOT, 'sce_sys', 'icon0.png'))
    background(os.path.join(ROOT, 'sce_sys', 'livearea', 'contents', 'bg.png'))
    startup(os.path.join(ROOT, 'sce_sys', 'livearea', 'contents', 'startup.png'))
    icon(256, os.path.join(ROOT, 'assets', 'logo.png'))


if __name__ == '__main__':
    main()
