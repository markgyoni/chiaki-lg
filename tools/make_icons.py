#!/usr/bin/env python3
"""Render the Chiaki-webOS app icons (the "Couch Pad" mark) with Pillow.

    python3 tools/make_icons.py          # needs: pip install pillow

Writes packaging/icon.png (80 px, simplified for small sizes),
packaging/largeIcon.png (130 px) and docs/branding/icon-{160,512}.png.
The vector source is docs/branding/logo.svg; keep the two in sync.
The mark is drawn on a 200x200 grid, 4x supersampled for smooth edges.
"""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
BG = (0x00, 0x43, 0x9C)          # appinfo iconColor / bgColor
WHITE = (0xFF, 0xFF, 0xFF)
ACCENT = (0x00, 0x72, 0xFF)      # the app's accent blue
FADED = tuple(round(b * 0.4 + 255 * 0.6) for b in BG)   # white at 60% over BG
SS = 4                           # supersampling
DY = 7                           # centre the mark vertically


def render(size: int, simple: bool) -> Image.Image:
    s = size * SS / 200.0
    img = Image.new("RGB", (size * SS, size * SS), BG)
    d = ImageDraw.Draw(img)

    def box(x0, y0, x1, y1):
        return [x0 * s, (y0 + DY) * s, x1 * s, (y1 + DY) * s]

    def circle(cx, cy, r, fill):
        d.ellipse(box(cx - r, cy - r, cx + r, cy + r), fill=fill)

    def arc(r, width, fill):
        # an arc over the pad, centred on (100, 68), from 225 to 315 degrees;
        # Pillow strokes inward from the box, so grow it by half the width
        import math
        o = r + width / 2
        d.arc(box(100 - o, 68 - o, 100 + o, 68 + o), 225, 315, fill=fill, width=round(width * s))
        # round caps
        for a in (225, 315):
            x = 100 + r * math.cos(math.radians(a))
            y = 68 + r * math.sin(math.radians(a))
            circle(x, y, width / 2, fill)

    if simple:
        arc(25.5, 11, WHITE)
    else:
        arc(19.8, 9, WHITE)
        arc(33.9, 9, FADED)

    # body and grips
    d.rounded_rectangle(box(30, 72, 170, 134), radius=31 * s, fill=WHITE)
    circle(54, 128, 24, WHITE)
    circle(146, 128, 24, WHITE)

    if simple:
        d.rounded_rectangle(box(49, 98, 77, 108), radius=2 * s, fill=BG)
        d.rounded_rectangle(box(58, 89, 68, 117), radius=2 * s, fill=BG)
        circle(140, 103, 9, ACCENT)
    else:
        d.rounded_rectangle(box(51, 99, 75, 107), radius=2 * s, fill=BG)
        d.rounded_rectangle(box(59, 91, 67, 115), radius=2 * s, fill=BG)
        circle(134, 96, 7, ACCENT)
        circle(150, 111, 7, BG)

    return img.resize((size, size), Image.LANCZOS)


def main():
    out = [
        (ROOT / "packaging" / "icon.png", 80, True),
        (ROOT / "packaging" / "largeIcon.png", 130, False),
        (ROOT / "docs" / "branding" / "icon-160.png", 160, False),
        (ROOT / "docs" / "branding" / "icon-512.png", 512, False),
    ]
    for path, size, simple in out:
        path.parent.mkdir(parents=True, exist_ok=True)
        render(size, simple).save(path, optimize=True)
        print(f"{path.relative_to(ROOT)}  {size}x{size}")


if __name__ == "__main__":
    main()
