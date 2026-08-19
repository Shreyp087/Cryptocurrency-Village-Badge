#!/usr/bin/env python3
"""Crop meme artwork to the badge screen and emit little-endian RGB565."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import Image, ImageEnhance, ImageFilter

SCREEN_SIZE = (320, 240)


def rgb565(red: int, green: int, blue: int) -> int:
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


def crop_to_aspect(image: Image.Image) -> Image.Image:
    target_ratio = SCREEN_SIZE[0] / SCREEN_SIZE[1]
    source_ratio = image.width / image.height
    if source_ratio > target_ratio:
        width = round(image.height * target_ratio)
        left = (image.width - width) // 2
        return image.crop((left, 0, left + width, image.height))

    height = round(image.width / target_ratio)
    top = (image.height - height) // 2
    return image.crop((0, top, image.width, top + height))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument(
        "--pixel-art",
        action="store_true",
        help="use nearest-neighbor scaling and a small saturation boost",
    )
    parser.add_argument("--raw", type=Path, default=Path("main/meme_rgb565.bin"))
    parser.add_argument(
        "--preview",
        type=Path,
        default=Path("assets/meme_screen_preview.png"),
    )
    args = parser.parse_args()

    with Image.open(args.source) as opened:
        image = crop_to_aspect(opened.convert("RGB"))

    resampling = Image.Resampling.NEAREST if args.pixel_art else Image.Resampling.LANCZOS
    image = image.resize(SCREEN_SIZE, resampling)
    if args.pixel_art:
        image = ImageEnhance.Contrast(image).enhance(1.08)
        image = ImageEnhance.Color(image).enhance(1.08)
    else:
        image = image.filter(ImageFilter.UnsharpMask(radius=0.8, percent=150, threshold=2))
        image = ImageEnhance.Contrast(image).enhance(1.04)

    quantized = Image.new("RGB", SCREEN_SIZE)
    raw = bytearray()
    output_pixels = []
    for red, green, blue in image.getdata():
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
