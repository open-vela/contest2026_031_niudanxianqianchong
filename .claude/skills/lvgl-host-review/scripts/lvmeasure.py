#!/usr/bin/env python3
"""Pixel measurement helpers for LVGL host-rendered screenshots.

All helpers assume an 8-bit RGB PIL image decoded from the framebuffer
export (PPM/PNG). They are deliberately tolerant: LVGL on a 16-bit panel
quantises colour, so exact RGB equality almost never holds.

Typical use:

    from lvmeasure import load, color_bbox, text_bands, region_diff

    im, px = load("home.png")
    print(color_bbox(px, im.size, (0xFF, 0xFA, 0xF6)))   # card rect
    print(text_bands(px, 24, 900, 56, 540))              # text rows
    print(region_diff(a_px, b_px, (24, 136, 1000, 520))) # design vs host
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Iterable, Sequence

RGB = tuple[int, int, int]
Box = tuple[int, int, int, int]   # x, y, w, h

#: Colour distance (sum of absolute channel deltas / 3) treated as "same".
#: RGB565 has 5/6/5 bits per channel, so a value like 0xF8 becomes 0xFF and
#: 0xEC becomes 0xEE: an 8-31 step of drift is expected on every channel.
DEFAULT_COLOR_TOL = 8


def load(path):
    """Open an image as RGB and return (image, pixel-access)."""
    from PIL import Image

    im = Image.open(path).convert("RGB")
    return im, im.load()


def hexstr(c: RGB) -> str:
    return "#%02X%02X%02X" % c


def color_distance(a: RGB, b: RGB) -> float:
    """Mean absolute per-channel difference; RGB565 noise lands under ~10."""
    return (abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])) / 3.0


def near(a: RGB, b: RGB, tol: int = DEFAULT_COLOR_TOL) -> bool:
    return color_distance(a, b) <= tol


def color_bbox(px, size, color: RGB, tol: int = DEFAULT_COLOR_TOL,
               box: Box | None = None) -> Box | None:
    """Bounding box of every pixel within *tol* of *color*.

    Returns None when nothing matches.

    NOTE: the result is the union of ALL matching pixels. A card fill that also
    appears in several cards, or a colour reused by the page background, yields
    a box spanning all of them (often the whole screen). Pass *box* to restrict
    the search to one region when that happens -- do not conclude "the card is
    at (0,0)" from a full-screen result.

    Use this for cards, chips and dividers: fills you already know. Gradients
    produce near-misses, which is why the tolerance defaults to the RGB565
    noise floor rather than exact equality.
    """
    w, h = size                        # type: ignore[misc]
    x0, y0, x1, y1 = box or (0, 0, w, h)
    xs: list[int] = []
    ys: list[int] = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            if near(px[x, y], color, tol):
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return (min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)


def dark_bbox(px, size, box: Box, threshold: int = 170) -> Box | None:
    """Bounding box of pixels darker than *threshold* (text, glyphs)."""
    w, h = size                        # type: ignore[misc]
    x0, y0, x1, y1 = box
    xs: list[int] = []
    ys: list[int] = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            c = px[x, y]
            if (c[0] + c[1] + c[2]) / 3.0 < threshold:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return (min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)


def text_bands(px, x0: int, x1: int, y0: int, y1: int, threshold: int = 165,
               gap: int = 3, min_height: int = 4) -> list[tuple[int, int]]:
    """Group consecutive rows containing dark pixels into text line bands.

    Returns [(top, bottom), ...]. This is the right primitive for comparing
    text layout: an SVG `y` is a BASELINE, while LVGL `lv_obj_set_pos` is the
    widget TOP-LEFT. Comparing those numbers directly produces phantom
    "30 px too high" conclusions (this exact mistake happened during the V9
    work). Measure rendered bands instead.
    """
    bands: list[list[int]] = []
    for y in range(y0, y1):
        hit = any(
            (sum(px[x, y]) / 3.0) < threshold for x in range(x0, x1)
        )
        if hit:
            if bands and y - bands[-1][1] <= gap:
                bands[-1][1] = y
            else:
                bands.append([y, y])
    return [(a, b) for a, b in bands if b - a + 1 >= min_height]


@dataclass
class DiffStat:
    pixels: int
    strong: int
    strong_ratio: float
    mean: float

    def __str__(self) -> str:
        return (f"{self.strong} px >40 ({self.strong_ratio * 100:.1f}%), "
                f"mean {self.mean:.1f}")


def region_diff(a_px, b_px, box: Box, strong: float = 40.0) -> DiffStat:
    """Compare two same-sized screenshots inside *box*.

    `strong` counts pixels whose mean channel delta exceeds the threshold;
    that number is what you quote as "the layout differs here". The mean is
    reported separately because antialiasing and font hinting lift it even
    when every box is positioned correctly.
    """
    x0, y0, x1, y1 = box
    total = strong_px = 0
    acc = 0.0
    for y in range(y0, y1):
        for x in range(x0, x1):
            d = color_distance(a_px[x, y], b_px[x, y])
            if d > strong:
                strong_px += 1
            acc += d
            total += 1
    total = max(total, 1)
    return DiffStat(total, strong_px, strong_px / total, acc / total)


def row_profile(px, size, box: Box, threshold: int = 170) -> list[int]:
    """Dark-pixel count per row inside *box*; useful for spotting clipping."""
    w, h = size                        # type: ignore[misc]
    x0, y0, x1, y1 = box
    return [
        sum(1 for x in range(x0, x1)
            if (sum(px[x, y]) / 3.0) < threshold)
        for y in range(y0, y1)
    ]


def col_profile(px, size, box: Box, threshold: int = 170) -> list[int]:
    """Dark-pixel count per column inside *box*."""
    w, h = size                        # type: ignore[misc]
    x0, y0, x1, y1 = box
    return [
        sum(1 for y in range(y0, y1)
            if (sum(px[x, y]) / 3.0) < threshold)
        for x in range(x0, x1)
    ]


def ascii_preview(px, size, box: Box, threshold: int = 170,
                  step: int = 4) -> str:
    """Coarse ASCII rendering of a region -- fast sanity check in a terminal.

    "#" = dark (ink), "+" = mid, "." = background. Handy for spotting a glyph
    that only renders in its centre (the classic `inner_align=AUTO_TRANSFORM`
    clipping bug).
    """
    w, h = size                        # type: ignore[misc]
    x0, y0, x1, y1 = box
    out = []
    for y in range(y0, y1, step):
        row = []
        for x in range(x0, x1, step):
            v = sum(px[x, y]) / 3.0
            row.append("#" if v < threshold - 60 else
                       ("+" if v < threshold else "."))
        out.append("".join(row))
    return "\n".join(out)


def coverage(px, size, box: Box, bg: RGB,
             tol: int = DEFAULT_COLOR_TOL) -> float:
    """Fraction of pixels in *box* that differ from *bg* (ink coverage)."""
    x0, y0, x1, y1 = box
    total = max((x1 - x0) * (y1 - y0), 1)
    ink = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            if not near(px[x, y], bg, tol):
                ink += 1
    return ink / total
