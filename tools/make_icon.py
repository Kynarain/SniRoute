#!/usr/bin/env python3
"""Generate the application icon.

One source of truth for two consumers:
  gui/icon.ico  -> Windows executable icon (jpackage --icon) and the MSI
  gui/icon.png  -> the tray icon that the Java app loads at runtime

Design: rounded blue tile, white shield outline, white dot in the middle.
Drawn with Pillow so the .ico can hold real 16/24/32/48/256 variants instead of
one bitmap scaled down (a scaled-down shield turns to mush at 16 px).

Run from the repo root:  python tools/make_icon.py
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
BG = (31, 111, 235, 255)      # blue tile
FG = (255, 255, 255, 255)     # white glyph
SIZES = [16, 24, 32, 48, 64, 128, 256]


def render(size: int) -> Image.Image:
    scale = 4                      # supersample, then downscale for clean edges
    s = size * scale
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    radius = int(s * 0.22)
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=radius, fill=BG)

    # shield: flat top, tapering to a point, inscribed in the tile
    m = s * 0.24
    top = s * 0.20
    bottom = s - m * 0.72
    left = m
    right = s - m
    shoulder = s * 0.60

    shield = [
        (left, top),
        (right, top),
        (right, shoulder),
        (s / 2, bottom),
        (left, shoulder),
    ]
    width = max(2, int(s * 0.055))
    d.line(shield + [shield[0]], fill=FG, width=width, joint="curve")

    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    out_dir = ROOT / "gui"
    out_dir.mkdir(parents=True, exist_ok=True)

    frames = [render(n) for n in SIZES]
    ico_path = out_dir / "icon.ico"
    frames[-1].save(ico_path, format="ICO", sizes=[(n, n) for n in SIZES],
                    append_images=frames[:-1])

    png_path = out_dir / "icon.png"
    render(256).save(png_path, format="PNG")

    print(f"wrote {ico_path} ({ico_path.stat().st_size} bytes)")
    print(f"wrote {png_path} ({png_path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
