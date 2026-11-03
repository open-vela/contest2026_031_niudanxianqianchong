#!/usr/bin/env python3
"""Summarize [v9-perf] lines from a board serial log.

Usage:
    scripts/v9_perf_report.py board.log
    scripts/v9_perf_report.py board.log --out report.md
    scripts/v9_perf_report.py board.log --fill docs/.../2026-10-31-V9-P01-P12-回填表.md
    scripts/v9_perf_report.py board.log --gate

The firmware prints one line every 5 s under CONFIG_SMART_HOME_V9_PERF_LOG:

    [v9-perf] frames=151 avg_ms=12 max_ms=48 heap_used=734112 heap_free=1823744 heap_largest=1032192 scene=home

Two grouping modes:

* MARK mode (preferred): before each scenario type  echo MARK P03  at the NSH
  prompt.  Lines containing  MARK P0x  are scenario boundaries, so every row is
  attributed exactly.
* SCENE mode (fallback): without marks the active screen label groups the rows,
  which cannot tell P02 from P03 because both run on the home screen.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import OrderedDict
from dataclasses import dataclass

PERF_RE = re.compile(
    r"\[v9-perf\]\s+frames=(?P<frames>\d+)\s+avg_ms=(?P<avg>\d+)\s+max_ms=(?P<max>\d+)"
    r"(?:\s+heap_used=(?P<used>\d+)\s+heap_free=(?P<free>\d+)\s+heap_largest=(?P<largest>\d+))?"
    r"(?:\s+scene=(?P<scene>[a-z-]+))?"
)
MARK_RE = re.compile(r"\bMARK\s+(P\d{2})\b")
ROW_RE = re.compile(r"^\|\s*(P\d{2})\s*\|")


@dataclass
class Stats:
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

    def add(self, frames, avg, worst, used, free, largest) -> None:
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


def parse(paths):
    grouped = OrderedDict()
    mode = "scene"
    current = None
    unmatched = 0
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                for chunk in line.replace("\r", "\n").split("\n"):
                    mark = MARK_RE.search(chunk)
                    if mark:
                        mode = "mark"
                        current = mark.group(1)
                        grouped.setdefault(current, Stats())
                        continue
                    if "v9-perf" not in chunk:
                        continue
                    found = PERF_RE.search(chunk)
                    if not found:
                        unmatched += 1
                        continue
                    key = current if mode == "mark" else (found.group("scene") or "unknown")
                    if key is None:
                        key = "before-mark"
                    grouped.setdefault(key, Stats()).add(
                        int(found.group("frames")), int(found.group("avg")),
                        int(found.group("max")),
                        int(found.group("used")) if found.group("used") else None,
                        int(found.group("free")) if found.group("free") else None,
                        int(found.group("largest")) if found.group("largest") else None)
    return grouped, mode, unmatched


def render(grouped, mode):
    head = "P 编号" if mode == "mark" else "场景"
    lines = [
        "| " + head + " | 窗口数 | 总帧数 | 平均帧耗时 | 最坏帧 | 峰值堆占用 | 最低剩余 | 最小最大块 |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for key, item in grouped.items():
        heap = ((str(item.peak_used), str(item.min_free), str(item.min_largest))
                if item.has_heap else ("—", "—", "—"))
        lines.append(
            "| " + key + " | " + str(item.windows) + " | " + str(item.frames)
            + " | " + format(item.avg_ms, ".1f") + " ms | " + str(item.worst_ms) + " ms"
            + " | " + heap[0] + " | " + heap[1] + " | " + heap[2] + " |")
    return "\n".join(lines)


def fill_table(path, grouped):
    with open(path, "r", encoding="utf-8") as handle:
        rows = handle.readlines()
    filled = 0
    for index, line in enumerate(rows):
        match = ROW_RE.match(line)
        if not match:
            continue
        stats = grouped.get(match.group(1))
        if stats is None or not stats.windows:
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) < 10:
            continue
        cells[2] = str(stats.windows)
        cells[3] = str(stats.frames)
        cells[4] = format(stats.avg_ms, ".1f") + " ms"
        cells[5] = str(stats.worst_ms) + " ms"
        cells[6] = str(stats.peak_used) + " B" if stats.has_heap else "—"
        cells[7] = str(stats.min_free) + " B" if stats.has_heap else "—"
        cells[8] = str(stats.min_largest) + " B" if stats.has_heap else "—"
        cells[9] = "已测"
        rows[index] = "| " + " | ".join(cells) + " |\n"
        filled += 1
    with open(path, "w", encoding="utf-8") as handle:
        handle.writelines(rows)
    return filled


def evaluate_gate(grouped, image_bytes, stable):
    checks = []
    p03 = grouped.get("P03")
    p12 = grouped.get("P12")
    for label, stats in (("P03", p03), ("P12", p12)):
        if not stats or not stats.windows:
            checks.append((label + " 平均帧耗时", "≤ 33 ms", "无数据", False))
            checks.append((label + " 最坏帧", "≤ 80 ms", "无数据", False))
            continue
        checks.append((label + " 平均帧耗时", "≤ 33 ms",
                       format(stats.avg_ms, ".1f") + " ms", stats.avg_ms <= 33.0))
        checks.append((label + " 最坏帧", "≤ 80 ms",
                       str(stats.worst_ms) + " ms", stats.worst_ms <= 80))
    largest = p12.min_largest if p12 and p12.has_heap else 0
    free = p12.min_free if p12 and p12.has_heap else 0
    checks.append(("P12 heap_largest", "≥ 1,310,720 B", str(largest) + " B",
                   largest >= 1310720))
    checks.append(("P12 heap_free", "≥ 1,048,576 B", str(free) + " B",
                   free >= 1048576))
    if image_bytes is None:
        checks.append(("数据镜像", "≤ 10,485,760 B", "未提供", False))
    else:
        checks.append(("数据镜像", "≤ 10,485,760 B", str(image_bytes) + " B",
                       image_bytes <= 10485760))
    checks.append(("10 分钟稳定性", "无复位/丢触",
                   "通过" if stable else "未确认", bool(stable)))
    return checks


def main(argv):
    parser = argparse.ArgumentParser(description="汇总 [v9-perf] 日志")
    parser.add_argument("logs", nargs="+")
    parser.add_argument("--out", help="把汇总表写到该文件")
    parser.add_argument("--fill", help="把数值回填到 P01-P12 回填表")
    parser.add_argument("--gate", action="store_true", help="判定像素帧动画闸门")
    parser.add_argument("--image-bytes", type=int, help="含动画包的数据镜像字节数")
    parser.add_argument("--stability-ok", action="store_true", help="10 分钟稳定性已确认")
    args = parser.parse_args(argv)

    grouped, mode, unmatched = parse(args.logs)
    if not grouped:
        print("没有找到 [v9-perf] 行；确认固件打开了 CONFIG_SMART_HOME_V9_PERF_LOG。",
              file=sys.stderr)
        return 1

    table = render(grouped, mode)
    print(table)
    print()
    print("分组方式: " + ("MARK（echo MARK P0x，逐场景精确）" if mode == "mark"
                         else "SCENE（按活动屏幕，无法区分 P02/P03）"))
    if unmatched:
        print("有 " + str(unmatched) + " 行 [v9-perf] 格式不符，已跳过")
    print("注意：平均帧耗时是窗口平均值，最坏帧是窗口内单次最大值，都不是严格 p95/p99。")

    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(table + "\n")
        print("汇总表写入 " + args.out)

    if args.fill:
        count = fill_table(args.fill, grouped)
        print("回填 " + str(count) + " 行到 " + args.fill)

    if args.gate:
        checks = evaluate_gate(grouped, args.image_bytes, args.stability_ok)
        print()
        print("| 条件 | 阈值 | 实测 | 结果 |")
        print("| --- | --- | --- | --- |")
        for name, limit, value, ok in checks:
            print("| " + name + " | " + limit + " | " + value + " | "
                  + ("满足" if ok else "未满足") + " |")
        passed = all(item[3] for item in checks)
        print()
        print("动画准入: " + ("通过，可把 CONFIG_SMART_HOME_V9_DEVICE_ANIMATION 改为 y"
                              if passed else "不通过，保持 n 并记录未达标项"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
