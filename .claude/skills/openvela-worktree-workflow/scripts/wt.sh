#!/usr/bin/env bash
# wt.sh — contest 仓库 worktree 管理助手（RULE.md 配套工具）
#
# 用法:
#   wt.sh check              会话自检：分支/脏区/是否安全可写
#   wt.sh new <topic> [agent] 创建 ../worktrees/<topic>-<agent> 并切到该分支
#   wt.sh list               列出全部 worktree 及其分支
#   wt.sh done <worktree路径> 收尾：校验无未提交改动后移除 worktree
#
# 规则依据: RULE.md（主检出只读；写操作一律 worktree；未提交改动不得过夜）

set -euo pipefail

usage() { grep '^#' "$0" | head -12; exit 1; }

REPO=$(git rev-parse --show-toplevel 2>/dev/null) || {
  echo "错误: 不在 git 仓库内运行" >&2; exit 1; }
BRANCHES_NAME=$(basename "$REPO")
WTROOT="$(dirname "$REPO")/worktrees"
MAIN_BRANCH="contest2026"

cmd=${1:-}; shift || true
case "$cmd" in
  check)
    cur=$(git branch --show-current)
    dirty=$(git status --porcelain | wc -l)
    echo "检出路径 : $REPO"
    echo "当前分支 : $cur"
    echo "未提交项 : $dirty"
    if [ "$dirty" -gt 0 ]; then
      echo "状态     : ⚠ 有未提交改动 — 先 stash 或 WIP commit，禁止 reset/checkout --/clean"
    elif [ "$cur" = "$MAIN_BRANCH" ]; then
      echo "状态     : ✅ 主检出安全（只读/验证/fast-forward；写操作去 wt.sh new）"
    else
      echo "状态     : ⚠ 非主分支 — 确认是否应在 worktree 中工作"
    fi
    ;;
  new)
    topic=${1:?用法: wt.sh new <topic> [agent]}; shift || true
    agent=${1:-$(id -un)}
    name="${topic}-${agent}"
    path="$WTROOT/$name"
    if [ -e "$path" ]; then
      echo "错误: $path 已存在（agent 撞车？先 wt.sh list 协商）" >&2; exit 1
    fi
    git -C "$REPO" worktree add "$path" -b "feat/${name}" "$MAIN_BRANCH"
    echo "✅ worktree 就绪: $path (分支 feat/${name}，基于 $MAIN_BRANCH)"
    echo "   构建: cd $path && ../../contest2026_031_niudanxianqianchong/scripts/build_p4x.sh <config> -j8"
    ;;
  list)
    git -C "$REPO" worktree list
    ;;
  done)
    path=${1:?用法: wt.sh done <worktree路径>}
    [ -d "$path" ] || { echo "错误: $path 不存在" >&2; exit 1; }
    dirty=$(git -C "$path" status --porcelain | wc -l)
    if [ "$dirty" -gt 0 ]; then
      echo "错误: $path 还有 $dirty 处未提交改动 — 先 stash 或 WIP commit（规则五）" >&2
      exit 1
    fi
    branch=$(git -C "$path" branch --show-current)
    git -C "$REPO" worktree remove "$path"
    echo "✅ 已移除 $path（分支 $branch 保留，合并/删除请回主检出 fast-forward 处理）"
    ;;
  *)
    usage
    ;;
esac
