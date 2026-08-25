#!/usr/bin/env python3
"""Generate the Lightroom Clone application icon.

Composition (1024 canvas, rendered at 4x supersampling for clean edges):
  - Dark blue-grey rounded square (diagonal gradient) with a soft glass
    highlight along the top edge, matching the app's dark theme.
  - A golden radial-gradient sun and a white twin-peak mountain silhouette
    (the classic "photo" mark).
  - A horizontal adjustment slider at the bottom whose filled left half uses
    the theme accent blue (#5A86C8), echoing dark.qss' slider sub-page color.

Outputs PNGs at every common size plus a multi-resolution .ico into
resources/icons/. Re-run:  python resources/tools/make_icons.py
"""

import math
import os

import numpy as np
from PIL import Image, ImageDraw

BASE = 1024          # master canvas size
SS = 4               # supersample factor (render at BASE*SS, then downscale)
S = BASE * SS        # render resolution
OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "icons")


def hex_rgb(h):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))


def mix(c1, c2, t):
    return tuple(int(c1[i] + (c2[i] - c1[i]) * t) for i in range(3))


# ---------------------------------------------------------------- background

def make_rounded_mask(rect, radius):
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [rect[0] * SS, rect[1] * SS, rect[2] * SS, rect[3] * SS],
        radius=radius * SS, fill=255)
    return mask


def build_background():
    """Dark blue-grey diagonal-gradient rounded square + glass top highlight."""
    rect = (40, 40, BASE - 40, BASE - 40)          # (l, t, r, b)
    radius = 216
    top = hex_rgb("#3A4160")                        # light corner (top-left)
    bot = hex_rgb("#20242E")                        # dark corner (bottom-right)

    yy, xx = np.ogrid[:S, :S]
    t = (xx + yy).astype(np.float32) / (2.0 * (S - 1))
    t = np.clip(t, 0.0, 1.0)
    bg = np.empty((S, S, 3), np.float32)
    for c in range(3):
        bg[..., c] = top[c] + (bot[c] - top[c]) * t
    img = Image.fromarray(bg.astype(np.uint8), "RGB")

    # Glass highlight: a soft white strip fading down from the top edge,
    # clipped to the rounded square.
    strip = Image.new("L", (S, S), 0)
    sd = ImageDraw.Draw(strip)
    top_y = rect[1] * SS
    strip_h = 130 * SS
    for y in range(top_y, top_y + strip_h):
        a = max(0, int(30 * (1.0 - (y - top_y) / float(strip_h))))
        sd.line([(rect[0] * SS, y), (rect[2] * SS, y)], fill=a)
    strip = Image.composite(strip, Image.new("L", (S, S), 0),
                            make_rounded_mask(rect, radius))
    img.paste(Image.new("RGB", (S, S), (255, 255, 255)), (0, 0), strip)

    img.putalpha(make_rounded_mask(rect, radius))
    return img


# ---------------------------------------------------------------------- sun

def build_sun(cx, cy, r):
    """Golden radial-gradient disc on a transparent layer."""
    yy, xx = np.ogrid[:S, :S]
    d = np.sqrt((xx - cx * SS) ** 2 + (yy - cy * SS) ** 2)
    t = np.clip(d / (r * SS), 0.0, 1.0).astype(np.float32)
    inner = hex_rgb("#FFF6DC")
    outer = hex_rgb("#FFC874")
    rgb = np.empty((S, S, 3), np.float32)
    for c in range(3):
        rgb[..., c] = inner[c] + (outer[c] - inner[c]) * t
    alpha = np.clip((1.0 - t) * (SS * 1.5), 0.0, 1.0) * 255.0
    alpha = alpha.astype(np.uint8)
    return Image.fromarray(np.dstack([rgb.astype(np.uint8), alpha]), "RGBA")


# ----------------------------------------------------------------- mountain

def build_mountain(rect, peaks, base_y):
    """White twin-peak mountain silhouette sitting on a given baseline."""
    l, t, r, b = rect
    base = base_y * SS
    pts = [(l * SS, base)]
    for (px, py) in peaks:                          # peak / valley sequence
        pts.append((px * SS, py * SS))
    pts += [(r * SS, base)]
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(img).polygon(pts, fill=(242, 245, 251, 255))
    return img


# ------------------------------------------------------------------ slider

def build_slider(rect, track, thumb):
    """Adjustment slider: dark groove, accent-blue filled half, white thumb."""
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    x1, y1, x2, y2 = track                         # groove (x1,y1,x2,y2)
    h = (y2 - y1) * SS
    r = h / 2
    # Groove: subtle dark well with a faint outline.
    d.rounded_rectangle([x1 * SS, y1 * SS, x2 * SS, y2 * SS], radius=r,
                        fill=(0, 0, 0, 92), outline=(255, 255, 255, 40),
                        width=SS)
    # Filled (already-adjusted) left half in the theme accent blue.
    d.rounded_rectangle([x1 * SS, y1 * SS, thumb * SS, y2 * SS], radius=r,
                        fill=(90, 134, 200, 235))
    # Thumb: white disc with a small accent dot, soft drop shadow beneath.
    tx, ty = thumb, (y1 + y2) / 2
    tr = (x2 - x1) * 0.045 * SS                     # radius relative to track
    d.ellipse([tx * SS - tr, ty * SS - tr + 2 * SS, tx * SS + tr,
               ty * SS + tr + 2 * SS], fill=(0, 0, 0, 70))
    d.ellipse([tx * SS - tr, ty * SS - tr, tx * SS + tr, ty * SS + tr],
              fill=(255, 255, 255, 255))
    dot = tr * 0.42
    d.ellipse([tx * SS - dot, ty * SS - dot, tx * SS + dot, ty * SS + dot],
              fill=(90, 134, 200, 255))
    return img


# ------------------------------------------------------------------ render

def render():
    rect = (40, 40, BASE - 40, BASE - 40)
    canvas = build_background()
    # Sun: a large golden disc hovering just above the mountain ridge, letting
    # the classic "sun rising behind the peaks" photo mark read at any size.
    sun = build_sun(cx=512, cy=440, r=122)
    # Mountain: baseline 856 keeps a clean background band below for the
    # slider; peaks (left 660, valley 780, right 580) nestle under the sun.
    mountain = build_mountain(rect, [(300, 660), (530, 780), (720, 580)], 856)
    slider = build_slider(
        rect,
        track=(276, 918, 748, 930),
        thumb=652,
    )
    canvas.alpha_composite(mountain)
    canvas.alpha_composite(sun)
    canvas.alpha_composite(slider)
    return canvas


def downscale(img, size):
    return img.resize((size, size), Image.LANCZOS)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    master = render()

    sizes = [1024, 512, 256, 128, 64, 48, 32, 16]
    for s in sizes:
        img = downscale(master, s)
        img.save(os.path.join(OUT_DIR, "app_%d.png" % s))
        print("wrote app_%d.png" % s)

    # Multi-resolution ICO (256 is the Windows max and scales down cleanly).
    ico_sizes = [256, 128, 64, 48, 32, 16]
    ico_images = [downscale(master, s) for s in ico_sizes]
    ico_path = os.path.join(OUT_DIR, "app.ico")
    ico_images[0].save(ico_path, format="ICO", append_images=ico_images[1:],
                       sizes=[(s, s) for s in ico_sizes])
    print("wrote app.ico")


if __name__ == "__main__":
    main()
