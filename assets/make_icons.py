"""Draws Rekodr's icons from the logo, rekodr-logo.svg: a centre dot between two rounded corner brackets, white.

    python assets/make_icons.py      (needs Pillow)

Every icon puts the logo on a dark plate, so the white mark shows on light grounds too (Explorer, a light Start
menu or taskbar). Writes, next to this file:
  rekodr.ico            the app icon (exe, installer, window, shortcuts, taskbar), 16 to 256 px
  tray-recording.ico    tray icon while recording: the centre dot red, 16 to 48 px
  tray-paused.ico       tray icon while paused: the brackets grey, a pause sign in place of the dot
  rekodr.png            256 px, for docs
  rekodr-emoji.png      128 px, the Discord emoji on the cards' "via" line
At 16 to 24 px the logo fills more of the plate and is drawn on its own rather than shrunk, so it stays crisp.
"""
import re
from pathlib import Path
from PIL import Image, ImageDraw

HERE = Path(__file__).parent
SS = 8  # supersampling
RED = (239, 68, 68)
GREY = (150, 155, 166)
PLATE = (22, 24, 31)
WHITE = (255, 255, 255)


def logo():
    """The logo's shapes from the SVG: (view box size, [circle (cx, cy, r)], [closed outlines as point lists])."""
    svg = (HERE / "rekodr-logo.svg").read_text()
    box = float(re.search(r'viewBox="0 0 ([\d.]+)', svg).group(1))
    circles = [tuple(float(v) for v in m) for m in re.findall(r'<circle cx="([\d.]+)" cy="([\d.]+)" r="([\d.]+)"', svg)]
    outlines = [outline(d) for d in re.findall(r'<path d="([^"]+)"', svg)]
    return box, circles, outlines


def outline(d):
    """An SVG path (M L H V C S Z, absolute or relative) as one closed list of points; curves become short lines."""
    tokens = re.findall(r"[MmLlHhVvCcSsZz]|-?(?:\d+\.?\d*|\.\d+)(?:[eE]-?\d+)?", d)
    pts, x, y, cmd, i, last_ctrl = [], 0.0, 0.0, "", 0, None

    def num():
        nonlocal i
        i += 1
        return float(tokens[i - 1])

    def curve(p0, p1, p2, p3, steps=24):
        for k in range(1, steps + 1):
            t = k / steps
            u = 1 - t
            pts.append((u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0],
                        u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]))

    while i < len(tokens):
        if tokens[i].isalpha():
            cmd = tokens[i]
            i += 1
            if cmd in "Zz":
                continue
        rel = cmd.islower()
        c = cmd.upper()
        ox, oy = (x, y) if rel else (0.0, 0.0)
        if c == "M":
            x, y = ox + num(), oy + num()
            pts.append((x, y))
            cmd = "l" if rel else "L"  # further pairs after a move are lines
        elif c == "L":
            x, y = ox + num(), oy + num()
            pts.append((x, y))
        elif c == "H":
            x = (x if rel else 0.0) + num()
            pts.append((x, y))
        elif c == "V":
            y = (y if rel else 0.0) + num()
            pts.append((x, y))
        elif c in "CS":
            if c == "C":
                p1 = (ox + num(), oy + num())
            else:  # S: the first control point mirrors the last one
                p1 = (2 * x - last_ctrl[0], 2 * y - last_ctrl[1]) if last_ctrl else (x, y)
            p2 = (ox + num(), oy + num())
            p3 = (ox + num(), oy + num())
            curve((x, y), p1, p2, p3)
            last_ctrl, (x, y) = p2, p3
            continue
        last_ctrl = None
    return pts


LOGO = logo()


def draw(size, dot=WHITE, marks=WHITE, paused=False):
    """The logo on its plate, size px square."""
    S = size * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, S - 1, S - 1], S * 0.22, fill=PLATE + (255,))
    box, circles, outlines = LOGO
    share = 0.80 if size <= 24 else 0.66  # of the plate the logo spans
    k = S * share / box
    off = (S - box * k) / 2
    for o in outlines:
        d.polygon([(off + px * k, off + py * k) for px, py in o], fill=marks)
    for cx, cy, r in circles:
        cx, cy, r = off + cx * k, off + cy * k, r * k
        if paused:  # two bars inside the dot's place, clear of the brackets
            bw, gap, h = r * 0.5, r * 0.46, r * 0.75
            for sx in (-1, 1):
                x0 = cx + sx * (gap / 2 + bw / 2)
                d.rounded_rectangle([x0 - bw / 2, cy - h, x0 + bw / 2, cy + h], bw * 0.3, fill=marks)
        else:
            d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=dot)
    return img.resize((size, size), Image.LANCZOS)


def ico(path, sizes, **kw):
    imgs = [draw(s, **kw) for s in sizes]
    imgs[-1].save(path, format="ICO", sizes=[(s, s) for s in sizes], append_images=imgs[:-1])


if __name__ == "__main__":
    ico(HERE / "rekodr.ico", [16, 20, 24, 32, 40, 48, 64, 128, 256])
    ico(HERE / "tray-recording.ico", [16, 20, 24, 32, 40, 48], dot=RED)
    ico(HERE / "tray-paused.ico", [16, 20, 24, 32, 40, 48], marks=GREY, paused=True)
    draw(256).save(HERE / "rekodr.png")
    draw(128).save(HERE / "rekodr-emoji.png")
    print("written to", HERE)
