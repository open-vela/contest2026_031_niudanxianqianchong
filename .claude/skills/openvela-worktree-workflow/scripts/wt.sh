#!/usr/bin/env bash
# wt.sh — contest 仓库 worktree 管理助手（RULE.md 配套工具）
#
# 用法:
#   wt.sh check              会话自检：分支/脏区/是否安全可写
#   wt.sh new <topic> [agent] 创建 ../worktrees/<topic>-<agent> 并切到该分支
#   wt.sh list               列出全部 worktree 及其分支
#   wt.sh finish <worktree路径>
#                            标准收尾：已合并进 contest2026 的分支，
#                            校验通过后移除 worktree 并删除分支
#   wt.sh done <worktree路径>
#                            手动收尾（特殊出口）：仅校验无未提交改动后
#                            移除 worktree，分支保留不删
#
# 规则依据: RULE.md（主检出只读；写操作一律 worktree；未提交改动不得过夜；
#           收尾一律 finish，done 仅限分支不删的场景）

set -euo pipefail

usage() { grep '^#' "$0" | head -14; exit 1; }

REPO=$(cd "$(dirname "$0")/../../.." && git rev-parse --show-toplevel 2>/dev/null) || {
  echo "错误: 无法从脚本位置定位 git 仓库" >&2; exit 1; }
WTROOT="$(dirname "$REPO")/worktrees"
MAIN_BRANCH="contest2026"

cmd=${1:-}; shift || true
case "$cmd" in
  check)
    cur=$(git -C "$REPO" branch --show-current)
    dirty=$(git -C "$REPO" status --porcelain | wc -l)
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
    echo "   构建: cd $path && $REPO/scripts/build_p4x.sh <config> -j8"
    echo 
    ;;
  list)
    git -C "$REPO" worktree list
    ;;
  finish)
    path=${1:?用法: wt.sh finish <worktree路径>}
    [ -d "$path" ] || { echo "错误: $path 不存在" >&2; exit 1; }

    # ① 清理滞留的 worktree 登记（路径已删除的），否则分支删除会被占用误报
    git -C "$REPO" worktree prune

    # ② worktree 必须干净
    dirty=$(git -C "$path" status --porcelain | wc -l)
    if [ "$dirty" -gt 0 ]; then
      echo "错误: $path 还有 $dirty 处未提交改动 — 先提交或 stash（规则五）" >&2
      exit 1
    fi

    branch=$(git -C "$path" branch --show-current)
    if [ -z "$branch" ]; then
      echo "错误: $path 处于 detached HEAD，无分支可合并 — 请改用 wt.sh done" >&2
      exit 1
    fi

    # ③ 分支必须已并入主分支（不代劳合并；未合并先回主检出 fast-forward）
    if ! git -C "$REPO" merge-base --is-ancestor "$branch" "$MAIN_BRANCH" 2>/dev/null; then
      unmerged=$(git -C "$REPO" rev-list --count "$MAIN_BRANCH..$branch")
      echo "错误: 分支 $branch 尚未并入 $MAIN_BRANCH（领先 $unmerged 个提交）" >&2
      echo "      请先在主检出完成 fast-forward 合并，再执行 finish" >&2
      exit 1
    fi

    # ④ 主检出必须在主分支且干净
    cur=$(git -C "$REPO" branch --show-current)
    maindirty=$(git -C "$REPO" status --porcelain | wc -l)
    if [ "$cur" != "$MAIN_BRANCH" ] || [ "$maindirty" -gt 0 ]; then
      echo "错误: 主检出状态异常（分支=$cur 未提交=$maindirty）—" >&2
      echo "      先让主检出回到干净的 $MAIN_BRANCH，再执行 finish" >&2
      exit 1
    fi

    # ⑤ 移除 worktree + 删除已合并分支（-d 自带"未合并不删"保护）
    git -C "$REPO" worktree remove "$path"
    if git -C "$REPO" branch -d "$branch" 2>/dev/null; then
      echo "✅ finish 完成：worktree 与分支 $branch 均已删除"
    else
      echo "✅ finish 完成：worktree 已删除；分支 $branch 未能自动删除" >&2
      echo "   （可能仍被其他 worktree 检出 — 用 wt.sh list 定位，禁止 -D 强删）" >&2
    fi
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
