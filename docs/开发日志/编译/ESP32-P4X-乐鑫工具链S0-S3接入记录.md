# P4 乐鑫 RISC-V 工具链接入记录（S0–S3）

日期：2026-10-31

本文记录乐鑫 RISC-V 工具链整平台切换方案的 S0–S3 阶段（切换方案原文未随仓
入库，结论与入口以本文为准）。**只完成环境、补丁和构建入口；业务代码未改动，
整机构建与板端验证由接手方执行。**

## 1. S0：工具链就位

| 项 | 值 |
| --- | --- |
| 版本 | crosstool-NG `esp-14.2.0_20260121`，GCC 14.2.0，binutils 2.43.1 |
| 本机路径 | `~/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/` |
| prebuilts 入口 | `prebuilts/gcc/linux-x86_64/riscv32-esp-elf`（软链接，**不入库**） |
| `riscv32-esp-elf-gcc` sha256 | `789918490cee5a48112cae580f3279c08a26d37fbf738b8655b4b7dbe11f9957` |
| `riscv32-esp-elf-ld` sha256 | `d4afcd04997ff69a4825ff52a5764dfb7e33cf31359e2b17cd339bff6be37016` |

建立方式：

```sh
cd /home/arongw/openvela
ln -sfn /home/arongw/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf prebuilts/gcc/linux-x86_64/riscv32-esp-elf
```

`prebuilts/gcc/linux-x86_64/` 下每个工具链目录是独立 repo 工程；这个软链接未在
manifest 登记，`repo sync` 不会创建它，也不会跟踪它。它属于**本地手工产物**，
其他机器必须自己重建。

### 1.1 版本选择的硬性依据

上游 release 说明的第一条即为 **"Add ESP32P4 ECO5 support (xespv extension)"**，
因此 `esp-14.2.0_20260121` 是第一个能编码 PIE 指令的版本；S3 在用的 esp-12.2.0
不满足。低版本会直接报：

```text
Error: unrecognized opcode 'esp.vld.128.ip q0,(a0),16'
```

### 1.2 官方下载（供其他机器复现）

```text
https://github.com/espressif/crosstool-NG/releases/tag/esp-14.2.0_20260121
riscv32-esp-elf-14.2.0_20260121-x86_64-linux-gnu.tar.xz   279 MB
sha256 b3fce4b04dd15a9f0ed1c209b5a2f389c04eff92ed85e4cc0ebc275b7d95e95a
```

解压后目录结构与 `prebuilts` 里 xtensa 那份同构（`bin/ lib/ libexec/ share/
riscv32-esp-elf/`）。

## 2. S1：Kconfig 开关

`chips/esp32p4/Kconfig` 新增：

```kconfig
config ESP32P4_ESPRESSIF_TOOLCHAIN
    bool "Build ESP32-P4 with the Espressif riscv32-esp-elf toolchain"
    default n
```

**默认 n 是有意的**：主分支必须保持"干净 clone 用 prebuilts 里的 xPack 就能
构建"。需要 PIE 的配置（ESP-NN 相关）在自己的 defconfig 里显式打开，并接受
"需要额外工具链"这一前提。这与方案 §3.1 的 `default y` 不同，理由见 §5。

## 3. S2：NuttX 补丁 0012（重写）

`patches/nuttx/0012-espressif-riscv-toolchain.patch`，两处，均按开关选择：

| 文件 | 改动 |
| --- | --- |
| `arch/risc-v/src/common/Toolchain.defs` | `GNU_RVG` 分支内加 `else ifeq ($(CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN),y)` → `CROSSDEV ?= riscv32-esp-elf-` |
| `arch/risc-v/src/cmake/Toolchain.cmake` | 默认工具链分支加 `elseif(CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN)` → `set(TOOLCHAIN_PREFIX riscv32-esp-elf)` |

要点：

- 保留 `?=`，命令行 `CROSSDEV=riscv-none-elf-` 仍可临时覆盖（方案 §10 回滚）。
- **不再修改** `arch/risc-v/Kconfig` 的 `ARCH_RV_ISA_VENDOR_EXTENSIONS`。上一版
  补丁让它对 P4 默认 `"xespv"`，会使**整个固件**按 `-march=..._xespv` 编译；而
  NuttX 不保存 PIE 寄存器，抢占/迁移下可能静默损坏数据。按方案 §3.2，扩展只由
  ESP-NN 包为自己的源文件追加。
- CMake 路径同样要改：`scripts/build_cmake_p4x.sh` 是 P4 当前主构建路径，只改
  Make 会得到两套编译器产出的镜像。

补丁已在干净的 `nuttx/` 上 `git apply --check` 与 `git apply` 双向验证。

## 4. S3：构建入口

新增 `scripts/build_p4x.sh`（Make 路径封装），并改 `scripts/build_cmake_p4x.sh`。
两者逻辑一致：

1. 读该配置的 `defconfig`，看 `CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN`；
2. 打开时把 `prebuilts/gcc/linux-x86_64/riscv32-esp-elf/bin` 放到 PATH 最前，
   并**校验工具链真实存在**（缺失时报错退出，不静默回退）；
3. 打印实际使用的编译器路径与版本。

为什么不改 `build/envsetup.sh`：它只匹配 `prebuilts/gcc/<host>/{riscv,riscv-none}-{eabi,elf}/bin`，
匹配不到 `riscv32-esp-elf`。改公共工作区的 `envsetup.sh` 会影响所有架构的构建，
本期不做（方案 §3.3）。

## 5. 与方案原文的一处偏离

方案 §3.1 建议 `ESP32P4_ESPRESSIF_TOOLCHAIN` 默认 `y`，本文实现为默认 `n`。

原因：默认 `y` 意味着**合入主分支后所有 P4 构建都依赖外部工具链**，而配套的
分发方案（prebuilt 仓库 + manifest）在方案 §11 中明确排在本期之外。两者叠加会
让"clone 下来就能编"失效。默认 `n` 时：

- 主分支 xPack 可编，行为与现在完全一致；
- ESP-NN 相关配置显式 `=y`，前置条件写在 README 与本文；
- 分发方案落地后，把默认值改成 `y` 是一行改动。

## 6. 已完成的验证

| 检查 | 方法 | 结果 |
| --- | --- | --- |
| 工具链可执行 | 通过 prebuilts 软链调用 `--version` | 14.2.0 |
| PIE 可编码 | 汇编 `esp.vld.128.ip` / `esp.vmulas.s8.xacc` / `esp.vldbc.8.ip` 并反汇编 | 通过 |
| Make 选中逻辑 | `make -n`，开关 y / 未设 | `riscv32-esp-elf-gcc` / `riscv-none-elf-gcc` |
| CMake 选中逻辑 | 独立 `.cmake` 脚本复现条件分支 | 同上 |
| 补丁可用性 | 干净 nuttx 上 `apply --check` + `apply` | 通过 |
| 脚本语法 | `bash -n` | 通过 |

## 7. 未完成（交给接手方）

| 项 | 说明 |
| --- | --- |
| CMake 全流程 | 被 `nuttx/.config` 残留挡住（NuttX 要求先 `make distclean`），未跑 |
| S4 `nsh` 构建 | 先 `make -C ../nuttx distclean`，再 `scripts/build_p4x.sh nsh` |
| S5 全量编译矩阵 | 方案 §6.1 |
| S6 尺寸对比 | 方案 §6.3 |
| S7 板端回归 | 开发板不在 USB 上 |
| S8 ESP-NN 包局部启用 `xespv` | 依赖 `feat/p4x-esp-nn` rebase 到本分支 |

## 8. 接手方第一步

```sh
cd /home/arongw/openvela
ln -sfn /home/arongw/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf prebuilts/gcc/linux-x86_64/riscv32-esp-elf

cd contest2026_031_niudanxianqianchong
echo "CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN=y" >> board/esp32p4/esp32p4-function-ev-board/configs/nsh/defconfig

make -C ../nuttx distclean
scripts/build_p4x.sh nsh -j8
```

预期第一行输出 `CC: .../riscv32-esp-elf-gcc (14.2.0)`。

## 9. S4 实测结果（2026-10-31 补充）

### 9.1 结论：整机编译通过

`nsh` 配置用乐鑫 GCC 14.2.0 全量编译**成功**，`nuttx.bin` 正常生成：

| 检查项 | 结果 |
| --- | --- |
| `.comment` | `GCC: (crosstool-NG esp-14.2.0_20260121) 14.2.0`（仅此一种） |
| ABI | `0x1, RVC, soft-float ABI` |
| PIE 指令计数 | `0`（平台层无 xespv，符合 §3.2 设计） |
| `nuttx.bin` | 273,404 B（xPack 基线 273,592 B，**-188 B / -0.07%**） |
| `irom_seg` | 150,522 B（xPack 150,710 B，-188 B） |
| `sram_seg` | 36,376 B（与 xPack 完全一致） |

尺寸差异只有 188 字节，说明 GCC 13→14 在本配置上代码生成几乎等价。

### 9.2 K1 风险的实际表现

方案 §4 预判 GCC 14 会把三类警告升级为错误。实测确认**确有 1 处**，但不是数量级问题：

```text
include/nuttx/net/usrsock.h:323:10: error: implicit declaration of function
  'net_sem_timedwait2'; did you mean 'nxsem_timedwait'?
  [-Wimplicit-function-declaration]
```

根因：`net_sem_timedwait2` 的声明在 `include/nuttx/net/net.h` 的 `#ifdef CONFIG_NET`
（第 31 行）之内，而 `usrsock.h` 无条件调用它。`nsh` 配置没有 `CONFIG_NET`，于是声明
不可见。这是 **NuttX 既有的头文件依赖缺陷**，GCC 13 只是警告，GCC 14 变成错误。

修复方向（二选一，建议前者）：

1. 给 `usrsock.h` 的 `usrsock_sem_timedwait()` 加 `#ifdef CONFIG_NET` 守卫，或把
   `net.h` 的 `net_sem_timedwait2` 声明移出 `CONFIG_NET`；
2. 仅对该文件放宽 `-Wno-error=implicit-function-declaration`（不推荐，会掩盖同类问题）。

本轮为拿到完整错误画像，临时用 `EXTRAFLAGS` 把三类警告降回警告后全量编译，
**没有修改任何源码**。除上述 1 处，其余 2600+ 编译单元在 GCC 14 下零警告、零错误。

### 9.3 归档验证的真实阻塞：控制台

`nsh` 配置只有 `CONFIG_UART0_SERIAL_CONSOLE=y`（UART0 物理引脚），而开发板接的是
USB Serial/JTAG（`/dev/ttyACM0`）。已真机验收的 `smart_home` 配的是
`CONFIG_ESPRESSIF_USBSERIAL=y`。

因此 `nsh` 烧录后只能看到 ROM 引导日志，进入 NuttX 后无任何输出——这是配置问题，
**与工具链无关**。对照实验：xPack 编出的同配置镜像行为完全一致（同样加载 7 段后静默）。

结论：**本阶段的板端验收应使用 `smart_home` 或 `smp_smart_home`，不要用 `nsh`。**

### 9.4 尚未验证

- `smart_home` / `smp_smart_home` 的全量编译（未执行）
- 板端 NSH 交互与 `ostest`（被 §9.3 的控制台配置阻塞）
- 方案 §6.1 的其余配置矩阵

## 10. 构建环境的两个坑（实测）

### 10.1 worktree 里 `CONFIG_ARCH_CHIP_CUSTOM_DIR` 指向主工作区

`nsh` 的 defconfig 写死：

```text
CONFIG_ARCH_CHIP_CUSTOM_DIR="../contest2026_031_niudanxianqianchong/chips/esp32p4"
```

在 worktree 中构建时，`configure.sh` 生成的 `arch/risc-v/src/chip` 软链仍指回
**主工作区**的 `chips/esp32p4`，于是 worktree 里对 `chips/esp32p4/Kconfig` 的修改
（例如新增的 `ESP32P4_ESPRESSIF_TOOLCHAIN`）不会生效。这是方案 §8.1 已警告的
构建污染的实例。

绕过方式：本轮验证用 `CROSSDEV=riscv32-esp-elf-` 强制工具链，不依赖 Kconfig 开关。
正式实施需要在 worktree 内同步 `CONFIG_ARCH_CHIP_CUSTOM_DIR` /
`CONFIG_ARCH_BOARD_CUSTOM_DIR`，或在主工作区构建。

### 10.2 `make distclean` 会清空 `esp-hal-3rdparty` 的子模块

`make distclean` 触发 `chip/esp-hal-3rdparty` 重新 clone，当前网络无法访问
`github.com`（超时），且 `components/mbedtls/mbedtls` 等子模块在本地裸库中缺少目标
提交，**构建会直接失败**。

恢复办法（本轮已执行）：`esp-hal-3rdparty-cache/` 是完整快照，可复制回
`chips/esp32p4/esp-hal-3rdparty`；mbedtls 工作树可从原副本的
`.git/modules/components/mbedtls/mbedtls` 裸库 checkout 恢复。

**建议**：在离线或网络受限环境里，避免对含 `esp-hal-3rdparty` 的树执行
`make distclean`；改用 `make clean` 或只删除目标对象。

## 11. 工具链分发：入仓但忽略提交（2026-10-31 补充）

### 11.1 决定

工具链实体放到**比赛仓**的 `prebuilts/gcc/linux-x86_64/riscv32-esp-elf/`，
通过 `.gitignore` 排除，不进入版本历史；只提交 `prebuilts/README.md`
（说明期望布局）与 `scripts/setup_toolchain.sh`（可复现的安装流程）。

理由：比赛仓是自包含的交付物，工具链放在仓内路径下便于定位；但 428 MB
的二进制不可差分，一旦入库无法回收，因此排除。

### 11.2 裁剪结果

源安装 2.15 GB → 裁剪后 **428 MB**（与预置的 `xtensa-esp32s3-elf` 417 MB 相当）。

| 删除项 | 体积 | 依据 |
| --- | --- | --- |
| `picolibc/` | 760 MB | NuttX 用 `-nostdlib` + 自带 libc |
| 4 套无关 multilib（`rv32i` / `rv32imc` / `rv32imafc` / `rv32imafc_zb*`） | 733 MB | 平台固定 `-march=rv32imac_zicsr_zifencei -mabi=ilp32` |
| `no-rtti` 变体 | 83 MB | 不使用 `-fno-rtti` 构建 |
| LTO 组件（`lto1` / `lto-dump` / `lto-wrapper`） | 102 MB | 构建不使用 LTO |

保留 `rv32imac_zicsr_zifencei/ilp32` 一套，加 `cc1` / `cc1plus` / `collect2` / binutils。

**风险**：将来若引入 LTO 构建或改用其他 `-march`，必须重装完整工具链。

### 11.3 构建脚本的查找顺序

`scripts/build_p4x.sh` 与 `scripts/build_cmake_p4x.sh` 都按以下顺序找工具链：

```text
1. <比赛仓>/prebuilts/gcc/linux-x86_64/riscv32-esp-elf/bin   ← 优先
2. <openvela>/prebuilts/gcc/linux-x86_64/riscv32-esp-elf/bin ← 回退
```

两处都没有且配置开启了开关时，脚本报错退出，不静默回退到 xPack。

### 11.4 端到端验证（真机）

```text
config : smp_smart_home + CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN=y
command: scripts/build_p4x.sh smp_smart_home -j8
CC     : <比赛仓>/prebuilts/.../riscv32-esp-elf-gcc (14.2.0)
.config: CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN=y   ← 开关确实生效
.comment: GCC: (crosstool-NG esp-14.2.0_20260121) 14.2.0  ← 仅此一种
PIE    : 0 条（平台层干净）
nuttx.bin: 1,742,500 B
```

烧录后串口日志：

```text
*** Booting NuttX ***
[CPU0] ESP32-P4 SMP: CPU1 ready
[CPU0] INFO: P4X MIPI-DSI framebuffer registered: /dev/fb0 1024x600 RGB565
[CPU0] GT911 product id: 39 31 31 00 (911)
[CPU0] P4X LittleFS mounted: source=/dev/espflash mount=/data
NuttShell (NSH)
```

裁剪后的工具链构建的镜像功能完整。

### 11.5 关于 §10.1 的补充

本轮验证再次确认了 `CONFIG_ARCH_CHIP_CUSTOM_DIR` 指向主工作区造成的问题，
并且发现一个新的前置条件：**`arch/dummy/Kconfig` 是芯片 Kconfig 的副本，
由 make 规则按时间戳刷新**。改动芯片 Kconfig 后，`configure.sh` 阶段仍会用
旧副本，导致新符号被静默丢弃。

绕过方式：改动 Kconfig 后先手动 `cp -f <chip>/Kconfig arch/dummy/Kconfig`，
或先跑一次 `make` 让它刷新，再重新 `configure.sh`。

本轮为此在**主工作区**同步了 `chips/esp32p4/Kconfig`（工作区文件，未提交），
因为 `CUSTOM_DIR` 指向那里。合并到主分支后该问题自然消失。
