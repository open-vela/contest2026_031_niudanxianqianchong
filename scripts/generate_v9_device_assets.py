#!/usr/bin/env python3
"""Generate V9 card art, keeping modified pixel-art states on one shared grid.

The unchanged devices retain their V9 design-source transforms. The chandelier
and camera use checked-in modern source images and are cropped without
resampling so state changes cannot shift or blur the artwork.
"""

from __future__ import annotations

import argparse
import hashlib
import math
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = Path(
    "/home/arongw/openvela/out/smart-home-ui-design-v9-card-layout-20261030/"
    "assets/devices"
)
OUTPUT = ROOT / "demos/smart_home/res/devices"
MODERN_SOURCE = OUTPUT / "source"

# scale, canvas offset, visible union bounds in card-local coordinates
DEVICES = {
    "ac":     (1.0,    30.0,   78.0,  (66, 94, 164, 232),         "standing-ac-cool", "standing-ac-off"),
    "washer": (0.9375, 34.625, 87.625,(64.625, 117.625, 164, 232), "washing-machine-wash", "washing-machine-idle"),
    "robot":  (0.8125, 47.0,  115.0,  (63.25, 173.5, 164, 232),   "robot-vacuum-clean", "robot-vacuum-idle"),
    "fan":    (1.0,    38.0,  80.0,   (76, 108, 164, 232),         "fan-run", "fan-off"),
}

MODERN_STATES = {
    "light": {
        "on": ("chandelier-on", "light-on"),
        "off": ("chandelier-off", "light-off"),
    },
    "camera": {
        "offline": ("camera-offline", "camera-offline"),
        "online": ("camera-online", "camera-online"),
        "patrol": ("camera-patrol", "camera-patrol"),
    },
}


def generate_modern_assets() -> None:
    """Crop each state family to one shared alpha box without resampling."""
    for device, states in MODERN_STATES.items():
        opened = {
            source: Image.open(MODERN_SOURCE / f"{source}.png").convert("RGBA")
            for source, _ in states.values()
        }
        bounds = [image.getchannel("A").getbbox() for image in opened.values()]
        if any(box is None for box in bounds):
            raise SystemExit(f"empty device art in {MODERN_SOURCE}: {device}")
        crop = (
            min(box[0] for box in bounds),
            min(box[1] for box in bounds),
            max(box[2] for box in bounds),
            max(box[3] for box in bounds),
        )
        for source_name, output_name in states.values():
            image = opened[source_name].crop(crop)
            destination = OUTPUT / f"{output_name}.png"
            image.save(destination)
            digest = hashlib.sha256(destination.read_bytes()).hexdigest()[:12]
            print(f"{output_name}: {image.width}x{image.height} sha256={digest}")

    for obsolete in ("camera-on.png", "camera-off.png"):
        (OUTPUT / obsolete).unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    args = parser.parse_args()
    if not args.source.is_dir():
        raise SystemExit(f"source directory not found: {args.source}")
    if not MODERN_SOURCE.is_dir():
        raise SystemExit(f"modern source directory not found: {MODERN_SOURCE}")
    OUTPUT.mkdir(parents=True, exist_ok=True)

    for name, (scale, ox, oy, vis, on_name, off_name) in DEVICES.items():
        left, top, right, bottom = vis
        box = (
            math.floor((left - ox) / scale),
            math.floor((top - oy) / scale),
            math.ceil((right - ox) / scale),
            math.ceil((bottom - oy) / scale),
        )
        width = int(round((box[2] - box[0]) * scale))
        height = int(round((box[3] - box[1]) * scale))
        place = (int(round(ox + box[0] * scale)), int(round(oy + box[1] * scale)))

        for state, source_name in (("on", on_name), ("off", off_name)):
            src_path = args.source / f"{source_name}.png"
            image = Image.open(src_path).convert("RGBA").crop(box)
            image = image.resize((width, height), Image.NEAREST)
            dst = OUTPUT / f"{name}-{state}.png"
            image.save(dst)
            digest = hashlib.sha256(dst.read_bytes()).hexdigest()[:12]
            print(f"{name}-{state}: {width}x{height} @ {place} sha256={digest}")

    generate_modern_assets()


if __name__ == "__main__":
    main()
