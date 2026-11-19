#!/usr/bin/env python3
"""Describe a screenshot numerically: since there is no image viewer here,
print a coarse luminance map and the dominant colours per region, so layout
and material can be judged from numbers."""
import sys
from PIL import Image

def main(path, cols=96, rows=34):
    im = Image.open(path).convert("RGB")
    W, H = im.size
    small = im.resize((cols, rows), Image.BOX)
    ramp = " .:-=+*#%@"
    print(f"{path}: {W}x{H}")
    for y in range(rows):
        line = ""
        for x in range(cols):
            r, g, b = small.getpixel((x, y))
            L = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0
            line += ramp[min(len(ramp) - 1, int(L * 3.2 * (len(ramp) - 1)))]
        print(line)
    # sample colours along a few rows
    for fy in (0.02, 0.05, 0.2, 0.5, 0.97):
        y = int(H * fy)
        samples = [im.getpixel((int(W * fx), y)) for fx in (0.01, 0.1, 0.3, 0.5, 0.7, 0.9, 0.99)]
        print(f"y={fy:.2f}", " ".join("#%02x%02x%02x" % s for s in samples))

if __name__ == "__main__":
    for p in sys.argv[1:]:
        main(p)
