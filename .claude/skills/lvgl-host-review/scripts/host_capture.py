#!/usr/bin/env python3
"""Render real LVGL pages on the host and export them as PNG/PPM.

The script never edits the tree it captures: it configures a CMake build in a
temporary directory, builds only the requested targets, then runs the test
binary with capture environment variables set.

Page recipes live in a JSON file so the script stays project-agnostic:

    {
      "target": "panel_check",
      "build": {"source": "demos/app/tests/lvgl", "openvela_root": "/home/u/openvela"},
      "pages": [
        {"name": "home", "env": "APP_CAPTURE", "screen": "home"},
        {"name": "settings", "env": "APP_SETTINGS_CAPTURE"}
      ]
    }

Usage:

    python3 host_capture.py --config pages.json --out /tmp/shots
    python3 host_capture.py --config pages.json --out /tmp/shots --pages home
    python3 host_capture.py --config pages.json --out /tmp/shots --build-dir /tmp/b
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from lvmeasure import load


def run(cmd, cwd=None, env=None, capture=True):
    """Run a command, echoing it, and return CompletedProcess."""
    printable = " ".join(str(c) for c in cmd)
    print(f"$ {printable}", flush=True)
    return subprocess.run(
        [str(c) for c in cmd], cwd=cwd, env=env,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
    )


def configure(source: Path, build_dir: Path, openvela_root: Path, extra: dict):
    build_dir.mkdir(parents=True, exist_ok=True)
    cmd = ["cmake", "-S", source, "-B", build_dir,
           f"-DOPENVELA_ROOT={openvela_root}"]
    for key, value in extra.items():
        cmd.append(f"-D{key}={value}")
    result = run(cmd)
    if result.returncode != 0:
        sys.stderr.write(result.stdout or "")
        raise SystemExit(f"cmake configure failed ({result.returncode})")


def build(build_dir: Path, targets: list[str], jobs: int, allow_partial: bool):
    """Build the requested targets.

    `--target X` compiles ONLY X. Running ctest afterwards then uses stale
    binaries for every other test and reports phantom failures, so by default
    this function builds ALL targets and only falls back to per-target builds
    (skipping the ones that fail) when the full build breaks -- a baseline can
    legitimately be uncompilable, and that must not be mistaken for a
    regression in the change under review.
    """
    result = run(["cmake", "--build", build_dir, "-j", jobs])
    if result.returncode == 0:
        return []
    if not allow_partial:
        sys.stderr.write(result.stdout or "")
        raise SystemExit("build failed; pass --allow-partial to capture anyway")

    print("!! full build failed; falling back to per-target builds",
          file=sys.stderr)
    sys.stderr.write((result.stdout or "")[-2000:])
    failed = []
    for target in targets:
        one = run(["cmake", "--build", build_dir, "--target", target, "-j", jobs])
        if one.returncode != 0:
            failed.append(target)
    return failed


def capture(build_dir: Path, target: str, pages: list[dict], out: Path,
            timeout: int) -> dict[str, Path]:
    """Run the test binary once with every capture env var set."""
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    wanted = {}
    for page in pages:
        ppm = out / f"{page['name']}.ppm"
        env[page["env"]] = str(ppm)
        wanted[page["name"]] = ppm
    binary = build_dir / target
    if not binary.exists():
        raise SystemExit(f"missing binary: {binary}")
    result = run([binary], cwd=build_dir, env=env)
    log = out / "host-output.txt"
    log.write_text(result.stdout or "", encoding="utf-8")
    if result.returncode not in (0, None):
        print(f"!! {target} exited with {result.returncode}; "
              f"captures before the failure are still usable", file=sys.stderr)

    produced = {}
    for name, ppm in wanted.items():
        if not ppm.exists():
            continue
        png = ppm.with_suffix(".png")
        im, _ = load(ppm)
        im.save(png)
        ppm.unlink()
        produced[name] = png
        print(f"   {name}: {png}  {im.width}x{im.height}")
    return produced


def load_config(path: Path) -> dict:
    config = json.loads(path.read_text(encoding="utf-8"))
    config.setdefault("target", "panel_check")
    build = config.setdefault("build", {})
    if "source" not in build:
        raise SystemExit("config.build.source is required")
    if "openvela_root" not in build:
        raise SystemExit("config.build.openvela_root is required")
    if not config.get("pages"):
        raise SystemExit("config.pages must list at least one page")
    return config


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", type=Path, required=True, help="page recipe JSON")
    ap.add_argument("--out", type=Path, required=True, help="screenshot directory")
    ap.add_argument("--build-dir", type=Path,
                    help="reuse this CMake build dir (default: temp under /tmp)")
    ap.add_argument("--pages", help="comma separated subset of page names")
    ap.add_argument("--jobs", default="8")
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--allow-partial", action="store_true",
                    help="keep going when the baseline itself does not build")
    ap.add_argument("--clean", action="store_true", help="delete build dir first")
    args = ap.parse_args(argv)

    config = load_config(args.config)
    root = Path(config["build"]["openvela_root"]).resolve()
    raw_source = Path(config["build"]["source"])
    # Relative paths resolve against the config file, not the caller's cwd, so
    # the recipe stays valid no matter where the script is invoked from.
    source = (raw_source if raw_source.is_absolute()
              else (args.config.parent / raw_source))
    source = source.resolve()
    if not source.is_dir():
        raise SystemExit(f"source directory not found: {source}")
    if not root.is_dir():
        raise SystemExit(f"openvela root not found: {root}")

    build_dir = args.build_dir or Path("/tmp/lvgl-host-review-build")
    if args.clean and build_dir.exists():
        shutil.rmtree(build_dir)

    pages = config["pages"]
    if args.pages:
        keep = {p.strip() for p in args.pages.split(",") if p.strip()}
        unknown = keep - {p["name"] for p in pages}
        if unknown:
            raise SystemExit(f"unknown page(s): {sorted(unknown)}")
        pages = [p for p in pages if p["name"] in keep]

    targets = sorted({config["target"]} |
                     {p.get("target", config["target"]) for p in pages})

    configure(source, build_dir, root,
              config["build"].get("options", {}))
    failed = build(build_dir, targets, args.jobs, args.allow_partial)
    if failed:
        print(f"!! could not build: {failed}", file=sys.stderr)

    ok = True
    for target in targets:
        if target in failed:
            ok = False
            continue
        subset = [p for p in pages if p.get("target", config["target"]) == target]
        if not subset:
            continue
        produced = capture(build_dir, target, subset, args.out, args.timeout)
        missing = {p["name"] for p in subset} - set(produced)
        if missing:
            print(f"!! {target} produced no image for: {sorted(missing)}",
                  file=sys.stderr)
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
