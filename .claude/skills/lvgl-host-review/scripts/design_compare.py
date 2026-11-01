#!/usr/bin/env python3
"""Compare host-rendered screenshots against design references.

Produces, per page:

  * a side-by-side PNG (design | host) at 1:1,
  * a region diff table so you can tell "layout is wrong" from
    "font rasterisation differs".

Usage:

    python3 design_compare.py --host /tmp/shots --design out/design/designs/png \
        --out /tmp/review
    python3 design_compare.py --host /tmp/shots --design ... --pairs map.json

`map.json` maps host page name -> design file stem when they differ:

    {"home": "03-devices", "settings": "09-settings"}

Pages with no design counterpart are skipped with a note, never silently
passed.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from lvmeasure import DiffStat, load, region_diff

#: Coarse regions used for the diff table. Each is (label, x, y, w, h) in
#: absolute screen pixels; adjust with --regions if the layout differs.
DEFAULT_REGIONS = [
    ("top bar", 0, 0, 1024, 56),
    ("title area", 24, 56, 1000, 60),
    ("body", 24, 116, 976, 400),
    ("bottom bar", 0, 536, 1024, 64),
]


def make_side_by_side(design_path: Path, host_path: Path, out: Path,
                      title: str, crop: tuple[int, int, int, int] | None):
    from PIL import Image, ImageDraw, ImageFont

    d_im = Image.open(design_path).convert("RGB")
    h_im = Image.open(host_path).convert("RGB")
    if crop:
        x, y, w, h = crop
        box = (x, y, x + w, y + h)
        d_im = d_im.crop(box)
        h_im = h_im.crop(box)
    w, h = d_im.size
    pad = 30
    canvas = Image.new("RGB", (w * 2 + 30, h + 50), "#dfe7e4")
    draw = ImageDraw.Draw(canvas)
    try:
        font = ImageFont.load_default(size=16)
    except TypeError:                      # Pillow < 10
        font = ImageFont.load_default()
    draw.text((10, 8), f"{title}  design", fill="#314956", font=font)
    draw.text((w + 20, 8), f"{title}  host", fill="#314956", font=font)
    canvas.paste(d_im, (10, pad + 10))
    canvas.paste(h_im, (w + 20, pad + 10))
    canvas.save(out)
    return (w, h)


def compare_pages(host_dir: Path, design_dir: Path, out_dir: Path,
                  mapping: dict[str, str], regions, crop) -> list[tuple]:
    rows = []
    for host_png in sorted(host_dir.glob("*.png")):
        name = host_png.stem
        design_name = mapping.get(name, name)
        candidates = [design_dir / f"{design_name}.png"] if design_name else []
        design_png = next((c for c in candidates if c.exists()), None)
        if design_png is None:
            print(f"- {name}: no design reference, skipped")
            rows.append((name, None, None))
            continue

        d_im, d_px = load(design_png)
        h_im, h_px = load(host_png)
        if d_im.size != h_im.size:
            print(f"! {name}: size mismatch design={d_im.size} host={h_im.size}")
        size = (min(d_im.width, h_im.width), min(d_im.height, h_im.height))

        out_png = out_dir / f"compare-{name}.png"
        make_side_by_side(design_png, host_png, out_png, name, crop)

        per_region = []
        for label, x, y, w, h in regions:
            box = (x, y, min(x + w, size[0]), min(y + h, size[1]))
            if box[2] <= box[0] or box[3] <= box[1]:
                continue
            stat: DiffStat = region_diff(d_px, h_px, box)
            per_region.append((label, stat))
        rows.append((name, per_region, out_png))
    return rows


def print_table(rows) -> None:
    print("\n| page | region | diff>40 | ratio | mean |")
    print("| --- | --- | ---: | ---: | ---: |")
    for name, per_region, _ in rows:
        if per_region is None:
            print(f"| {name} | (no reference) | - | - | - |")
            continue
        for label, stat in per_region:
            print(f"| {name} | {label} | {stat.strong} | "
                  f"{stat.strong_ratio * 100:.1f}% | {stat.mean:.1f} |")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", type=Path, required=True, help="host screenshots")
    ap.add_argument("--design", type=Path, required=True, help="design PNG dir")
    ap.add_argument("--out", type=Path, required=True, help="review output dir")
    ap.add_argument("--pairs", type=Path, help="JSON map host name -> design stem")
    ap.add_argument("--crop", help="x,y,w,h region to crop for the side-by-side")
    ap.add_argument("--regions-json", type=Path,
                    help="override the diff regions with a JSON list")
    args = ap.parse_args(argv)

    if not args.host.is_dir():
        raise SystemExit(f"host dir not found: {args.host}")
    if not args.design.is_dir():
        raise SystemExit(f"design dir not found: {args.design}")
    args.out.mkdir(parents=True, exist_ok=True)

    mapping = {}
    if args.pairs:
        mapping = json.loads(args.pairs.read_text(encoding="utf-8"))

    regions = DEFAULT_REGIONS
    if args.regions_json:
        regions = [tuple(r) for r in
                   json.loads(args.regions_json.read_text(encoding="utf-8"))]

    crop = None
    if args.crop:
        crop = tuple(int(v) for v in args.crop.split(","))
        if len(crop) != 4:
            raise SystemExit("--crop expects x,y,w,h")

    rows = compare_pages(args.host, args.design, args.out, mapping,
                         regions, crop)
    print_table(rows)

    compared = sum(1 for _, r, _ in rows if r is not None)
    print(f"\n{compared}/{len(rows)} pages compared; images in {args.out}")
    print("Reminder: a high diff in a text region is usually font metrics; "
          "check the side-by-side before calling it a layout bug.")
    return 0 if compared else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
