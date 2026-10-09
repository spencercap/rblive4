#!/usr/bin/env python3
"""Make the boot screen the launcher paints while rbp starts.

    tools/make-splash.py LOGO.png|webp|... [OUT_DIR]

Fits the logo on a black 1280x800 screen and writes, to OUT_DIR (default scripts/device):
  splash.png      the screen as you see it (kept in git)
  splash.raw.gz   one fb0 page as the panel stores it (800x1280, 32 bpp BGRA, rotated), gzip.
                  Copy it to /data/splash.raw.gz on the unit; start-rb.sh paints it.
"""
import gzip
import os
import sys

from PIL import Image

W, H = 1280, 800          # what you see
FIT_W = 1000              # width of the logo on screen
CUT = 24                  # grey levels at or below this become pure black (the source's background is noisy)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "scripts", "device")
    logo = Image.open(sys.argv[1]).convert("L")
    lut = [0 if v <= CUT else min(255, round((v - CUT) * 255 / (255 - CUT))) for v in range(256)]
    logo = logo.point(lut)
    box = logo.getbbox()                       # trim the empty margin so the logo is centred
    if not box:
        sys.exit("the logo is empty")
    logo = logo.crop(box)
    h = round(logo.height * FIT_W / logo.width)
    logo = logo.resize((FIT_W, h), Image.LANCZOS)
    screen = Image.new("L", (W, H), 0)
    screen.paste(logo, ((W - FIT_W) // 2, (H - h) // 2))
    screen = screen.convert("RGB")
    screen.save(os.path.join(out, "splash.png"), optimize=True)
    # fb0 is the panel's own 800x1280 portrait; the screen is that turned a quarter (see docs/06-display.md)
    page = screen.rotate(90, expand=True).convert("RGBA")
    r, g, b, a = page.split()
    raw = Image.merge("RGBA", (b, g, r, Image.new("L", page.size, 255))).tobytes()
    with gzip.open(os.path.join(out, "splash.raw.gz"), "wb", 9) as f:
        f.write(raw)
    print("wrote splash.png and splash.raw.gz (%d bytes raw) to %s" % (len(raw), os.path.abspath(out)))


main()
