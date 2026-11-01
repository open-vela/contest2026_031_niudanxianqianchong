# 多 Agent 工作区纪律（RULE.md）

> 版本：v1.0（2026-11-01）
> 适用范围：本仓主检出（本目录）+ 外层 openvela 工作树（`nuttx/`、`apps/`、`chips/` 等）
> 生效对象：所有 AI agent 会话（CodeBuddy / Claude / Codex）与人类操作者
> 配套流程：`.claude/skills/openvela-worktree-workflow/SKILL.md`

## 背景

2026-10-31 连续发生两次 git 事故：

1. 主检出在未提交改动裸奔的情况下切换分支，6 个提交落错分支（`feat/p4x-gt911-irq`）；
2. 干跑 cherry-pick 在主检出执行，途中 `reset --hard` 物理蒸发 11 个未提交文件
   （首页 v9 233 行、摄像头弹窗接线等，无任何 git 对象可恢复）。

本规则即为杜绝复发。**规则冲突时，本文件优先于任何 agent 的效率偏好。**

## 一、主检出的角色边界（硬性）

主检出（本目录）只允许三类操作：

1. **读**：查看代码、日志、diff、状态；
2. **验证**：构建、烧录产物检查、运行冒烟；
3. **推进**：对 `contest2026` 分支的 fast-forward 式推进（合并已验证的 worktree 分支）。

主检出**禁止**：实验分支提交、dry-run、rebase、cherry-pick、reset（任何形式）、
`clean`、`checkout -- <file>`、以及任何会修改工作区文件的操作。

## 二、分支纪律

1. 主检出常驻 `contest2026` 分支。切分支一律去 worktree 做。
2. **会话开始自检**（任何 agent 动手前必做并汇报）：

   ```bash
   git branch --show-current && git status --short | wc -l
   ```

   不在 `contest2026`，或存在未提交改动 → 先报告，未获用户确认不动手。
3. 提交前再看一眼 `git branch --show-current`——落错分支的提交要用
   `git cherry-pick` 归位，而不是切分支重做。

## 三、写操作一律 worktree

任何改代码、建分支、试验性操作，必须先：

```bash
git worktree add ../worktrees/<topic>-<agent名> -b feat/<topic>-<agent名>
```

- 命名沿用现有 `worktrees/` 惯例，**必须带主题和 agent 标识**；
- `scripts/build_p4x.sh` 已支持在 worktree 内构建；
- dry-run、fixup、rebase 等历史重写操作**只允许在 worktree 做**。

## 四、文件所有权

1. **一个文件一个 owner**：每个 agent 只修改自己 worktree 对应任务内的文件；
2. 跨 agent 需要改同一文件时，先协商，后改方必须先 rebase 对方已提交的成果；
3. 派活方（lead）在任务里写明文件范围；接收方发现范围重叠时停下来问，不要猜。

## 五、未提交改动生存法则

1. 未提交改动**不得过夜**：收工前 `git stash`（至少进对象库，可救）或
   WIP commit 到自己分支（更好）；
2. 任何 `reset --hard` / `checkout -- <file>` / `clean` 执行前，
   `git status --porcelain` 必须为空，否则立即停止并报告；
3. 主检出出现非预期未提交改动：停下、报告、等人工确认。**先抢救，再清理**。

## 六、同步回主线

1. worktree 分支完成后：在 worktree 内 rebase 到最新 `contest2026`；
2. 主检出只做 fast-forward 式合并，**合并前必须确认工作区干净**；
3. 每个提交必须独立可构建（bisect/cherry-pick 安全）；功能跨多提交时，
   用 fixup 并入首个提交，而不是留半截提交。

## 七、nuttx 工作树补丁纪律

1. `nuttx/` 工作树的任何改动，必须能重放进 `patches/nuttx/` 系列
   （驱动改动按主题拆分编号）；
2. 修改 nuttx 源码的提交，同一提交内必须同步更新对应 patch 文件；
3. patch 系列变更后，在干净 nuttx 检出上按序 `git apply --check` 验证。

## 违规处置

agent 违反本规则（如在主检出 reset）：

1. 立即停止当前操作链，报告已执行的全部命令；
2. 恢复路径只允许：`git reflog` 定位 + cherry-pick 归位，禁止二次 reset；
3. 事故写入 `docs/开发日志/` 对应日期，含根因与预防措施。
