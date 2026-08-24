# ESP32-P4 构建与 Kconfig 排障记录

本文记录 2026-08-19 为 `esp32p4-function-ev-board` 构建最小 `nsh`
配置时遇到的配置生成和编译问题。2026-08-21 已在实板以 `usbconsole` 配置
验证最小 `nsh>`；MIPI-DSI Probe 的 M1 命令写验证与专项问题另见
[DSI Host Probe 排障记录](2026-08-21-DSI-Host-Probe排障记录.md)。

## 适用范围与标准入口

板级配置的标准入口是：

```bash
./build.sh vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/nsh -j2
```

`vendor/espressif/boards/esp32p4/esp32p4-function-ev-board` 是指向竞赛工程
`board/esp32p4/esp32p4-function-ev-board` 的软链接。前者用于保持与工作区标准
板级目录一致；实际 custom board 与 custom chip 源码仍在竞赛工程中。

本板的 HAL 兼容层接入 NuttX Make 构建链路，当前只使用 Make 路径；不要改用
`--cmake` 作为本问题的绕过手段。

## 现象

直接构建时，`nuttx/.config` 没有补齐 Kconfig 默认值，随后 C 编译阶段出现：

```text
unknown type name 'g_interrupt_context'
CONFIG_NCPUS undeclared
CONFIG_STREAM_OUT_BUFFER_SIZE undeclared
CONFIG_STREAM_HEXDUMP_BUFFER_SIZE undeclared
CONFIG_STREAM_BASE64_BUFFER_SIZE undeclared
```

这些宏分别影响 per-CPU、调度器和 stream 结构定义。它们不是 ESP32-P4
defconfig 中应逐项手工维护的选项。

## 根因链路

问题的顺序如下：

1. `configure.sh -e` 会将当前板级 `defconfig` 与 `nuttx/defconfig` 的备份
   逐字比较。两者相同便输出 `No configuration change.` 并提前结束；这个判断
   不代表已有的 `nuttx/.config` 已完成 Kconfig 展开。
2. 当时的 `nuttx/.config` 仅 77 行，实质上是板级 defconfig 的副本，未包含
   `CONFIG_NCPUS`、per-CPU 与 stream buffer 的默认配置。随后 Make 使用这份
   未展开配置进行编译，产生了前述错误。
3. 初次直接执行 `make -C nuttx olddefconfig` 时，交互 shell 没有加载
   `build/envsetup.sh`。`nuttx/tools/Unix.mk` 因找不到 `menuconfig`，退回到系统
   `/usr/bin/kconfig-conf`，而不是 openvela 自带的 Python `kconfiglib`。
4. 系统 kconfig-frontends 不支持工作区实际使用的 `osource` 扩展，因而在
   `external/zblue`、LVGL、multimedia、quickapp 等目录报告大量语法、跨文件
   `endif`/`endmenu` 错误。`tricoreht/Kconfig` 和 `ril/Kconfig` 中的成对
   `if`/`endif` 也被误报为跨文件，故这些日志不能作为逐项修改基础树源码的依据。
5. 在较早的排障中还发现两项独立的基础树卫生问题：
   - `nuttx/arch/tricore/Kconfig` 第 116、122 行将帮助标记写成 `--help--`；
     NuttX 语法应为 `---help---`。这是独立的语法规范性修正；在错误前端下
     出现的跨文件 `endif`/`endmenu` 报错不应据此逐项归因。
   - 自动生成的 `apps/examples/Kconfig` 仍 `source` 不存在的
     `apps/examples/audio_record/Kconfig`，属于过期索引，不是本板需要启用
     `audio_record`。
6. 因此前没有一次使用正确前端成功完成 `olddefconfig`，`.config` 没有生成
   `CONFIG_NCPUS`、stream buffer 默认值及相应的 per-CPU 配置，最终表现为前述
   C 编译错误。

因此，先补写 `CONFIG_NCPUS` 等宏只能掩盖失败，后续仍会出现更多缺失的默认值。

## 已执行的处理

- 已将 `nuttx/arch/tricore/Kconfig` 两处帮助标记从 `--help--` 修正为
  `---help---`。这属于外层 NuttX 基础树的独立修复，不应混入竞赛工程的 P4
  板级提交。
- 已尝试以 `make -C apps -B preconfig` 全量再生 Apps Kconfig；该命令目前因
  `apps/import/scripts/Make.defs` 缺失而停止。
- 随后在 `apps/examples` 目录使用已有的 `../tools/mkkconfig.sh -m Examples`
  再生 examples 索引，生成结果已移除失效的 `audio_record/Kconfig` 引用。
  `apps/examples/Kconfig` 是生成且忽略的工作区文件，不应作为手写功能修改提交。
- 已使用正确的 openvela 环境执行：

  ```bash
  source build/envsetup.sh
  make -C nuttx olddefconfig
  ```

  此时 `menuconfig`、`olddefconfig` 和 `kconfiglib` 均来自
  `prebuilts/tools/python`；命令以 `Loaded configuration '.config'` 与
  `Configuration saved to '.config'` 成功结束。
- 成功后 `.config` 从 77 行展开为 2409 行，已实际生成：

  ```ini
  CONFIG_NCPUS=1
  CONFIG_PERCPU_ARRAY=y
  CONFIG_SMP_NCPUS=1
  CONFIG_STREAM_OUT_BUFFER_SIZE=64
  CONFIG_STREAM_HEXDUMP_BUFFER_SIZE=128
  CONFIG_STREAM_BASE64_BUFFER_SIZE=128
  ```

  `nuttx/include/nuttx/config.h` 会在下一次正式 Make 编译开始时重新生成；
  `olddefconfig` 的 `clean_context` 阶段不存在该文件是正常现象。

## 建议的复测顺序

在工作区根目录执行。必须先加载 openvela 环境，再进入下一步：

```bash
source build/envsetup.sh

command -v menuconfig
command -v olddefconfig
python3 -c 'import kconfiglib; print(kconfiglib.__file__)'

make -C nuttx olddefconfig

grep -E '^CONFIG_(UP|PERCPU_ARRAY|NCPUS|SMP_NCPUS|STREAM_OUT_BUFFER_SIZE|STREAM_HEXDUMP_BUFFER_SIZE|STREAM_BASE64_BUFFER_SIZE)=' nuttx/.config

./build.sh vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/nsh -j2
```

判断原则：

- `menuconfig`、`olddefconfig` 应来自 `prebuilts/tools/python/bin`，且
  `kconfiglib` 应来自 `prebuilts/tools/python/dist-packages`。若它们落到
  `/usr/bin/kconfig-conf`，先重新 `source build/envsetup.sh`，不要据该前端的
  `osource`、跨文件 `endif` 等报错批量修改源码。
- `olddefconfig` 必须以成功状态结束；若使用正确前端后仍失败，只处理其报告的
  第一处 Kconfig 解析或缺失文件问题，不要立即继续编译。
- `grep` 用于确认关键配置已由 Kconfig 写入 `.config`；具体取值由依赖关系和
  默认值决定，不应手工猜测或修改。
- 只有配置生成成功后，才判断后续 C/汇编错误是否属于 P4 HAL、custom chip 或
  custom board 的真实问题。
- 配置阶段显示 `No configuration change` 本身不表示失败；前提是前一步
  `olddefconfig` 已经成功完成。

## 当前验收状态

| 项目 | 状态 |
| --- | --- |
| Kconfig 前端 | 已确认使用 openvela Python `kconfiglib`；不使用系统 `/usr/bin/kconfig-conf` 直接排障 |
| Tricore 两处 Kconfig 语法 | 已修正 |
| examples 过期 `audio_record` 索引 | 已再生并移除 |
| `make -C nuttx olddefconfig` | 已通过；关键默认配置已生成 |
| ESP32-P4 最小 `nsh` 完整构建 | 已通过 `usbconsole` 配置 |
| 烧录与串口 `nsh>` | 已在实板验证；DSI 命令、DPI 视频色条及标准 `/dev/fb0` framebuffer 均已通过 |

相关文档：[P4 最小 NSH 操作与测试](../../硬件适配/esp32p4-nsh-operation-and-test.md)、
[历史 P4 移植开发记录](../dev.md)。
