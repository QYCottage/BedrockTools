#!/usr/bin/env python3
"""Regenerate the embedded Nametag Icon pixels from the launcher icon.

The Third Person Nametag module draws the BedrockTools launcher icon (the very
same artwork that ships as ``icon.png`` inside ``BedrockTools.levipack``) next
to the player name. The launcher's mod-menu image API takes raw RGBA pixels, so
the icon is embedded in the binary as base64 RGBA instead of a PNG (there is no
PNG decoder in the mod).

The source artwork is a glowing logo painted on an opaque black square. Copying
it verbatim makes the game draw a black tile over the world, which is why the
icon looked wrong in game. This script therefore:

  * derives an alpha channel from the artwork (black background -> transparent,
    glow -> opaque) and un-premultiplies the color, which is exact because the
    artwork is composited over black;
  * crops to the logo's bounding box so the hexagon fills the icon instead of
    being surrounded by empty padding;
  * box-filters the crop down to the embedded texture size.

Usage:

    python3 scripts/generate_nametag_icon.py

which rewrites ``src/modules/visual/nametagicon_assets.hpp`` in place.
"""

from __future__ import annotations

import argparse
import base64
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = ROOT / "assets" / "bedrocktools.png"
DEFAULT_OUTPUT = ROOT / "src" / "modules" / "visual" / "nametagicon_assets.hpp"
DEFAULT_SIZE = 64

# Pixels dimmer than this (after the gain below) are treated as pure background.
ALPHA_CUTOFF = 10
# Slightly boosts the glow so the logo stays readable over bright terrain.
ALPHA_GAIN = 1.25
# Extra breathing room kept around the logo's bounding box, in source pixels.
CROP_MARGIN_RATIO = 0.02


def read_png(path: Path) -> tuple[int, int, bytearray]:
    """Minimal non-interlaced 8-bit PNG reader returning RGBA pixels."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\x0a":
        raise ValueError(f"{path} is not a PNG file")

    pos = 8
    idat = bytearray()
    palette = b""
    transparency = b""
    width = height = depth = color_type = interlace = 0

    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos : pos + 4])
        kind = data[pos + 4 : pos + 8]
        chunk = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
        elif kind == b"IDAT":
            idat += chunk
        elif kind == b"PLTE":
            palette = chunk
        elif kind == b"tRNS":
            transparency = chunk
        elif kind == b"IEND":
            break

    if depth != 8 or interlace != 0:
        raise ValueError("only 8-bit non-interlaced PNGs are supported")

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray()
    previous = bytearray(stride)
    offset = 0

    for _ in range(height):
        filter_type = raw[offset]
        offset += 1
        line = bytearray(raw[offset : offset + stride])
        offset += stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            up_left = previous[i - channels] if i >= channels else 0
            if filter_type == 1:
                line[i] = (line[i] + left) & 0xFF
            elif filter_type == 2:
                line[i] = (line[i] + up) & 0xFF
            elif filter_type == 3:
                line[i] = (line[i] + (left + up) // 2) & 0xFF
            elif filter_type == 4:
                pa = abs(up - up_left)
                pb = abs(left - up_left)
                pc = abs(left + up - 2 * up_left)
                predictor = left if (pa <= pb and pa <= pc) else (up if pb <= pc else up_left)
                line[i] = (line[i] + predictor) & 0xFF
            elif filter_type != 0:
                raise ValueError(f"unsupported PNG filter {filter_type}")
        out += line
        previous = line

    rgba = bytearray(width * height * 4)
    for i in range(width * height):
        pixel = out[i * channels : (i + 1) * channels]
        if color_type == 6:
            rgba[i * 4 : i * 4 + 4] = pixel
        elif color_type == 2:
            rgba[i * 4 : i * 4 + 3] = pixel
            rgba[i * 4 + 3] = 255
        elif color_type == 0:
            rgba[i * 4 : i * 4 + 3] = bytes(pixel * 3)
            rgba[i * 4 + 3] = 255
        elif color_type == 4:
            rgba[i * 4 : i * 4 + 3] = bytes([pixel[0]] * 3)
            rgba[i * 4 + 3] = pixel[1]
        else:  # palette
            index = pixel[0]
            rgba[i * 4 : i * 4 + 3] = palette[index * 3 : index * 3 + 3]
            rgba[i * 4 + 3] = transparency[index] if index < len(transparency) else 255
    return width, height, rgba


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    """Writes an RGBA PNG (used for the optional preview output)."""
    raw = b"".join(b"\x00" + bytes(rgba[y * width * 4 : (y + 1) * width * 4]) for y in range(height))

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    path.write_bytes(
        b"\x89PNG\r\n\x1a\x0a"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def alpha_of(r: int, g: int, b: int) -> int:
    """Opacity implied by a glow painted over black."""
    value = int(max(r, g, b) * ALPHA_GAIN)
    if value <= ALPHA_CUTOFF:
        return 0
    return min(255, value)


def logo_bounds(width: int, height: int, rgba: bytearray) -> tuple[int, int, int, int]:
    min_x, min_y, max_x, max_y = width, height, -1, -1
    for y in range(height):
        row = y * width
        for x in range(width):
            p = (row + x) * 4
            if alpha_of(rgba[p], rgba[p + 1], rgba[p + 2]) > 0:
                if x < min_x:
                    min_x = x
                if x > max_x:
                    max_x = x
                if y < min_y:
                    min_y = y
                if y > max_y:
                    max_y = y
    if max_x < 0:
        return 0, 0, width - 1, height - 1
    return min_x, min_y, max_x, max_y


def square_crop(width: int, height: int, bounds: tuple[int, int, int, int]) -> tuple[int, int, int]:
    """Squares up the bounding box (plus margin) and clamps it to the image."""
    min_x, min_y, max_x, max_y = bounds
    margin = int(round(max(width, height) * CROP_MARGIN_RATIO))
    min_x = max(0, min_x - margin)
    min_y = max(0, min_y - margin)
    max_x = min(width - 1, max_x + margin)
    max_y = min(height - 1, max_y + margin)

    side = max(max_x - min_x + 1, max_y - min_y + 1)
    side = min(side, width, height)
    center_x = (min_x + max_x) // 2
    center_y = (min_y + max_y) // 2
    x = min(max(0, center_x - side // 2), width - side)
    y = min(max(0, center_y - side // 2), height - side)
    return x, y, side


def build_icon(source: Path, size: int) -> bytearray:
    width, height, rgba = read_png(source)
    crop_x, crop_y, side = square_crop(width, height, logo_bounds(width, height, rgba))

    out = bytearray(size * size * 4)
    for oy in range(size):
        # Source span covered by this output row/column (box filter).
        y0 = crop_y + (oy * side) // size
        y1 = max(y0 + 1, crop_y + ((oy + 1) * side) // size)
        for ox in range(size):
            x0 = crop_x + (ox * side) // size
            x1 = max(x0 + 1, crop_x + ((ox + 1) * side) // size)
            acc_r = acc_g = acc_b = count = 0
            for sy in range(y0, y1):
                row = sy * width
                for sx in range(x0, x1):
                    p = (row + sx) * 4
                    # The artwork is opaque, so its RGB is already the
                    # "premultiplied over black" form we want to average.
                    acc_r += rgba[p]
                    acc_g += rgba[p + 1]
                    acc_b += rgba[p + 2]
                    count += 1
            r = acc_r // count
            g = acc_g // count
            b = acc_b // count
            a = alpha_of(r, g, b)
            p = (oy * size + ox) * 4
            if a == 0:
                out[p : p + 4] = b"\x00\x00\x00\x00"
                continue
            # Un-premultiply so the launcher can blend the icon over the world.
            out[p + 0] = min(255, (r * 255 + a // 2) // a)
            out[p + 1] = min(255, (g * 255 + a // 2) // a)
            out[p + 2] = min(255, (b * 255 + a // 2) // a)
            out[p + 3] = a
    return out


def render_header(pixels: bytes, size: int) -> str:
    encoded = base64.b64encode(pixels).decode("ascii")
    chunk_len = 96
    lines = [encoded[i : i + chunk_len] for i in range(0, len(encoded), chunk_len)]
    body = "\n".join(f'    "{line}"' for line in lines)
    return f"""#pragma once

// Embedded launcher icon (the BedrockTools logo that ships as icon.png inside
// BedrockTools.levipack), cropped to the artwork and decoded to {size}x{size} RGBA.
//
// The source artwork is painted on an opaque black square. The alpha channel
// below is derived from the glow so the launcher blends the logo over the world
// instead of stamping a black tile next to the player name. Straight alpha,
// base64-encoded so the mod menu can register it without a runtime PNG loader.
//
// Regenerate with: python3 scripts/generate_nametag_icon.py

namespace bedrocktools::nametag {{

inline constexpr int kNametagIconSize = {size};
inline constexpr const char* kNametagIconImageId = "bedrocktools_nametag_icon";
inline constexpr const char* kNametagIconBase64 =
{body};

}} // namespace bedrocktools::nametag
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--size", type=int, default=DEFAULT_SIZE)
    parser.add_argument("--preview", type=Path, default=None, help="also write the icon as a PNG")
    args = parser.parse_args()

    pixels = build_icon(args.source, args.size)
    args.output.write_text(render_header(bytes(pixels), args.size), encoding="utf-8")
    if args.preview:
        write_png(args.preview, args.size, args.size, pixels)
    print(f"wrote {args.output} ({args.size}x{args.size}, {len(pixels)} bytes of RGBA)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
