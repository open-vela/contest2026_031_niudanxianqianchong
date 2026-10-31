#!/usr/bin/env python3
"""Summarize [v9-perf] lines from a board serial log into a markdown table.

Usage:
    scripts/v9_perf_report.py board.log
    scripts/v9_perf_report.py board.log other.log --out report.md

The firmware prints one line every 5 s under CONFIG_SMART_HOME_V9_PERF_LOG:

    [v9-perf] frames=151 avg_ms=12 max_ms=48 heap_used=734112 heap_free=1823744 heap_largest=1032192 scene=home

Frame numbers are per-window (5 s); heap values are mallinfo() of the umm user
heap at the end of that window.  Scene labels come from the active screen, so
each P0x scenario must stay on one screen longer than 15 s to yield >= 3 rows.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import OrderedDict
from dataclasses import dataclass, field

LINE_RE = re.compile(
    r"\[v9-perf\]\s+frames=(?P<frames>\d+)\s+avg_ms=(?P<avg>\d+)\s+max_ms=(?P<max>\d+)"
    r"(?:\s+heap_used=(?P<used>\d+)\s+heap_free=(?P<free>\d+)\s+heap_largest=(?P<largest>\d+))?"
    r"(?:\s+scene=(?P<scene>[a-z-]+))?"
)

# Which scenario codes each scene contributes to.
SCENE_TO_SCENARIOS = {
    "home": "P02/P03",
    "chat": "P05",
    "settings": "P06",
    "screensaver": "P09",
    "security": "P07",
    "panel": "P03",
}


@dataclass
class SceneStats:
    windows: int = 0
    frames: int = 0
    weighted_ms: int = 0
    worst_ms: int = 0
    peak_used: int = 0
    min_free: int = 0
    min_largest: int = 0
    has_heap: bool = False

    @property
    def avg_ms(self) -> float:
        return self.weighted_ms / self.frames if self.frames else 0.0

    def add(self, frames: int, avg: int, worst: int,
            used: int | None, free: int | None, largest: int | None) -> None:
        self.windows += 1
        self.frames += frames
        self.weighted_ms += avg * frames
        self.worst_ms = max(self.worst_ms, worst)
        if used is not None:
            self.has_heap = True
            self.peak_used = max(self.peak_used, used)
        if free is not None:
            self.min_free = free if self.min_free == 0 else min(self.min_free, free)
        if largest is not None:
            self.min_largest = (largest if self.min_largest == 0
                                else min(self.min_largest, largest))


def parse(paths: list[str]) -> tuple[OrderedDict[str, SceneStats], int, int]:
    stats: OrderedDict[str, SceneStats] = OrderedDict()
    matched = 0
    unmatched = 0
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.replace("\r", "\n")
                for chunk in line.split("\n"):
                    if "v9-perf" not in chunk:
                        continue
                    found = LINE_RE.search(chunk)
                    if not found:
                        unmatched += 1
                        continue
                    matched += 1
                    scene = found.group("scene") or "unknown"
                    stats.setdefault(scene, SceneStats()).add(
                        int(found.group("frames")),
                        int(found.group("avg")),
                        int(found.group("max")),
                        int(found.group("used")) if found.group("used") else None,
                        int(found.group("free")) if found.group("free") else None,
                        int(found.group("largest")) if found.group("largest") else None,
                    )
    return stats, matched, unmatched


def render(stats: OrderedDict[str, SceneStats]) -> str:
    lines = [
        "| 场景 | P 编号 | 窗口数 | 总帧数 | 平均帧耗时 | 最坏帧耗时 | 峰值堆占用 | 最低剩余 | 最小最大块 |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for scene, item in stats.items():
        if item.has_heap:
            heap = (f"{item.peak_used} B", f"{item.min_free} B", f"{item.min_largest} B")
        else:
            heap = ("—", "—", "—")
        lines.append(
            f"| {scene} | {SCENE_TO_SCENARIOS.get(scene, '—')} | {item.windows} | {item.frames} "
            f"| {item.avg_ms:.1f} ms | {item.worst_ms} ms | {heap[0]} | {heap[1]} | {heap[2]} |"
        )
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", help="串口日志文件")
    parser.add_argument("--out", help="把 markdown 表写入该文件")
    args = parser.parse_args(argv)

    stats, matched, unmatched = parse(args.logs)
    if not matched:
        print("没有找到 [v9-perf] 行；确认固件打开了 CONFIG_SMART_HOME_V9_PERF_LOG。",
              file=sys.stderr)
        return 1

    table = render(stats)
    print(table)
    print()
    print(f"已解析 {matched} 行" + (f"，{unmatched} 行格式不符" if unmatched else ""))
    print("注意：平均帧耗时是窗口平均值，最坏帧耗时是窗口内单次最大值；"
          "都不是严格 p50/p95/p99。")
    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(table + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
