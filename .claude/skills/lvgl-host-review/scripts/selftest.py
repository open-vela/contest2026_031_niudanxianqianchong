#!/usr/bin/env python3
"""Self-test for the skill scripts. No project paths, no LVGL build needed.

Run from the skill directory:

    python3 scripts/selftest.py

It synthesises two small images that share a known layout and verifies that
lvmeasure reports the offsets correctly, then exercises the CLI argument
handling of the two driver scripts.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lvmeasure import (color_bbox, coverage, dark_bbox, hexstr, load,
                       region_diff, text_bands)

HERE = Path(__file__).resolve().parent
BG = (0xEE, 0xEE, 0xEE)
CARD = (0xFF, 0xFA, 0xF6)


def build_reference(path: Path, card_x: int, text_y: int) -> None:
    im = Image.new("RGB", (200, 200), BG)
    draw = ImageDraw.Draw(im)
    draw.rectangle([card_x, 40, card_x + 99, 139], fill=CARD)
    draw.rectangle([card_x + 10, text_y, card_x + 79, text_y + 9],
                   fill=(0x31, 0x49, 0x56))
    im.save(path)


def check(name: str, ok: bool, detail: str = "") -> bool:
    print(f"{'ok  ' if ok else 'FAIL'} {name}{' -- ' + detail if detail else ''}")
    return ok


def main() -> int:
    passed = True
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        a = tmpdir / "a.png"
        b = tmpdir / "b.png"
        build_reference(a, 20, 60)
        build_reference(b, 26, 60)          # card 6 px right, text same
        im, a_px = load(a)
        _, b_px = load(b)

        box = color_bbox(a_px, im.size, CARD, box=(0, 0, 200, 200))
        passed &= check("color_bbox finds the card", box == (20, 40, 100, 100),
                        str(box))

        wide = color_bbox(a_px, im.size, BG)
        passed &= check("color_bbox union covers the page",
                        wide == (0, 0, 200, 200), str(wide))

        bands = text_bands(a_px, 0, 200, 0, 200)
        passed &= check("text_bands finds one line at y=60",
                        bands == [(60, 69)], str(bands))

        ink = dark_bbox(a_px, im.size, (0, 0, 200, 200))
        passed &= check("dark_bbox spans the text",
                        ink is not None and ink[1] == 60, str(ink))

        cov = coverage(a_px, im.size, (20, 40, 120, 140), BG)
        passed &= check("coverage of a filled card is 1.0",
                        abs(cov - 1.0) < 0.01, f"{cov:.3f}")

        same = region_diff(a_px, a_px, (0, 0, 200, 200))
        passed &= check("identical images diff to zero",
                        same.strong == 0 and same.mean == 0.0, str(same))

        # A filled rectangle shifted by 6 px only changes ink at the EDGES:
        # interior pixels keep the same colour. 120 px is the two 6x100 edge
        # strips, which is exactly why "diff count" alone cannot size a shift.
        moved = region_diff(a_px, b_px, (0, 0, 200, 200))
        passed &= check("6 px shift registers an edge-only diff",
                        100 <= moved.strong <= 300, str(moved))

        passed &= check("hexstr formats", hexstr((255, 250, 246)) == "#FFFAF6")

        # CLI: unknown page must fail, not silently succeed.
        cfg = tmpdir / "pages.json"
        cfg.write_text(
            '{"target": "x", "build": {"source": ".", '
            '"openvela_root": "."}, "pages": [{"name": "a", "env": "E"}]}',
            encoding="utf-8")
        result = subprocess.run(
            [sys.executable, str(HERE / "host_capture.py"),
             "--config", str(cfg), "--out", str(tmpdir / "o"),
             "--pages", "missing"],
            capture_output=True, text=True)
        passed &= check("unknown page exits non-zero",
                        result.returncode != 0,
                        result.stderr.strip().splitlines()[-1]
                        if result.stderr.strip() else "")

        result = subprocess.run(
            [sys.executable, str(HERE / "design_compare.py"),
             "--host", str(tmpdir), "--design", str(tmpdir / "nope"),
             "--out", str(tmpdir / "o2")],
            capture_output=True, text=True)
        passed &= check("missing design dir exits non-zero",
                        result.returncode != 0)

    print()
    print("all checks passed" if passed else "FAILURES above")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
