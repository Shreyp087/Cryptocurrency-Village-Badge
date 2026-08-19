#!/usr/bin/env python3
"""Render a desktop preview of the SHREY and Bollywood badge scenes."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageEnhance

WIDTH = 320
HEIGHT = 240
TRANSITION_FRAMES = 6

FONT = {
    " ": (0x00, 0x00, 0x00, 0x00, 0x00),
    "!": (0x00, 0x00, 0x5F, 0x00, 0x00),
    "'": (0x00, 0x05, 0x03, 0x00, 0x00),
    ".": (0x00, 0x60, 0x60, 0x00, 0x00),
    ":": (0x00, 0x36, 0x36, 0x00, 0x00),
    "A": (0x7E, 0x11, 0x11, 0x11, 0x7E),
    "B": (0x7F, 0x49, 0x49, 0x49, 0x36),
    "C": (0x3E, 0x41, 0x41, 0x41, 0x22),
    "D": (0x7F, 0x41, 0x41, 0x22, 0x1C),
    "E": (0x7F, 0x49, 0x49, 0x49, 0x41),
    "F": (0x7F, 0x09, 0x09, 0x09, 0x01),
    "G": (0x3E, 0x41, 0x49, 0x49, 0x7A),
    "H": (0x7F, 0x08, 0x08, 0x08, 0x7F),
    "I": (0x00, 0x41, 0x7F, 0x41, 0x00),
    "J": (0x20, 0x40, 0x41, 0x3F, 0x01),
    "K": (0x7F, 0x08, 0x14, 0x22, 0x41),
    "L": (0x7F, 0x40, 0x40, 0x40, 0x40),
    "M": (0x7F, 0x02, 0x0C, 0x02, 0x7F),
    "N": (0x7F, 0x04, 0x08, 0x10, 0x7F),
    "O": (0x3E, 0x41, 0x41, 0x41, 0x3E),
    "P": (0x7F, 0x09, 0x09, 0x09, 0x06),
    "Q": (0x3E, 0x41, 0x51, 0x21, 0x5E),
    "R": (0x7F, 0x09, 0x19, 0x29, 0x46),
    "S": (0x46, 0x49, 0x49, 0x49, 0x31),
    "T": (0x01, 0x01, 0x7F, 0x01, 0x01),
    "U": (0x3F, 0x40, 0x40, 0x40, 0x3F),
    "V": (0x1F, 0x20, 0x40, 0x20, 0x1F),
    "W": (0x3F, 0x40, 0x38, 0x40, 0x3F),
    "X": (0x63, 0x14, 0x08, 0x14, 0x63),
    "Y": (0x07, 0x08, 0x70, 0x08, 0x07),
    "Z": (0x61, 0x51, 0x49, 0x45, 0x43),
}


def text_width(text: str, scale: int) -> int:
    return (len(text) * 6 - 1) * scale if text else 0


def draw_text(
    image: Image.Image,
    text: str,
    x: int,
    y: int,
    scale: int,
    color: tuple[int, int, int],
    visible: int | None = None,
) -> None:
    draw = ImageDraw.Draw(image)
    visible = len(text) if visible is None else min(visible, len(text))
    for character_index, character in enumerate(text[:visible]):
        columns = FONT.get(character, FONT[" "])
        for column, bits in enumerate(columns):
            for row in range(7):
                if bits & (1 << row):
                    left = x + (character_index * 6 + column) * scale
                    top = y + row * scale
                    draw.rectangle(
                        (left, top, left + scale - 1, top + scale - 1),
                        fill=color,
                    )


def fade_amount(frame: int, duration: int) -> float:
    if frame < TRANSITION_FRAMES:
        return frame / TRANSITION_FRAMES
    frames_left = duration - 1 - frame
    if frames_left < TRANSITION_FRAMES:
        return frames_left / TRANSITION_FRAMES
    return 1.0


def name_frame(frame: int) -> Image.Image:
    image = Image.new("RGB", (WIDTH, HEIGHT))
    pixels = image.load()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            color = [2 + y * 5 // HEIGHT, 4 + x * 6 // WIDTH, 16 + y * 14 // HEIGHT]
            if (x + frame) % 40 < 2 or (y + frame // 2) % 32 < 2:
                color = [color[0] + 3, color[1] + 8, color[2] + 18]
            if (x * 17 + y * 31 + frame * 13) % 997 < 3:
                color = [20, 180, 220]
            pixels[x, y] = tuple(color)

    eyebrow = "HELLO! I'M"
    name = "SHREY"
    message = "BUILD MODE: ON"
    entrance = (18 - frame) * 5 if frame < 18 else 0
    eyebrow_x = (WIDTH - text_width(eyebrow, 3)) // 2
    name_x = (WIDTH - text_width(name, 7)) // 2 - entrance
    message_x = (WIDTH - text_width(message, 3)) // 2

    draw_text(image, name, name_x + 3, 85, 7, (26, 8, 70))
    draw_text(image, eyebrow, eyebrow_x, 31, 3, (255, 174, 28))
    draw_text(image, name, name_x, 82, 7, (30, 238, 255))
    ImageDraw.Draw(image).rectangle(
        (name_x, 151, name_x + text_width(name, 7) - 1, 154),
        fill=(255, 174, 28),
    )
    draw_text(image, message, message_x + 2, 187, 3, (20, 5, 60))
    draw_text(image, message, message_x, 185, 3, (114, 255, 196))
    return ImageEnhance.Brightness(image).enhance(fade_amount(frame, 75))


def meme_frame(base: Image.Image, frame: int) -> Image.Image:
    inset_cycle = (0, 1, 2, 3, 4, 5, 4, 3, 2, 1)
    inset = inset_cycle[(frame // 3) % len(inset_cycle)]
    inset_y = inset * 3 // 4
    shake_phase = frame % 46
    shake_x = -2 if shake_phase == 0 else (2 if shake_phase == 1 else 0)
    image = Image.new("RGB", (WIDTH, HEIGHT), (1, 2, 18))
    cropped = base.crop(
        (inset, inset_y, WIDTH - inset, HEIGHT - inset_y)
    )
    zoomed = cropped.resize((WIDTH, HEIGHT), Image.Resampling.BILINEAR)
    image.paste(zoomed, (shake_x, 0))
    return ImageEnhance.Brightness(image).enhance(fade_amount(frame, 95))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "meme_preview",
        type=Path,
        nargs="?",
        default=Path("assets/meme_screen_preview.png"),
    )
    parser.add_argument(
        "--gif",
        type=Path,
        default=Path("assets/showcase_animation_preview.gif"),
    )
    parser.add_argument(
        "--contact-sheet",
        type=Path,
        default=Path("assets/new_scenes_preview.png"),
    )
    args = parser.parse_args()

    with Image.open(args.meme_preview) as opened:
        meme = opened.convert("RGB")
    if meme.size != (WIDTH, HEIGHT):
        raise SystemExit(f"expected {WIDTH}x{HEIGHT}; got {meme.size}")

    frames = [name_frame(frame) for frame in range(0, 75, 3)]
    frames.extend(meme_frame(meme, frame) for frame in range(0, 95, 3))
    args.gif.parent.mkdir(parents=True, exist_ok=True)
    frames[0].save(
        args.gif,
        save_all=True,
        append_images=frames[1:],
        duration=220,
        loop=0,
        disposal=2,
        optimize=True,
    )

    sheet = Image.new("RGB", (WIDTH * 2, HEIGHT), "black")
    sheet.paste(name_frame(70), (0, 0))
    sheet.paste(meme_frame(meme, 70), (WIDTH, 0))
    sheet = sheet.resize((WIDTH * 4, HEIGHT * 2), Image.Resampling.NEAREST)
    sheet.save(args.contact_sheet)
    print(f"wrote {args.gif} ({len(frames)} frames)")
    print(f"wrote {args.contact_sheet}")


if __name__ == "__main__":
    main()
