# prebuilts（本地工具链，不入库）

本目录用于存放 ESP32-P4 构建所需的乐鑫 RISC-V 工具链。**内容全部被
`.gitignore` 忽略**，只有本 README 会被提交。

## 为什么需要

P4 的 PIE 指令属于私有扩展 `xespv`，OpenVela 自带的 xPack
`riscv-none-elf` 无法编码。启用 `CONFIG_ESP32P4_ESPRESSIF_TOOLCHAIN` 的
配置（ESP-NN 相关）必须用乐鑫工具链，与 ESP32-S3 用 `xtensa-esp32s3-elf`
同理。

## 期望布局

```text
prebuilts/gcc/linux-x86_64/riscv32-esp-elf/
├── bin/                      # riscv32-esp-elf-gcc / ld / objdump ...
├── lib/gcc/riscv32-esp-elf/14.2.0/
│   ├── rv32imac_zicsr_zifencei/ilp32/libgcc.a
│   └── include/
├── libexec/gcc/riscv32-esp-elf/14.2.0/
│   └── cc1, cc1plus, collect2
├── riscv32-esp-elf/lib/rv32imac_zicsr_zifencei/ilp32/
│   └── libc.a, libm.a, libsupc++.a, libstdc++.a
└── share/
```

裁剪后约 **430 MB**，与预置的 `xtensa-esp32s3-elf`（417 MB）相当。

## 获取方式

```sh
scripts/setup_toolchain.sh          # 下载、校验、解压、裁剪
scripts/setup_toolchain.sh --check  # 只校验现有安装
```

## 裁剪说明

相对官方发行包（解压 2.15 GB）删除了以下内容，均有构建验证：

| 删除项 | 体积 | 依据 |
| --- | --- | --- |
| `picolibc/` | 760 MB | NuttX 用 `-nostdlib` + 自带 libc，不链接 picolibc |
| 4 套无关 multilib | 733 MB | 平台固定 `-march=rv32imac_zicsr_zifencei -mabi=ilp32` |
| `no-rtti` 变体 | 83 MB | NuttX 不使用 `-fno-rtti` 构建 C++ |
| LTO 组件（`lto1` / `lto-dump` / `lto-wrapper`） | 102 MB | 构建不使用 LTO |

保留 `rv32imac_zicsr_zifencei/ilp32` 一套，以及 `cc1` / `cc1plus` /
`collect2` / binutils。

**风险提示**：如果将来引入 LTO 构建或改用其他 `-march`，必须重新安装
完整工具链；裁剪版不包含相应组件。
