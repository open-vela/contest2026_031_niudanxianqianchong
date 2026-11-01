#!/usr/bin/env python3
"""Generate the SmartHome LVGL icon library from prototype-web PNGs.

The PNG files are the source of truth.  This script crops transparent margins,
fits the glyph into a standard canvas and writes uncompressed LVGL v9 A8 image
descriptors.  Generated C files are intentionally committed: firmware builds
must not depend on Pillow or the QuickApp tooling.

Approved V9 pixel PNGs under demos/smart_home/res/icons/v9 override native
20/32 px sizes without crop, resampling or recolor. Other icons retain the
legacy A8 conversion. Brand and V9 pixels are emitted as little-endian
ARGB8888 bitmaps, matching ESP32-P4 and the LVGL software renderer.
"""

from __future__ import annotations

from pathlib import Path
from typing import Iterable

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "quickapp/smart_home_ui/prototype-web/assets/icons"
ICON_DIR = ROOT / "demos/smart_home/src/ui/lvgl/icons"
OUTPUT_DIR = ICON_DIR / "generated"
HEADER = ICON_DIR / "smart_home_lvgl_png_icons.h"
LOOKUP_SOURCE = ICON_DIR / "smart_home_lvgl_png_icons.c"

BASE_SIZE = 32
SMALL_SIZE = 20
LARGE_SIZE = 48
# Home weather badge uses a dedicated 64 px asset so the glyph is rendered
# natively instead of being upscaled from 32 px.
WEATHER_SIZE = 128

# Brand artwork already includes its own background and transparent corners.
COLOR_ICONS = {"openvela": 36}

# Approved V9 pixels override only the sizes supplied here.
V9_SOURCE_DIR = ROOT / "demos/smart_home/res/icons/v9"

# Top/status/navigation and compact device glyphs need a native 20 px source.
SMALL_ICONS = {
    "air-conditioning", "bulb", "camera", "camera-off", "cloud-rain", "devices", "dots",
    "home", "microphone", "microphone-off", "moon", "propeller", "robot", "shield",
    "sparkle-2", "sun", "temperature", "volume", "wifi", "wifi-off",
    # Scene-menu glyphs sit in a 20 px slot on the home screen; without a
    # native 20 px bitmap the lookup would fall back to 32 px and overflow.
    "run", "television-classic", "zzz",
}

# These are the only glyphs used as visual anchors on the 1024x600 home page.
LARGE_ICONS = {
    "air-conditioning", "camera", "cloud-rain", "home", "home-shield",
    "music-circle-outline", "robot", "sparkle-2",
}

# Weather glyphs additionally get a native 64 px bitmap for the home badge.
WEATHER_ICONS = {"sun", "cloud-rain", "cloud", "cloud-cog"}


def symbol_name(stem: str, size: int) -> str:
    return "smart_home_icon_{}_{}".format(stem.replace("-", "_"), size)


def icon_sizes(stem: str) -> list[int]:
    if stem in COLOR_ICONS:
        return [COLOR_ICONS[stem]]
    sizes = [BASE_SIZE]
    if stem in SMALL_ICONS or (V9_SOURCE_DIR / "20" / (stem + ".png")).exists():
        sizes.insert(0, SMALL_SIZE)
    if stem in LARGE_ICONS:
        sizes.append(LARGE_SIZE)
    if stem in WEATHER_ICONS:
        sizes.append(WEATHER_SIZE)
    return sizes


def alpha_canvas(source: Path, size: int) -> Image.Image:
    """Return a consistently padded A8 icon canvas of *size* square pixels."""
    image = Image.open(source).convert("RGBA")
    alpha = image.getchannel("A")
    bbox = alpha.getbbox()
    if bbox is None:
        raise ValueError(f"icon has no visible pixels: {source}")

    cropped = alpha.crop(bbox)
    # Keep the visible ink within an 80% square.  This removes inconsistent
    # PNG whitespace while retaining enough breathing room for antialiasing.
    inner = max(1, int(round(size * 0.80)))
    scale = min(inner / cropped.width, inner / cropped.height)
    width = max(1, int(round(cropped.width * scale)))
    height = max(1, int(round(cropped.height * scale)))
    resampling = getattr(getattr(Image, "Resampling", Image), "LANCZOS")
    resized = cropped.resize((width, height), resampling)
    canvas = Image.new("L", (size, size), 0)
    canvas.paste(resized, ((size - width) // 2, (size - height) // 2))
    return canvas


def alpha_data(alpha: Image.Image) -> bytes:
    """Return full alpha bytes for the LVGL software renderer's A8 format."""
    return alpha.tobytes()


def color_data(source: Path, size: int) -> bytes:
    """Keep brand colors and padding; emit BGRA bytes for ARGB8888 LE."""
    image = Image.open(source).convert("RGBA")
    resampling = getattr(getattr(Image, "Resampling", Image), "LANCZOS")
    image.thumbnail((size, size), resampling)
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.paste(image, ((size - image.width) // 2,
                        (size - image.height) // 2))
    return canvas.tobytes("raw", "BGRA")


def byte_rows(data: bytes, width: int = 12) -> Iterable[str]:
    for offset in range(0, len(data), width):
        yield "    " + ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + width]) + ","


def write_icon(source: Path, size: int) -> None:
    stem = source.stem
    symbol = symbol_name(stem, size)
    pixel_source = V9_SOURCE_DIR / str(size) / source.name
    is_pixel = pixel_source.exists()
    # Only the weather glyphs get an oversized bitmap beyond the approved V9
    # sizes. They must stay full colour at that size, otherwise the upscaled
    # 96 px sun would fall back to an A8 mask and render as a monochrome
    # outline. Other icons keep their original per-size colour decision so
    # the 48 px assets stay A8.
    weather_upscale = stem in WEATHER_ICONS and size == WEATHER_SIZE
    has_v9_pixels = any((V9_SOURCE_DIR / str(s) / source.name).exists()
                        for s in (SMALL_SIZE, BASE_SIZE, LARGE_SIZE))
    is_color = (stem in COLOR_ICONS or is_pixel or
                (weather_upscale and has_v9_pixels))
    if is_pixel:
        # Keep the approved canvas, palette and exact alpha bytes.
        with Image.open(pixel_source) as pixels:
            if pixels.size != (size, size):
                raise ValueError(f"invalid native V9 size: {pixel_source}")
            data = pixels.convert("RGBA").tobytes("raw", "BGRA")
    elif has_v9_pixels:
        # No native bitmap at this size: upscale the largest approved V9 bitmap
        # with NEAREST so the pixel-art edges stay crisp instead of blurring.
        native = max(s for s in (SMALL_SIZE, BASE_SIZE, LARGE_SIZE)
                     if (V9_SOURCE_DIR / str(s) / source.name).exists())
        with Image.open(V9_SOURCE_DIR / str(native) / source.name) as pixels:
            scaled = pixels.convert("RGBA").resize((size, size), Image.NEAREST)
        data = scaled.tobytes("raw", "BGRA")
    else:
        data = color_data(source, size) if is_color else alpha_data(alpha_canvas(source, size))
    color_format = "LV_COLOR_FORMAT_ARGB8888" if is_color else "LV_COLOR_FORMAT_A8"
    stride = size * (4 if is_color else 1)
    output = OUTPUT_DIR / f"{stem}_{size}.c"
    lines = [
        "/* Generated by scripts/generate_smart_home_lvgl_icons.py; do not edit. */",
        "#include <lvgl/lvgl.h>",
        "",
        "#ifndef LV_ATTRIBUTE_MEM_ALIGN",
        "#define LV_ATTRIBUTE_MEM_ALIGN",
        "#endif",
        "",
        f"const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t {symbol}_map[] = {{",
        *byte_rows(data),
        "};",
        "",
        f"const lv_image_dsc_t {symbol} = {{",
        "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
        f"    .header.cf = {color_format},",
        "    .header.flags = 0,",
        f"    .header.w = {size},",
        f"    .header.h = {size},",
        f"    .header.stride = {stride},",
        f"    .data_size = sizeof({symbol}_map),",
        f"    .data = {symbol}_map,",
        "};",
        "",
    ]
    output.write_text("\n".join(lines), encoding="utf-8")


def write_header(sources: list[Path]) -> None:
    lines = [
        "/** Generated SmartHome PNG-derived LVGL icon declarations. */",
        "#pragma once",
        "",
        "#include <lvgl/lvgl.h>",
        "",
        "#ifdef __cplusplus",
        "extern \"C\" {",
        "#endif",
        "",
    ]
    for source in sources:
        for size in icon_sizes(source.stem):
            lines.append(f"extern const lv_image_dsc_t {symbol_name(source.stem, size)};")
    lines += [
        "",
        "/* `name` must use the stable `asset:<png-stem>` vocabulary. */",
        "const lv_image_dsc_t *smart_home_lvgl_png_icon_get(const char *name, int requested_size);",
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
        "",
    ]
    HEADER.write_text("\n".join(lines), encoding="utf-8")


def write_lookup(sources: list[Path]) -> None:
    lines = [
        "/* Generated by scripts/generate_smart_home_lvgl_icons.py; do not edit. */",
        "#include \"smart_home_lvgl_png_icons.h\"",
        "",
        "#include <string.h>",
        "",
        "typedef struct {",
        "    const char *name;",
        "    const lv_image_dsc_t *small;",
        "    const lv_image_dsc_t *base;",
        "    const lv_image_dsc_t *large;",
        "    const lv_image_dsc_t *weather;",
        "} smart_home_png_icon_entry_t;",
        "",
        "static const smart_home_png_icon_entry_t g_png_icons[] = {",
    ]
    for source in sources:
        stem = source.stem
        base_size = COLOR_ICONS.get(stem, BASE_SIZE)
        small = f"&{symbol_name(stem, SMALL_SIZE)}" if SMALL_SIZE in icon_sizes(stem) else "NULL"
        large = f"&{symbol_name(stem, LARGE_SIZE)}" if stem in LARGE_ICONS else "NULL"
        weather = (f"&{symbol_name(stem, WEATHER_SIZE)}"
                   if stem in WEATHER_ICONS else "NULL")
        lines.append(
            f'    {{"asset:{stem}", {small}, &{symbol_name(stem, base_size)}, '
            f'{large}, {weather}}},'
        )
    lines += [
        "};",
        "",
        "const lv_image_dsc_t *smart_home_lvgl_png_icon_get(const char *name, int requested_size)",
        "{",
        "    size_t i;",
        "",
        "    if (!name) {",
        "        return NULL;",
        "    }",
        "    for (i = 0; i < sizeof(g_png_icons) / sizeof(g_png_icons[0]); i++) {",
        "        const smart_home_png_icon_entry_t *entry = &g_png_icons[i];",
        "",
        "        if (strcmp(name, entry->name) != 0) {",
        "            continue;",
        "        }",
        "        if (requested_size <= 24 && entry->small) {",
        "            return entry->small;",
        "        }",
        "        if (requested_size > 32 && requested_size <= 48 && entry->large) {",
        "            return entry->large;",
        "        }",
        "        if (requested_size > 48 && entry->weather) {",
        "            return entry->weather;",
        "        }",
        "        return entry->base;",
        "    }",
        "    return NULL;",
        "}",
        "",
    ]
    LOOKUP_SOURCE.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    if not SOURCE_DIR.is_dir():
        raise SystemExit(f"missing source directory: {SOURCE_DIR}")
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    by_name = {p.name: p for p in SOURCE_DIR.glob("*.png")}
    for pixel in (V9_SOURCE_DIR / "32").glob("*.png"):
        by_name.setdefault(pixel.name, pixel)
    sources = sorted(by_name.values(), key=lambda p: p.name)
    if not sources:
        raise SystemExit(f"no PNG icons under: {SOURCE_DIR}")

    expected = set()
    for source in sources:
        for size in icon_sizes(source.stem):
            output = OUTPUT_DIR / f"{source.stem}_{size}.c"
            expected.add(output)
            write_icon(source, size)
    for stale in OUTPUT_DIR.glob("*.c"):
        if stale not in expected:
            stale.unlink()
    write_header(sources)
    write_lookup(sources)
    print(f"generated {len(expected)} LVGL images from {len(sources)} PNG sources")


if __name__ == "__main__":
    main()
