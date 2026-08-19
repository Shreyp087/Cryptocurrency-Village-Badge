#!/usr/bin/env python3
"""Convert an image into the badge's 320x240 little-endian RGB565 asset."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import Image

SCREEN_SIZE = (320, 240)
AVATAR_BOX = (220, 220)


def rgb565(red: int, green: int, blue: int) -> int:
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("--raw", type=Path, default=Path("main/avatar_rgb565.bin"))
    parser.add_argument(
        "--preview",
        type=Path,
        default=Path("assets/avatar_screen_preview.png"),
    )
    args = parser.parse_args()

    with Image.open(args.source) as opened:
        opened.seek(0)
        avatar = opened.convert("RGB")

    avatar.thumbnail(AVATAR_BOX, Image.Resampling.NEAREST)
    screen = Image.new("RGB", SCREEN_SIZE, (0, 0, 0))
    offset = (
        (SCREEN_SIZE[0] - avatar.width) // 2,
        (SCREEN_SIZE[1] - avatar.height) // 2,
    )
    screen.paste(avatar, offset)

    quantized = Image.new("RGB", SCREEN_SIZE)
    raw = bytearray()
    output_pixels = []
    for y in range(SCREEN_SIZE[1]):
        for x in range(SCREEN_SIZE[0]):
            red, green, blue = screen.getpixel((x, y))
            value = rgb565(red, green, blue)
            raw.extend(struct.pack("<H", value))
            output_pixels.append(
                (
                    ((value >> 11) & 0x1F) * 255 // 31,
                    ((value >> 5) & 0x3F) * 255 // 63,
                    (value & 0x1F) * 255 // 31,
                )
            )
    quantized.putdata(output_pixels)

    args.raw.parent.mkdir(parents=True, exist_ok=True)
    args.preview.parent.mkdir(parents=True, exist_ok=True)
    args.raw.write_bytes(raw)
    quantized.save(args.preview)

    print(f"wrote {args.raw} ({len(raw)} bytes)")
    print(f"wrote {args.preview} ({SCREEN_SIZE[0]}x{SCREEN_SIZE[1]})")


if __name__ == "__main__":
    main()
