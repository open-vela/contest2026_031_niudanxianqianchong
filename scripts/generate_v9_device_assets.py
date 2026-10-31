#!/usr/bin/env python3
"""Generate V9 card-local device art from the approved 160x160 sources.

The source canvas is cropped once per device (union of on/off states), resized
with nearest-neighbour to the card-local target size and saved under
demos/smart_home/res/devices.  Do not re-center individual states: all states
share the same crop and anchor to avoid frame-to-frame jumping.
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

# scale, canvas offset, visible union bounds in card-local coordinates
DEVICES = {
    "light":  (0.875,  39.75, 111.25, (60.75, 142.75, 164, 232), "ceiling-light-on", "ceiling-light-off"),
    "ac":     (1.0,    30.0,   78.0,  (66, 94, 164, 232),         "standing-ac-cool", "standing-ac-off"),
    "washer": (0.9375, 34.625, 87.625,(64.625, 117.625, 164, 232), "washing-machine-wash", "washing-machine-idle"),
    "robot":  (0.8125, 47.0,  115.0,  (63.25, 173.5, 164, 232),   "robot-vacuum-clean", "robot-vacuum-idle"),
    "camera": (1.125,  26.75, 65.5,   (71.75, 121.75, 164, 232),  "camera-online", "camera-online"),
    "fan":    (1.0,    38.0,  80.0,   (76, 108, 164, 232),         "fan-run", "fan-off"),
}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    args = parser.parse_args()
    if not args.source.is_dir():
        raise SystemExit(f"source directory not found: {args.source}")
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


if __name__ == "__main__":
    main()
