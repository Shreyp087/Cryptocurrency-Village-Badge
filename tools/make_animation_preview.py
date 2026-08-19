#!/usr/bin/env python3
"""Render a desktop GIF preview of the firmware-side avatar animation."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageEnhance

WIDTH = 320
HEIGHT = 240
BOB_CYCLE = (0, -1, -2, -2, -1, 0, 1, 2, 2, 1)
PULSE_CYCLE = (224, 232, 240, 248, 255, 248, 240, 232)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("screen_preview", type=Path)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("assets/avatar_animation_preview.gif"),
    )
    args = parser.parse_args()

    with Image.open(args.screen_preview) as opened:
        base = opened.convert("RGB")
    if base.size != (WIDTH, HEIGHT):
        raise SystemExit(f"expected {WIDTH}x{HEIGHT}; got {base.size[0]}x{base.size[1]}")

    frames: list[Image.Image] = []
    for frame_number in range(54):
        bob = BOB_CYCLE[(frame_number // 2) % len(BOB_CYCLE)]
        brightness = PULSE_CYCLE[(frame_number // 3) % len(PULSE_CYCLE)] / 255
        bright = ImageEnhance.Brightness(base).enhance(brightness)
        frame = Image.new("RGB", (WIDTH, HEIGHT), "black")
        frame.paste(bright, (0, bob))

        blink_phase = frame_number % 54
        if blink_phase in (49, 50, 51):
            draw = ImageDraw.Draw(frame)
            draw.rectangle((130, 99 + bob, 155, 116 + bob), fill="black")
            draw.rectangle((171, 99 + bob, 196, 116 + bob), fill="black")

        scanline = (frame_number * 5) % HEIGHT
        pixels = frame.load()
        for y in range(max(0, scanline - 1), min(HEIGHT, scanline + 2)):
            for x in range(WIDTH):
                red, green, blue = pixels[x, y]
                if red or green or blue:
                    pixels[x, y] = (
                        red * 3 // 4,
                        (green * 3 + 255) // 4,
                        (blue * 3 + 255) // 4,
                    )
        frames.append(frame)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    frames[0].save(
        args.output,
        save_all=True,
        append_images=frames[1:],
        duration=55,
        loop=0,
        disposal=2,
        optimize=True,
    )
    print(f"wrote {args.output} ({len(frames)} frames)")


if __name__ == "__main__":
    main()
