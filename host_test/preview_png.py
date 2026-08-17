"""host_test/preview_png.py

Renders the same outline geometry the host-side C++ test driver
exercises into a PNG file, using Pillow. This is a tiny visualisation
that lets a viewer that can't open SVG still see the rebuilt block
outline as a colour cube. Same 30°/45° axonometric projection as the
C++ ASCII + SVG output so the visual proof is consistent across all
three deliverables.
"""

import math
import os
import struct
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    print("Pillow not installed. pip install --user pillow")
    sys.exit(1)

# Resolve build/ relative to this script — works whether the user
# runs us from the project root, from host_test/, or from anywhere.
HERE = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(HERE, "build")


def project(vx: float, vy: float, vz: float,
            cx: float, cy: float, scale: float,
            bx: float, by: float, bz: float):
    aY = 0.785398  # 45 deg around Y
    aX = 0.523598  # 30 deg around X
    dx = vx - bx
    dy = vy - by
    dz = vz - bz
    x1 =  dx * math.cos(aY) + dz * math.sin(aY)
    z1 = -dx * math.sin(aY) + dz * math.cos(aY)
    y1 =  dy * math.cos(aX) - z1 * math.sin(aX)
    return cx + x1 * scale, cy - y1 * scale


def argb_to_rgb(argb: int):
    return ((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF)


def make_image(title: str, mode: str, argb: int, gap: float, thickness: float):
    # Block at (100, 64, -200), block centre at (100.5, 64.5, -199.5).
    bx, by, bz = 100.5, 64.5, -199.5
    R, G, B = argb_to_rgb(argb)
    fill = (R, G, B, 215)  # alpha 0xFF -> 255 but PIL alpha is 0..255.

    W = H = 520
    img = Image.new("RGBA", (W, H), (13, 17, 23, 255))
    draw = ImageDraw.Draw(img, "RGBA")

    # Title.
    draw.text((20, 22), "BedrockTools - Block Outline (rebuilt)",
              fill=(230, 237, 243, 255))
    draw.text((20, 50), title, fill=(125, 133, 144, 255))

    cx, cy, scale = 260.0, 290.0, 130.0

    # The box sits just outside [bx,by,bz] -> [bx+1, by+1, bz+1] by `gap`.
    x0, y0, z0 = bx - gap, by - gap, bz - gap
    x1, y1, z1 = bx + 1.0 + gap, by + 1.0 + gap, bz + 1.0 + gap

    if mode == "2d":
        # The cpp/core converts the slider value (thickness) to a
        # band half-width in world units via * 0.01, capping at 0.45.
        band_t = min(max(thickness, 0.0) * 0.01, 0.45)
        draw_box_bands(draw, x0, y0, z0, x1, y1, z1, cx, cy, scale,
                       bx, by, bz, band_t, fill)
        draw_box_lines(draw, x0, y0, z0, x1, y1, z1, cx, cy, scale,
                       bx, by, bz)
    else:
        beam_h = max(min(thickness * 0.01, 0.45), 0.016)
        draw_box_beams(draw, x0, y0, z0, x1, y1, z1, cx, cy, scale,
                       bx, by, bz, beam_h, fill)
        ox = x0 - beam_h
        oy = y0 - beam_h
        oz = z0 - beam_h
        draw_box_lines(draw, ox, oy, oz, x1 + beam_h, y1 + beam_h,
                       z1 + beam_h, cx, cy, scale, bx, by, bz)

    footer = "outline ARGB=#%08X" % (argb & 0xFFFFFFFF)
    draw.text((20, H - 28), footer, fill=(125, 133, 144, 255))
    return img


def draw_box_lines(draw, x0, y0, z0, x1, y1, z1,
                   cx, cy, scale, bx, by, bz):
    line_color = (255, 255, 255, 255)
    edges = [
        ((x0, y0, z0), (x1, y0, z0)), ((x1, y0, z0), (x1, y0, z1)),
        ((x1, y0, z1), (x0, y0, z1)), ((x0, y0, z1), (x0, y0, z0)),
        ((x0, y1, z0), (x1, y1, z0)), ((x1, y1, z0), (x1, y1, z1)),
        ((x1, y1, z1), (x0, y1, z1)), ((x0, y1, z1), (x0, y1, z0)),
        ((x0, y0, z0), (x0, y1, z0)), ((x1, y0, z0), (x1, y1, z0)),
        ((x1, y0, z1), (x1, y1, z1)), ((x0, y0, z1), (x0, y1, z1)),
    ]
    for a, b in edges:
        ax, ay = project(*a, cx, cy, scale, bx, by, bz)
        bx_, by_ = project(*b, cx, cy, scale, bx, by, bz)
        draw.line([(ax, ay), (bx_, by_)], fill=line_color, width=2)


def rectY(draw, y, ax0, ax1, az0, az1, cx, cy, scale, bx, by, bz,
          fill):
    pts = [(ax0, y, az0), (ax1, y, az0), (ax1, y, az1), (ax0, y, az1)]
    proj = [project(*v, cx, cy, scale, bx, by, bz) for v in pts]
    draw.polygon(proj, fill=fill)


def rectXZ(draw, axis, value, ax0, ax1, ao0, ao1, cx, cy, scale,
           bx, by, bz, fill):
    if axis == "Z":
        pts = [(ax0, ao0, value), (ax1, ao0, value),
               (ax1, ao1, value), (ax0, ao1, value)]
    else:
        pts = [(value, ao0, ax0), (value, ao1, ax0),
               (value, ao1, ax1), (value, ao0, ax1)]
    proj = [project(*v, cx, cy, scale, bx, by, bz) for v in pts]
    draw.polygon(proj, fill=fill)


def draw_box_bands(draw, x0, y0, z0, x1, y1, z1,
                   cx, cy, scale, bx, by, bz, t, fill):
    for y in (y0, y1):
        rectY(draw, y, x0, x1, z0, z0 + t, cx, cy, scale, bx, by, bz, fill)
        rectY(draw, y, x0, x1, z1 - t, z1, cx, cy, scale, bx, by, bz, fill)
        rectY(draw, y, x0, x0 + t, z0 + t, z1 - t, cx, cy, scale, bx, by, bz, fill)
        rectY(draw, y, x1 - t, x1, z0 + t, z1 - t, cx, cy, scale, bx, by, bz, fill)
    for z in (z0, z1):
        rectXZ(draw, "Z", z, x0, x1, y0, y0 + t, cx, cy, scale,
               bx, by, bz, fill)
        rectXZ(draw, "Z", z, x0, x1, y1 - t, y1, cx, cy, scale,
               bx, by, bz, fill)
        rectXZ(draw, "Z", z, x0, x0 + t, y0 + t, y1 - t, cx, cy,
               scale, bx, by, bz, fill)
        rectXZ(draw, "Z", z, x1 - t, x1, y0 + t, y1 - t, cx, cy,
               scale, bx, by, bz, fill)
    for x in (x0, x1):
        rectXZ(draw, "X", x, z0, z1, y0, y0 + t, cx, cy, scale,
               bx, by, bz, fill)
        rectXZ(draw, "X", x, z0, z1, y1 - t, y1, cx, cy, scale,
               bx, by, bz, fill)
        rectXZ(draw, "X", x, z0, z0 + t, y0 + t, y1 - t, cx, cy,
               scale, bx, by, bz, fill)
        rectXZ(draw, "X", x, z1 - t, z1, y0 + t, y1 - t, cx, cy,
               scale, bx, by, bz, fill)


def cuboid(draw, ax0, ay0, az0, ax1, ay1, az1,
           cx, cy, scale, bx, by, bz, fill):
    faces = [
        [(ax0, ay0, az0), (ax0, ay1, az0), (ax0, ay1, az1), (ax0, ay0, az1)],
        [(ax1, ay0, az0), (ax1, ay1, az0), (ax1, ay1, az1), (ax1, ay0, az1)],
        [(ax0, ay0, az0), (ax1, ay0, az0), (ax1, ay0, az1), (ax0, ay0, az1)],
        [(ax0, ay1, az0), (ax1, ay1, az0), (ax1, ay1, az1), (ax0, ay1, az1)],
        [(ax0, ay0, az0), (ax1, ay0, az0), (ax1, ay1, az0), (ax0, ay1, az0)],
        [(ax0, ay0, az1), (ax1, ay0, az1), (ax1, ay1, az1), (ax0, ay1, az1)],
    ]
    for f in faces:
        proj = [project(*v, cx, cy, scale, bx, by, bz) for v in f]
        draw.polygon(proj, fill=fill)


def draw_box_beams(draw, x0, y0, z0, x1, y1, z1,
                   cx, cy, scale, bx, by, bz, h, fill):
    # Beams parallel to X.
    for y in (y0, y1):
        ya = y - h if y == y0 else y
        yb = y if y == y0 else y + h
        for z in (z0, z1):
            za = z - h if z == z0 else z
            zb = z if z == z0 else z + h
            cuboid(draw, x0 - h, ya, za, x1 + h, yb, zb,
                   cx, cy, scale, bx, by, bz, fill)
    # Beams parallel to Y.
    for x in (x0, x1):
        xa = x - h if x == x0 else x
        xb = x if x == x0 else x + h
        for z in (z0, z1):
            za = z - h if z == z0 else z
            zb = z if z == z0 else z + h
            cuboid(draw, xa, y0 - h, za, xb, y1 + h, zb,
                   cx, cy, scale, bx, by, bz, fill)
    # Beams parallel to Z.
    for x in (x0, x1):
        xa = x - h if x == x0 else x
        xb = x if x == x0 else x + h
        for y in (y0, y1):
            ya = y - h if y == y0 else y
            yb = y if y == y0 else y + h
            cuboid(draw, xa, ya, z0 - h, xb, yb, z1 + h,
                   cx, cy, scale, bx, by, bz, fill)


def main():
    cfg_argb = 0xFF40E0D8          # teal
    cfg_thick = 3.0
    cfg_gap   = 0.01

    img2d = make_image("2D band pass at block (100, 64, -200)",
                       "2d", cfg_argb, cfg_gap, cfg_thick)
    img2d.save(os.path.join(BUILD_DIR, "outline_2d.png"))
    print("wrote", os.path.join(BUILD_DIR, "outline_2d.png"))

    img3d = make_image("3D beam pass at block (100, 64, -200)",
                       "3d", cfg_argb, cfg_gap, cfg_thick)
    img3d.save(os.path.join(BUILD_DIR, "outline_3d.png"))
    print("wrote", os.path.join(BUILD_DIR, "outline_3d.png"))


if __name__ == "__main__":
    main()
