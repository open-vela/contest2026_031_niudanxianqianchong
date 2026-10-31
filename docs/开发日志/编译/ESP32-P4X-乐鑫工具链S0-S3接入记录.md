# P4 乐鑫 RISC-V 工具链接入记录（S0–S3）

日期：2026-10-31

本文记录 [乐鑫 RISC-V 工具链整平台切换方案](../../开发计划/基础与规划/ESP32-P4X-乐鑫RISC-V工具链整平台切换实施方案.md)
的 S0–S3 阶段。**只完成环境、补丁和构建入口；业务代码未改动，整机构建与
板端验证由接手方执行。**

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
