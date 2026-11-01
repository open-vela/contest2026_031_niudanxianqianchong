---
name: openvela-worktree-workflow
description: This skill should be used whenever an agent performs any write operation in the contest2026_031_niudanxianqianchong checkout or its nuttx tree — code changes, branch creation, commits, experiments, dry-runs, rebases, or cherry-picks. It enforces the workspace discipline in RULE.md (main checkout is read/verify/fast-forward only; all writes go through a dedicated git worktree), covers worktree creation and naming, file ownership between parallel agents, sync-back to the main branch, protection of uncommitted changes, and the nuttx patch replayability rule.
---

# Openvela Worktree Workflow

## 背景与定位

本 skill 是 `RULE.md`（仓库根目录，多 Agent 工作区纪律）的执行手册。两次事故
（提交落错分支、`reset --hard` 蒸发 11 个未提交文件）的根因都是**在主检出直接做
写操作**。本 skill 把安全路径变成默认路径。

策略层看 `RULE.md`；本文件只讲怎么做。冲突时以 `RULE.md` 为准。

## 会话自检（每次动手前必做）

运行仓库内脚本：

```bash
.claude/skills/openvela-worktree-workflow/scripts/wt.sh check
```

输出当前分支、未提交项数量和安全结论。判定规则：

- 分支不是 `contest2026`，或未提交项 > 0 → **先向用户报告，未获确认不动手**；
- 分支为 `contest2026` 且干净 → 主检出仅可做读操作、构建验证、fast-forward 推进；
  任何写操作进入下一节。

## 创建 worktree（写操作的标准入口）

```bash
.claude/skills/openvela-worktree-workflow/scripts/wt.sh new <topic> [agent名]
```

- 生成 `../worktrees/<topic>-<agent>`，分支 `feat/<topic>-<agent>`，基于 `contest2026`；
- `<topic>` 用功能短名（如 `voice-config`、`camera-dialog`），agent 名用于避免并行撞车；
- 路径已存在 = 另一个 agent 正在用同一主题 → 先 `wt.sh list` 并协商，不要强占；
- 构建直接在 worktree 内执行 `scripts/build_p4x.sh <config> -j8`（脚本支持
  worktree 布局，产物隔离在各自 worktree）。

## 文件所有权

- 一个文件一个 owner：只修改自己任务范围内的文件；
- 发现需要改动其他 agent 持有的文件：停下来协商；后改方先 rebase 对方已提交成果；
- 派活时（多人协作），在任务描述里写明文件范围，接收方发现重叠立即上报。

## 收尾与同步回主线

1. worktree 内：确认改动完整 → 按 `openvela-git-commit` 规范提交；
   **每个提交独立可构建**（bisect/cherry-pick 安全）；跨提交功能用 fixup 并入首个提交；
2. 收工时若仍有未提交改动：`git stash`（可救）或 WIP commit——**不得裸奔过夜**；
3. 主检出 fast-forward 合并该分支（合并前 `wt.sh check` 确认主检出干净；
   主检出只做 fast-forward，不做其他合并动作）；
4. **标准收尾（合并后必做）**：

   ```bash
   .claude/skills/openvela-worktree-workflow/scripts/wt.sh finish <worktree路径>
   ```

   finish 依次校验：滞留登记已 prune → worktree 干净 → 分支已并入
   `contest2026` → 主检出干净，全部通过才移除 worktree 并用 `branch -d`
   删除分支；任何一步不过即拒绝并说明原因，**禁止绕过**；
5. `wt.sh done` 是特殊出口：仅当分支**有意保留**（如等待评审）时使用，
   只删 worktree、分支保留；
6. 分支删除报"仍被检出"时：先 `git worktree prune` 清滞留登记再重试；
   **禁止 `branch -D` 强删**（曾发生 reset 丢 11 个文件的事故，强删即重演）。
## 禁止清单（在主检出）

以下操作一律移到 worktree 执行：实验分支提交、dry-run、rebase、cherry-pick、
fixup、`reset`（任何形式）、`checkout -- <file>`、`clean`。

主检出意外出现未提交改动：**先抢救（stash）再讨论**，禁止直接清理。
已发生丢失时，恢复只允许 `git reflog` 定位 + cherry-pick，禁止二次 reset。

## nuttx 工作树纪律

`nuttx/` 树的任何改动必须能重放进 `patches/nuttx/` 系列：

- 驱动改动按主题拆分编号，改源码的提交同步更新对应 patch 文件；
- patch 系列变更后，在干净 nuttx 检出上按序 `git apply --check` 验证。

## 事故处置

- 提交落错分支：`git cherry-pick` 归位到正确分支，不在原分支删除重做；
- 怀疑丢失未提交改动：依次查 `git stash list` → `git fsck --dangling` →
  `git reflog`，全部无果再报告用户；禁止用二次 reset/checkout "修复"。
