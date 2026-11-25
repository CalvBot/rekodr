r"""Draws Rekodr's icons from the logo, rekodr-logo.svg: a centre dot between two rounded corner brackets, white.

    python assets/make_icons.py      (needs Pillow)

Every icon puts the logo on a dark plate, so the white mark shows on light grounds too (Explorer, a light Start
menu or taskbar). From 32 px the plate is a charcoal squircle with a gradient, rim, gloss and drop shadow, and the
mark is soft white. Writes, next to this file:
  rekodr.ico            the app icon (exe, installer, window, shortcuts, taskbar), 16 to 256 px
  tray-recording.ico    tray icon while recording: the centre dot red, 16 to 48 px, flat plate
  tray-paused.ico       tray icon while paused: the brackets grey, a pause sign in place of the dot
  rekodr.png            256 px, for docs
  rekodr-emoji.png      128 px, the Discord emoji on the cards' "via" line
and into streamdeck\dev.calvbot.rekodr.sdPlugin\imgs: plugin.png (256), plugin@2x.png (512), category.png (28)
and category@2x.png (56).
At 16 to 24 px the plate is flat and the logo fills more of it, drawn on its own rather than shrunk, so it stays crisp.
"""
import re
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageOps

HERE = Path(__file__).parent
DECK = HERE.parent / "streamdeck" / "dev.calvbot.rekodr.sdPlugin" / "imgs"
SS = 8  # supersampling
RED = (239, 68, 68)
GREY = (150, 155, 166)
PLATE = (30, 31, 36)
PLATE_LIGHT = (54, 56, 62)  # the plate's gradient, top left to bottom right
PLATE_DARK = (20, 21, 25)
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


def squircle(S, inset):
    """A squircle (superellipse) filling an S px square less inset px each side, as polygon points."""
    import math
    a = (S - 2 * inset) / 2
    c = S / 2
    pts = []
    for k in range(360):
        t = 2 * math.pi * k / 360
        ct, st = math.cos(t), math.sin(t)
        pts.append((c + a * math.copysign(abs(ct) ** 0.5, ct), c + a * math.copysign(abs(st) ** 0.5, st)))
    return pts


def ramp(S, a, b, diagonal):
    """An S px square going from colour a to colour b, top to bottom or top-left to bottom-right."""
    g = Image.linear_gradient("L").resize((S, S))
    if diagonal:
        g = ImageChops.add(g, g.rotate(90), scale=2)
    return ImageOps.colorize(g, a, b)


def mask(S, fn):
    m = Image.new("L", (S, S), 0)
    fn(ImageDraw.Draw(m))
    return m


def draw(size, dot=WHITE, marks=WHITE, paused=False, flat=False):
    """The logo on its plate, size px square. From 32 px the plate has depth: a charcoal gradient, a light rim, a
    gloss and, from 64 px, a drop shadow. Below that, or with flat, it is a plain dark plate so the mark stays crisp."""
    S = size * SS
    rich = size >= 32 and not flat
    shadow = rich and size >= 64
    inset = S * 0.04 if shadow else 0
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    plate = mask(S, lambda d: d.polygon(squircle(S, inset), fill=255))
    if shadow:
        sh = Image.new("RGBA", (S, S), (0, 0, 0, 150))
        sm = ImageChops.offset(plate, 0, int(S * 0.02)).filter(ImageFilter.GaussianBlur(S * 0.025))
        img.paste(sh, (0, 0), sm)
    if rich:
        img.paste(ramp(S, PLATE_LIGHT, PLATE_DARK, True), (0, 0), plate)
        # gloss: a soft light spot towards the top left
        gloss = mask(S, lambda d: d.ellipse([S * 0.05, -S * 0.25, S * 0.85, S * 0.45], fill=34))
        gloss = ImageChops.multiply(gloss.filter(ImageFilter.GaussianBlur(S * 0.12)), plate)
        img.paste(Image.new("RGBA", (S, S), WHITE + (255,)), (0, 0), gloss)
        # rim: a thin light edge, brightest along the top
        w = max(SS, S * 0.008)
        edge = ImageChops.subtract(plate, mask(S, lambda d: d.polygon(squircle(S, inset + w), fill=255)))
        fade = Image.linear_gradient("L").resize((S, S)).point(lambda v: 70 - v * 55 // 255)
        img.paste(Image.new("RGBA", (S, S), WHITE + (255,)), (0, 0), ImageChops.multiply(edge, fade))
    else:
        r = S * 0.26
        img.paste(Image.new("RGBA", (S, S), PLATE + (255,)), (0, 0),
                  mask(S, lambda d: d.rounded_rectangle([0, 0, S - 1, S - 1], r, fill=255)))

    box, circles, outlines = LOGO
    span = S - 2 * inset
    k = span * (0.84 if size <= 24 else 0.74) / box
    off = (S - box * k) / 2
    pts = lambda o: [(off + px * k, off + py * k) for px, py in o]

    def shapes(d, which):
        if which == "marks":
            for o in outlines:
                d.polygon(pts(o), fill=255)
        for cx, cy, r in circles:
            cx, cy, r = off + cx * k, off + cy * k, r * k
            if paused and which == "marks":  # two bars inside the dot's place, clear of the brackets
                bw, gap, h = r * 0.5, r * 0.46, r * 0.75
                for sx in (-1, 1):
                    x0 = cx + sx * (gap / 2 + bw / 2)
                    d.rounded_rectangle([x0 - bw / 2, cy - h, x0 + bw / 2, cy + h], bw * 0.3, fill=255)
            elif not paused and which == "dot":
                d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)

    m_marks = mask(S, lambda d: shapes(d, "marks"))
    m_dot = mask(S, lambda d: shapes(d, "dot"))
    if shadow:
        both = ImageChops.lighter(m_marks, m_dot)
        sm = ImageChops.offset(both, 0, int(S * 0.012)).filter(ImageFilter.GaussianBlur(S * 0.012))
        img.paste(Image.new("RGBA", (S, S), (0, 0, 0, 255)), (0, 0), sm.point(lambda v: v * 110 // 255))
    for m, colour in ((m_marks, marks), (m_dot, dot)):
        if rich and colour == WHITE:  # soft white, a little darker towards the bottom
            img.paste(ramp(S, (250, 250, 252), (206, 208, 214), False), (0, 0), m)
        else:
            img.paste(Image.new("RGBA", (S, S), colour + (255,)), (0, 0), m)
    return img.resize((size, size), Image.LANCZOS)


def ico(path, sizes, **kw):
    imgs = [draw(s, **kw) for s in sizes]
    imgs[-1].save(path, format="ICO", sizes=[(s, s) for s in sizes], append_images=imgs[:-1])


if __name__ == "__main__":
    ico(HERE / "rekodr.ico", [16, 20, 24, 32, 40, 48, 64, 128, 256])
    ico(HERE / "tray-recording.ico", [16, 20, 24, 32, 40, 48], dot=RED, flat=True)
    ico(HERE / "tray-paused.ico", [16, 20, 24, 32, 40, 48], marks=GREY, paused=True, flat=True)
    draw(256).save(HERE / "rekodr.png")
    draw(128).save(HERE / "rekodr-emoji.png")
    for name, px in (("plugin", 256), ("plugin@2x", 512), ("category", 28), ("category@2x", 56)):
        draw(px).save(DECK / (name + ".png"))
    print("written to", HERE, "and", DECK)
