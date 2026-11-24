"""Generates the sample sprites for the ScreenDriver sprite demo (original, simple shapes).

    python tools/make_sample_art.py <output folder>

Writes:
  tile.png   32x32   background tile (opaque)
  coin.png   128x16  sheet: 8 frames of 16x16, a spinning coin (PNG transparency)
  slime.png  64x16   sheet: 4 frames of 16x16, a bouncing slime facing right (PNG transparency)
  ship.png   32x32   a small spaceship pointing up (PNG transparency)
  ball.bmp   16x16   24-bit BMP, magenta (#FF00FF) = transparent
Copy the folder to the SD card as /screendriver/.
"""
import math
import os
import random
import sys

from PIL import Image, ImageDraw

out = sys.argv[1] if len(sys.argv) > 1 else "sample_art"
os.makedirs(out, exist_ok=True)
random.seed(7)

# ---- tile: dark teal stone with a subtle bevel
tile = Image.new("RGB", (32, 32), (24, 52, 60))
px = tile.load()
for y in range(32):
    for x in range(32):
        n = random.randint(-6, 6)
        r, g, b = px[x, y]
        px[x, y] = (r + n, g + n, b + n)
d = ImageDraw.Draw(tile)
d.line([(0, 0), (31, 0)], fill=(44, 84, 94))
d.line([(0, 0), (0, 31)], fill=(44, 84, 94))
d.line([(0, 31), (31, 31)], fill=(12, 30, 36))
d.line([(31, 0), (31, 31)], fill=(12, 30, 36))
tile.save(os.path.join(out, "tile.png"))

# ---- coin: 8 frames, the ellipse width follows |cos| to look like it spins
coin = Image.new("RGBA", (128, 16), (0, 0, 0, 0))
d = ImageDraw.Draw(coin)
for f in range(8):
    half = max(1, round(7 * abs(math.cos(f * math.pi / 8))))
    cx = f * 16 + 8
    d.ellipse([cx - half, 1, cx + half - 1, 14], fill=(240, 180, 30, 255), outline=(150, 95, 10, 255))
    if half > 2:
        d.ellipse([cx - half + 2, 3, cx - half + 4, 6], fill=(255, 245, 190, 255))
coin.save(os.path.join(out, "coin.png"))

# ---- slime: 4 frames of squash and stretch, eye on the right so flipping is visible
slime = Image.new("RGBA", (64, 16), (0, 0, 0, 0))
d = ImageDraw.Draw(slime)
for f, (w, h) in enumerate([(12, 10), (14, 8), (12, 10), (10, 13)]):
    x0 = f * 16 + (16 - w) // 2
    y0 = 15 - h
    d.ellipse([x0, y0, x0 + w - 1, 15 + h // 2], fill=(90, 210, 90, 255), outline=(30, 110, 40, 255))
    slime.paste((0, 0, 0, 0), (f * 16, 15, f * 16 + 16, 16))  # flat bottom
    d.line([(x0 + 1, 15), (x0 + w - 2, 15)], fill=(30, 110, 40, 255))
    ex = x0 + w - 5
    d.rectangle([ex, y0 + 2, ex + 2, y0 + 4], fill=(255, 255, 255, 255))
    d.point((ex + 2, y0 + 3), fill=(0, 0, 0, 255))
slime.save(os.path.join(out, "slime.png"))

# ---- ship: pointing up
ship = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
d = ImageDraw.Draw(ship)
d.polygon([(16, 2), (27, 27), (16, 21), (5, 27)], fill=(180, 190, 210, 255), outline=(90, 100, 130, 255))
d.polygon([(16, 8), (19, 17), (13, 17)], fill=(80, 170, 255, 255))
d.polygon([(12, 24), (16, 30), (20, 24)], fill=(255, 140, 30, 255))
ship.save(os.path.join(out, "ship.png"))

# ---- ball: 24-bit BMP with a magenta background as the transparent color
ball = Image.new("RGB", (16, 16), (255, 0, 255))
d = ImageDraw.Draw(ball)
d.ellipse([1, 1, 14, 14], fill=(220, 40, 40), outline=(120, 10, 10))
d.ellipse([4, 3, 7, 6], fill=(255, 190, 190))
ball.save(os.path.join(out, "ball.bmp"))

print("written to", os.path.abspath(out))
