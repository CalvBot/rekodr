"""Draws Rekodr's icons: a white replay arrow around a red record dot, on a dark tile.

    python assets/make_icons.py      (needs Pillow)

Writes, next to this file:
  rekodr.ico          the app icon (exe, shortcuts, taskbar), 16–256 px
  tray-recording.ico    tray icon while recording, 16–48 px
  tray-paused.ico       tray icon while paused: grey arrow, pause sign instead of the dot
  favicon.png           64 px, the clips window's icon
  rekodr.png          256 px, for docs
Small sizes are drawn on their own (thicker strokes, no glow) rather than shrunk, so they stay crisp.
"""
import math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

HERE = Path(__file__).parent
SS = 8  # supersampling
RED = (239, 68, 68)
GREY = (150, 155, 166)
TILE = (22, 24, 31)
WHITE = (240, 242, 247)


def draw(size, paused=False):
    S = size * SS
    small = size <= 32
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    k = S / 1024  # design units are 1024 px

    inset = (0 if size <= 16 else 16 if small else 40) * k
    d.rounded_rectangle([inset, inset, S - inset, S - inset], (200 if small else 230) * k, fill=TILE + (255,))

    cx = cy = S / 2
    rad = (350 if small else 330) * k
    width = (125 if size <= 16 else 110 if small else 92) * k
    arrow = GREY if paused else WHITE
    start, end = 250, 560  # PIL angles: clockwise from 3 o'clock; gap at the upper left
    d.arc([cx - rad, cy - rad, cx + rad, cy + rad], start, end, fill=arrow, width=round(width))
    # Round cap on the tail end.
    a = math.radians(end)
    r0 = rad - width / 2
    x, y = cx + r0 * math.cos(a), cy + r0 * math.sin(a)
    d.ellipse([x - width / 2, y - width / 2, x + width / 2, y + width / 2], fill=arrow)
    # Arrowhead at the start, pointing anticlockwise (the replay direction).
    a = math.radians(start)
    px, py = cx + r0 * math.cos(a), cy + r0 * math.sin(a)
    tx, ty = math.sin(a), -math.cos(a)
    nx, ny = math.cos(a), math.sin(a)
    L, W = width * 1.9, width * 1.55
    tip = (px + tx * L * 0.9, py + ty * L * 0.9)
    b1 = (px + nx * W - tx * L * 0.15, py + ny * W - ty * L * 0.15)
    b2 = (px - nx * W - tx * L * 0.15, py - ny * W - ty * L * 0.15)
    d.polygon([tip, b1, b2], fill=arrow)

    r = (165 if small else 150) * k
    if paused:
        bw, bh, gap = (r * 0.55, r * 1.75, r * 0.3) if small else (r * 0.42, r * 1.5, r * 0.32)
        for sx in (-1, 1):
            x0 = cx + sx * (gap / 2 + bw / 2)
            d.rounded_rectangle([x0 - bw / 2, cy - bh / 2, x0 + bw / 2, cy + bh / 2], bw * 0.3, fill=GREY)
    else:
        if not small:  # soft glow
            g = Image.new("RGBA", img.size, (0, 0, 0, 0))
            ImageDraw.Draw(g).ellipse([cx - r * 1.6, cy - r * 1.6, cx + r * 1.6, cy + r * 1.6], fill=RED + (110,))
            img.alpha_composite(g.filter(ImageFilter.GaussianBlur(r * 0.6)))
            d = ImageDraw.Draw(img)
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=RED)
        if not small:  # highlight
            hl = Image.new("RGBA", img.size, (0, 0, 0, 0))
            ImageDraw.Draw(hl).ellipse([cx - r * 0.55, cy - r * 0.75, cx + r * 0.2, cy - r * 0.1], fill=(255, 255, 255, 60))
            img.alpha_composite(hl.filter(ImageFilter.GaussianBlur(r * 0.15)))
    return img.resize((size, size), Image.LANCZOS)


def ico(path, sizes, paused=False):
    imgs = [draw(s, paused) for s in sizes]
    imgs[-1].save(path, format="ICO", sizes=[(s, s) for s in sizes], append_images=imgs[:-1])


if __name__ == "__main__":
    ico(HERE / "rekodr.ico", [16, 20, 24, 32, 40, 48, 64, 128, 256])
    ico(HERE / "tray-recording.ico", [16, 20, 24, 32, 40, 48])
    ico(HERE / "tray-paused.ico", [16, 20, 24, 32, 40, 48], paused=True)
    draw(64).save(HERE / "favicon.png")
    draw(256).save(HERE / "rekodr.png")
    print("written to", HERE)
