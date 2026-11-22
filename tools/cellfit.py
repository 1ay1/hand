#!/usr/bin/env python3
# tools/cellfit.py — where glyphs sit in their cells, measured from a frame.
#
#   hand --png rows.png --shot rows   (inverse cells: HxgM |_^Ag / HHHH)
#   tools/cellfit.py rows.png
#
# For each inverse-video cell row it finds the cell's box (bright background)
# and the ink inside it (dark glyph pixels), and prints the space above and
# below the ink. For capitals (H, M) a centred glyph has equal space above
# its cap height and below its baseline... measured against the box, which
# is the line the terminal reserves. Pass --all to dump every row's profile.
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert('RGB')
W, H = im.size
px = im.load()

def lum(p): return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]

# the terminal's background is dark; inverse cells are bright boxes
rows = []
y = 0
while y < H:
    bright = [x for x in range(min(W, 900)) if lum(px[x, y]) > 140]
    if len(bright) > 30:
        y0 = y
        while y < H and sum(1 for x in range(bright[0], bright[0] + 20) if lum(px[x, y]) > 140) > 10:
            y += 1
        rows.append((y0, y - 1, bright[0]))
    y += 1

for (y0, y1, x0) in rows[:2]:
    # the run of bright pixels on the box's middle line: the first inverse span
    mid = (y0 + y1) // 2
    x = x0
    while x < W and (lum(px[x, mid]) > 140 or lum(px[x, y0 + 1]) > 140):
        x += 1
    x1 = x - 1
    ink_rows = [yy for yy in range(y0, y1 + 1) if any(lum(px[xx, yy]) < 90 for xx in range(x0 + 1, x1))]
    if not ink_rows:
        print(f"box y {y0}..{y1}: no ink"); continue
    top, bot = ink_rows[0], ink_rows[-1]
    print(f"box y {y0}..{y1} ({y1 - y0 + 1} px)  x {x0}..{x1}  ink y {top}..{bot}  "
          f"space above {top - y0} px, below {y1 - bot} px")
