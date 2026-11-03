# ESP32-P4X microSD 卡存储适配方案

状态：方案阶段，尚未完成驱动实现与实板验收。

## 1. 目标

通过板载 microSD 卡槽提供标准块设备，挂载 FAT32 到 `/mnt/sdcard`，完成文件读写、
正常卸载及重启后数据保留，并与 C6 Wi-Fi/BLE 共存。首期使用预格式化的 FAT32 卡，
不包含 UHS、exFAT、自动格式化和带文件打开状态的热拔插。

## 2. 硬件与现状

| 项目 | 接入方式 |
| --- | --- |
| microSD | SDMMC slot 0，CLK=GPIO43、CMD=GPIO44、D0～D3=GPIO39～42 |
| 供电 | 按 P4X 板卡版本核对 LDO4、SD_PWRn 及上电时序；首期固定 3.3 V，不切换 1.8 V |
| 板载 C6 | SDMMC slot 1，CLK=GPIO18、CMD=GPIO19、D0～D3=GPIO14～17 |
| 卡检测 | 官方 BSP 未配置 CD/WP；首期采用启动探测、显式卸载，不假定有检测中断 |

当前 `esp_hosted_sdio.c` 仅服务 C6，使用单例并在初始化时复位整个控制器；尚未提供
NuttX `sdio_dev_s` 接口。两个槽位共享控制器，不能分别初始化、复位或释放公共资源。
实施前按实板版本确认上述接线与供电，并固定所用 BSP、HAL 参考版本。

## 3. 架构

```text
文件操作 → FAT32 → MMCSD → sdio_dev_s → slot 0 → microSD
                                             ↓
                              共享 SDMMC 控制器管理层
                                             ↑
C6 Wi-Fi/BLE → ESP-Hosted → 现有 SDIO 接口 → slot 1
```

- 控制器层统一持有时钟、复位、FIFO、DMA、中断和引用计数，维护两个槽位状态。
- 全局锁覆盖完整的命令、数据和完成等待；长传输按有限块数拆分，避免饿死 C6。
- 公共源时钟保持稳定，槽位时钟分别配置；正常关闭单槽不得复位控制器或关闭另一槽。
- 新增 `sdio_dev_s` lower-half，接入现有 MMCSD 驱动，不自建块设备协议。
- 板级负责引脚、供电、块设备注册；挂载失败只报错，不格式化，不影响 LittleFS `/data`。
- DMA 描述符使用可访问的内部内存；用户缓冲不满足对齐或 DMA 条件时使用中转缓冲，
  统一处理缓存同步、超时、取消和失败回收。

## 4. 改动范围

以下路径相对项目根目录，新增文件名为规划名称。

| 位置 | 工作内容 |
| --- | --- |
| `chips/esp32p4/common/espressif/esp_sdmmc_host.c` 及对应头文件 | 抽取共享控制器管理层，承接现有 SDIO 底层资源 |
| `chips/esp32p4/common/espressif/esp_hosted_sdio.c` | 保留 C6 接口，改为使用共享控制器 |
| `chips/esp32p4/common/espressif/esp_sdmmc.c` 及对应头文件 | 实现 `sdio_dev_s`：命令、长短响应、数据、DMA、等待与取消 |
| `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_sdcard.c` | slot 0、供电及 MMCSD 注册 |
| 芯片与板级 Kconfig、Make/CMake、`board.h`、bring-up | 增加独立开关与注册入口 |
| 板级 `configs/sdcard/defconfig`、`app/sdcard_probe/` | 最小验证配置与文件完整性测试 |

复用工作区 `nuttx/drivers/mmcsd/` 和 FAT 文件系统；以
`nuttx/arch/xtensa/src/esp32s3/esp32s3_sdmmc.c` 参考 NuttX 接口组织，
寄存器、时钟与引脚使用 P4 资料。

## 5. 实施与验收

| 阶段 | 工作 | 通过条件 |
| --- | --- | --- |
| S0 | 核对卡槽供电、接线、HAL slot 0 支持 | 固定板卡与参考版本，确认 3.3 V 和卡检测方式 |
| S1 | 抽取共享控制器，保持 C6 外部接口 | C6 原有枚举、联网、持续收发回归通过 |
| S2 | 独立 SD 卡配置，400 kHz、1-bit 初始化 | 读取 CID/CSD、识别容量，标准 MMCSD 块设备可见 |
| S3 | FAT32 挂载、读写、卸载 | 生成 16 MiB 确定性测试文件，读回校验一致；卸载、重启后再次一致 |
| S4 | 启用 4-bit，逐步升至 20 MHz；稳定后评估 40 MHz | 连续读写 30 分钟无校验错误，记录实测吞吐 |
| S5 | SD 文件读写与 C6 Wi-Fi/BLE 并发 | 两种初始化顺序各冷启动 10 次；并发 30 分钟无卡死、数据错误或持续内存增长 |

无卡、挂载失败、传输超时应有明确错误并保持系统可交互。重新初始化单槽不得破坏另一槽；
必须全控制器复位时，先终止两槽请求，再统一恢复 SD 和 C6。

## 6. 依据

- [乐鑫板级引脚定义](https://github.com/espressif/esp-bsp/blob/master/bsp/esp32_p4_function_ev_board/include/bsp/esp32_p4_function_ev_board.h)
- [乐鑫 slot 0 与 LDO4 初始化](https://github.com/espressif/esp-bsp/blob/master/bsp/esp32_p4_function_ev_board/esp32_p4_function_ev_board.c)
- [P4X 板卡说明](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-function-ev-board/user_guide.html)
