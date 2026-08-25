# ESP32-S3-BOX-3 与 ESP32-P4X 构建补丁脚本

## ESP32-P4X：HAL 与 NuttX 延时接口兼容

P4X 的 LittleFS 启用 `CONFIG_ESPRESSIF_SPIFLASH` 后，会链接 ESP HAL 的
`spi_flash_os_func_app.c`。该组件通过 `platform/os.h` 的 `OS_TASK_DELAY()`
调用 NuttX 延时接口；固定版本的 HAL 使用了不存在的 `nxsched_usleep()`，会造成
链接错误。P4X 使用 `nxsig_usleep()`，并显式包含 `<nuttx/signal.h>`。

补丁存放在：

```text
chips/esp32p4/common/espressif/patches/
  0001-nuttx-platform-openvela-nxtask-init.patch
  0002-nuttx-platform-use-nxsig-usleep.patch
```

常规 Make/CMake 构建会在 HAL checkout/reset 后按文件名顺序自动应用这些补丁。Make
会在同一 checkout 配方中写入补丁戳，并删除 P4X HAL 生成的对象、依赖文件和
`libarch.a`/`libkarch.a`，确保含有旧 HAL 内联函数的对象不会被复用。
若已存在 HAL 工作树（例如本次链接失败后），先执行：

```bash
cd ~/openvela
contest2026_031_niudanxianqianchong/scripts/apply_p4x_hal_patches.sh
```

该脚本遵循 BOX-3 补丁的原则：修改的是可丢弃 HAL 副本，但修改内容只保存在竞赛仓的
补丁文件中；后续 `distclean` 或 HAL 重新拉取仍可复现。

## 为什么需要这些脚本

ESP32-S3-BOX-3 同时需要两套 mbedTLS：

- **ESP HAL mbedTLS**：Wi-Fi WPA 认证需要，编译在 `nuttx/arch/xtensa/src/chip/esp-hal-3rdparty/`
- **apps mbedTLS**：cAGENT HTTPS transport 需要，由 `CONFIG_CRYPTO_MBEDTLS=y` 启用，编译在 `apps/crypto/mbedtls/`

两套 mbedTLS 的结构体定义、配置宏、符号名不兼容，直接同时编译会报错。
这三个脚本通过编译器和预处理层面的修补让它们共存，不需要改 cAGENT 或 smart_home 源码。

## 脚本列表

| 脚本 | 修复内容 | 修复目标 |
|------|----------|----------|
| `fix_box3_mbedtls_header_priority.sh` | 1/3 头文件优先级 | `apps/crypto/mbedtls/Make.defs` |
| `fix_box3_mbedtls_disable_ccm.sh` | 2/3 禁用 CCM 密码算法 | ESP HAL `mbedtls_config.h` |
| `fix_box3_spinlock_initializer.sh` | 3/3 spinlock 初始化 | ESP HAL `clk_ctrl_os.c` |

## 使用方法

**时机**：在 `distclean` 后的首次构建时，脚本需要在构建**并行运行**。
因为 `esp-hal-3rdparty` 的 git clone 和 patch 是构建过程中异步执行的，
Fix 2 和 Fix 3 会等待目标文件出现（最多 180 秒）。

```bash
# 终端 1：启动构建
cd /home/arongw/openvela
source build/envsetup.sh
./build.sh vendor/espressif/boards/esp32s3/esp32s3-box-3/configs/smart_home/ -j8

# 终端 2：并行运行补丁
cd /home/arongw/openvela
bash contest2026_031_niudanxianqianchong/scripts/fix_box3_mbedtls_header_priority.sh
bash contest2026_031_niudanxianqianchong/scripts/fix_box3_mbedtls_disable_ccm.sh &
bash contest2026_031_niudanxianqianchong/scripts/fix_box3_spinlock_initializer.sh &
```

也可以一次运行全部（Fix 1 不需要等待，Fix 2 和 3 后台等待）：

```bash
cd /home/arongw/openvela
bash contest2026_031_niudanxianqianchong/scripts/fix_box3_mbedtls_header_priority.sh
bash contest2026_031_niudanxianqianchong/scripts/fix_box3_mbedtls_disable_ccm.sh &
bash contest2026_031_niudanxianqianchong/scripts/fix_box3_spinlock_initializer.sh &
```

## 各脚本详细说明

### Fix 1: `fix_box3_mbedtls_header_priority.sh`

**问题**：ESP HAL 编译时 `-I` 搜索路径中 apps mbedTLS 头文件优先级过高，
导致 ESP HAL 源文件用了 apps 侧的 `mbedtls_cipher_info_t`（字段不同）。

**修复**：把 `apps/crypto/mbedtls/Make.defs` 中的 `-I` 改成 `-isystem`。
`-isystem` 优先级低于 `-I`，ESP HAL 的 `-I` 路径会胜出，各用各的头文件。

**修改的文件**：`apps/crypto/mbedtls/Make.defs`（4 处 sed 替换）

**注意**：此脚本立即生效，不需要等待。

### Fix 2: `fix_box3_mbedtls_disable_ccm.sh`

**问题**：ESP-IDF 和 NuttX apps 的 `mbedtls_ccm_context` 结构体定义不同，
同时编译会导致 `cipher.c: 'mbedtls_cipher_info_t' has no member named 'base_idx'`。

**修复**：注释掉 ESP HAL mbedTLS 的 `#define MBEDTLS_CCM_C`，禁用 CCM 算法。
CCM 是 AES-CCM 认证加密，Wi-Fi WPA 不依赖它。

**修改的文件**：`nuttx/arch/xtensa/src/chip/esp-hal-3rdparty/components/mbedtls/mbedtls/include/mbedtls/mbedtls_config.h`

**等待机制**：轮询目标文件最多 180 秒。

### Fix 3: `fix_box3_spinlock_initializer.sh`

**问题**：ESP HAL 的 `clk_ctrl_os.c` 用 `#define LOCK_INITIALIZER_UNLOCKED 0`
初始化 `spinlock_t`，但 NuttX 的 `spinlock_t` 是 struct 类型，需要用
`SP_UNLOCKED` 宏（展开为 `{0}` 或 `{0, 0}`）。

**修复**：把 `0` 替换为 `SP_UNLOCKED`。

**修改的文件**：`nuttx/arch/xtensa/src/chip/esp-hal-3rdparty/components/esp_hw_support/clk_ctrl_os.c`

**等待机制**：轮询目标文件最多 180 秒。

## 与 ai_agent/fix_esp32s3.sh 的关系

本目录的脚本是 `packages/ai_agent/fix_esp32s3.sh` 的拆分版本，修复内容对应其 Fix 1-3。
`ai_agent/fix_esp32s3.sh` 额外包含 Fix 4（挂载 /data tmpfs），smart_home 不需要。

## 恢复

这些补丁是临时性的。`distclean` 后 ESP HAL 会重新 clone，补丁丢失，需要重新运行。
`apps/crypto/mbedtls/Make.defs` 的修改需要手动恢复或重新 sync apps 仓库。
