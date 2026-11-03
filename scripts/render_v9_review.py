#!/usr/bin/env python3
"""Export real Host LVGL pages and a contact sheet for V9 visual review."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
PAGES = (
    ("home", "首页", "SMART_HOME_V9_CAPTURE", "03-devices"),
    ("chat", "聊天", "SMART_HOME_V9_CHAT_CAPTURE", "11-chat"),
    ("settings", "设置 · 显示与声音", "SMART_HOME_V9_SETTINGS_CAPTURE", "09-settings"),
    ("model", "设置 · 模型服务", "SMART_HOME_V9_MODEL_CAPTURE", "13-model"),
    ("network", "设置 · 网络连接", "SMART_HOME_V9_NETWORK_CAPTURE", None),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True, help="built panel_check")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--design-root", type=Path, help="original V9 design directory")
    args = parser.parse_args()
    destination = args.out.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    for name, _, key, _ in PAGES:
        env[key] = str(destination / (name + ".ppm"))
    result = subprocess.run([str(args.binary.resolve())], cwd=ROOT, env=env,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (destination / "host-output.txt").write_text(result.stdout)
    result.check_returncode()
    for name, _, _, _ in PAGES:
        ppm = destination / (name + ".ppm")
        with Image.open(ppm) as source:
            source.save(destination / (name + ".png"))
        ppm.unlink()

    font = ImageFont.truetype(str(ROOT / "demos/smart_home/res/fonts/MiSans-Normal.ttf"), 20)
    overview = Image.new("RGB", (1056, 704), "#e4ece9")
    draw = ImageDraw.Draw(overview)
    for index, (name, title, _, _) in enumerate(PAGES[:4]):
        x, y = 8 + (index % 2) * 528, 8 + (index // 2) * 352
        draw.text((x + 8, y), title + " · LVGL 实际渲染", fill="#314956", font=font)
        with Image.open(destination / (name + ".png")) as page:
            overview.paste(page.resize((512, 300), getattr(Image, "Resampling", Image).NEAREST), (x, y + 32))
    overview.save(destination / "overview.png")

    if args.design_root:
        comparison = Image.new("RGB", (2072, 2640), "#e4ece9")
        draw = ImageDraw.Draw(comparison)
        for row, (name, title, _, original) in enumerate(PAGES[:4]):
            y = row * 660
            draw.text((12, y + 8), title + " · 原 V9 参考", fill="#314956", font=font)
            draw.text((1048, y + 8), title + " · LVGL / MiSans / RGB565", fill="#314956", font=font)
            with Image.open(args.design_root / "designs/png" / (original + ".png")) as page:
                comparison.paste(page.convert("RGB"), (8, y + 40))
            with Image.open(destination / (name + ".png")) as page:
                comparison.paste(page, (1040, y + 40))
        comparison.save(destination / "comparison.png")

    paths = list((ROOT / "demos/smart_home/src/ui/lvgl").glob("*.c"))
    paths += list((ROOT / "demos/smart_home/src/ui/lvgl").glob("*.h"))
    paths += list((ROOT / "demos/smart_home/src/ui/lvgl/icons").rglob("*.c"))
    paths += list((ROOT / "demos/smart_home/tests/lvgl").glob("*.c"))
    paths += list((ROOT / "demos/smart_home/src/ui/lvgl/images").glob("*.h"))
    paths += [ROOT / "scripts/generate_smart_home_lvgl_icons.py",
              ROOT / "scripts/render_v9_review.py",
              ROOT / "demos/smart_home/res/icons/v9/manifest.json",
              ROOT / "demos/smart_home/res/fonts/MiSans-Normal.ttf"]
    metadata = {
        "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "source_state": "working tree, including uncommitted changes",
        "renderer": "actual LVGL Host RGB565 framebuffer; no AI-generated UI",
        "fixtures": "local test doubles; 2026-10-30 19:39, rain 22 C, example devices and conversation",
        "files_sha256": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in sorted(paths)},
    }
    (destination / "provenance.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n")
    print(destination / "overview.png")


if __name__ == "__main__":
    main()
