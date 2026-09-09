---
name: openvela-git-commit
description: 整理 OpenVela 项目工作区并提交可审阅的 Git 变更。用于源码、配置和 docs 变更的分组、核查及中文提交说明；提交标题使用 fix、docs、feat 等英文前缀。
metadata:
  short-description: OpenVela Git 整理与中文提交
---

# OpenVela Git 提交流程

用于整理工作区、划分独立变更并创建可审阅的 Git 提交。适用于本项目及其可能存在的嵌套 Git 仓库。

提交标题采用 Conventional Commits 的英文前缀，后接中文描述，例如：

```text
fix: 修正 CSI RAW 缓冲区同步
docs: 补充 SC2336 摄像头接入指南
feat: 增加视频节点注册
```

不要使用“修复：”或“文档：”作为提交标题前缀。代码注释、标题正文和提交正文使用与项目现有风格一致的中文或英文。

## 开始前确认仓库边界

1. 在预期目录和其上级目录分别执行 `git rev-parse --show-toplevel`、`git status --short`，确认当前操作的是哪一个仓库。
2. 若发现嵌套仓库，分别审查其状态；只在用户指定的仓库内暂存和提交。
3. 查看当前分支、最近提交和变更摘要：

   ```bash
   git branch --show-current
   git log -5 --oneline
   git status --short
   git diff --stat
   git diff --cached --stat
   ```

4. 保留来源不明的未跟踪文件、用户本地配置、构建产物和其他任务的改动。除非用户明确要求，不删除、重置、清理或暂存它们。

## 整理和审查变更

按可以独立阅读、回退和验证的目的拆分变更。通常将源码或配置修复、诊断输出调整和 docs 分为不同提交；仅在它们无法独立工作时合并。

对每一组变更：

1. 先检查未暂存差异，确认不存在秘密信息、调试残留、无关格式化或生成物：

   ```bash
   git diff -- <文件或目录>
   ```

2. 只暂存该组文件，优先显式路径：

   ```bash
   git add -- <文件或目录>
   ```

3. 审查将要提交的确切内容：

   ```bash
   git diff --cached --check
   git diff --cached -- <文件或目录>
   git diff --cached --stat
   ```

4. 发现某文件只应提交部分内容时，使用 `git add -p -- <文件>`，再次审查暂存区。不要为追求拆分而将无法工作的源码改动拆开。

## 编写提交说明

标题格式为：

```text
<type>: <中文动宾短句>
```

常用 `type`：

- `fix`：修正已有行为、缺陷或诊断错误。
- `docs`：只调整文档、开发日志或指南。
- `feat`：增加用户可见能力或完整功能。
- `refactor`：不改变预期行为的结构整理。
- `chore`：维护性改动，且不属于以上类别。

提交正文使用中文分点说明，至少交代：

- 修改了哪些模块、文件或行为；
- 解决的问题、触发条件或设计目的；
- 已执行的检查、由谁执行的构建或硬件验证，以及尚未验证的项目。

示例：

```text
fix: 打通 ESP32-P4X SC2336 CSI RAW 接收

- 初始化 ISP RAW bypass，并配置 CSI Bridge 的正确行宽单位。
- 在启动 Bridge 前使能 GDMA，避免首帧丢失。
- 修正 M2C 缓冲区同步标志和对齐约束。
- 已执行 git diff --check；固件编译与板端验证由用户执行。
```

用临时文件保存多行提交正文，再传给 `git commit --file`，避免 shell 转义改变换行或字符：

```bash
git commit --file /tmp/openvela-commit-message.txt
```

仅当用户已明确要求提交时执行 `git commit`。不要默认执行 `git push`、创建标签、合并分支、变基或修改远端。

## 提交后核对

每次提交后执行：

```bash
git show --stat --oneline HEAD
git status --short
```

报告提交号、标题、涵盖内容，以及剩余改动的状态和归属。若用户负责构建或板端验证，明确写为“未由当前会话执行”，不能描述为已通过。

## 交付前检查

- 每个提交只包含一个清晰目的。
- 提交标题以 `fix:`、`docs:`、`feat:`、`refactor:` 或 `chore:` 开始；不用中文类别作前缀。
- 每个暂存集都通过 `git diff --cached --check`。
- 不存在未经确认的未跟踪文件或其他任务改动被混入提交。
- 未在用户未要求时推送或改写历史。
