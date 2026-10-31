#!/usr/bin/env python3
"""Verify linked V9 pixel bytes against the approved PNGs and manifest."""
import hashlib
import json
from pathlib import Path
import re
import unittest
from PIL import Image

ROOT = Path(__file__).resolve().parents[3]
ASSETS = ROOT / "demos/smart_home/res/icons/v9"
GENERATED = ROOT / "demos/smart_home/src/ui/lvgl/icons/generated"


class PixelAssetTest(unittest.TestCase):
    def test_native_pixels_and_palette_survive_conversion(self):
        manifest = json.loads((ASSETS / "manifest.json").read_text())
        self.assertGreaterEqual(len(manifest["assets"]), 40)
        for entry in manifest["assets"]:
            with self.subTest(asset=entry["asset"], size=entry["size"]):
                source = ASSETS / entry["file"]
                self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), entry["sha256"])
                with Image.open(source) as png:
                    self.assertEqual(png.size, (entry["size"], entry["size"]))
                    expected = png.convert("RGBA").tobytes("raw", "BGRA")
                descriptor = (GENERATED / f'{entry["asset"]}_{entry["size"]}.c').read_text()
                self.assertIn("LV_COLOR_FORMAT_ARGB8888", descriptor)
                self.assertIn(f'.header.stride = {entry["size"] * 4}', descriptor)
                array = descriptor.split("_map[] = {", 1)[1].split("};", 1)[0]
                actual = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", array))
                self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main()
