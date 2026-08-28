# ESP32-P4X LittleFS 挂载失败与修复

## 范围与结论

本文只记录 ESP32-P4X 板级存储初始化和 GT911 I²C 探测的联调结论。两者是独立
问题：Flash 默认芯片未初始化会使 LittleFS 挂载失败；I²C 引脚属性配置错误会使
GT911 Product ID 探测收到 NACK。它们不应被归因于彼此冲突。

修复后，板级能够在创建 MTD 分区前初始化默认 Flash 芯片，并以 100 kHz 在
`0x5d -> 0x14` 顺序探测 GT911。

## 稳定配置

| 项目 | 配置 |
| --- | --- |
| Flash 容量 | 16 MiB |
| LittleFS 分区偏移 | `0x00800000` |
| LittleFS 分区大小 | `0x00100000` |
| MTD 设备 | `/dev/espflash` |
| 挂载点 | `/data` |
| GT911 总线 | I²C0，SCL=GPIO8，SDA=GPIO7 |
| GT911 速率 | 100 kHz |
| GT911 地址 | 主地址 `0x5d`，备用地址 `0x14` |

固件配置和烧录镜像必须使用相同的 Flash 分区偏移；历史配置中的 `0x00e00000`
不能与上述偏移混用。

## LittleFS 根因与修复

故障日志中的 `0x6003` 对应 ESP HAL 的 `ESP_ERR_FLASH_NOT_INITIALISED`。这说明
问题发生在 LittleFS 解析前的 Flash 读取路径：

```text
LittleFS mount
  -> MTD bread()
    -> esp_spiflash_read()
      -> esp_flash_read(NULL, ...)
        -> ESP_ERR_FLASH_NOT_INITIALISED
```

芯片层新增显式且幂等的 `esp_spiflash_initialize()`，依次确认默认 Flash 是否可用，
执行 `esp_flash_app_init()` 和 `esp_flash_init_default_chip()`，并校验
`esp_flash_default_chip` 及其驱动。板级在创建 MTD 分区前调用该接口：

```text
board_spiflash_init()
  -> esp_spiflash_initialize()
  -> esp_spiflash_alloc_mtdpart()
  -> register_mtddriver("/dev/espflash")
  -> mount("/dev/espflash", "/data", "littlefs")
```

普通启动不应强制格式化预置资源分区；`forceformat` 无法修复 Flash 未初始化，且会
清空已写入的数据。

## GT911 I²C 根因与修复

原实现错误地以逻辑或组合 GPIO 属性：

```c
#define SCL_PIN_ATTR (FUNCTION_2 || INPUT_PULLUP || OUTPUT_OPEN_DRAIN)
#define SDA_PIN_ATTR (FUNCTION_2 || INPUT_PULLUP || OUTPUT_OPEN_DRAIN)
```

逻辑或会把表达式折叠为布尔值 `1`，从而丢失输入上拉和开漏输出标志。修复改为
位或，并通过 GPIO Matrix 完成外设信号路由：

```c
#define SCL_PIN_ATTR (INPUT_PULLUP | OUTPUT_OPEN_DRAIN)
#define SDA_PIN_ATTR (INPUT_PULLUP | OUTPUT_OPEN_DRAIN)
```

同时，GT911 Product ID 读取前等待 100 ms，总线速率收敛到 100 kHz。只有首选
地址 `0x5d` 返回 I²C `-EIO` 时才释放临时实例并尝试 `0x14`，避免掩盖内存或设备
注册等其他错误。I²C 诊断会区分 NACK、timeout 和 arbitration lost。

## 验证边界

真机启动时已观测到以下关键路径：

```text
GT911 probing I2C address 0x5d
GT911 product id: 39 31 31 00 (911)
GT911 registered at /dev/input0: I2C0 address=0x5d poll=20ms
P4X default Flash chip initialized: size=0x01000000
P4X LittleFS mounted: source=/dev/espflash mount=/data
```

这验证了主地址、`/dev/input0` 注册、Flash MTD 与 `/data` 挂载可在同一次启动中
共存。备用地址回退、多点触摸以及上层 UI 输入接入不属于本次验证范围；应通过多次
断电冷启动继续覆盖上电稳定性和地址回退路径。

## 涉及文件

| 文件 | 说明 |
| --- | --- |
| `chips/esp32p4/common/espressif/esp_spiflash.c` | 默认 Flash 芯片的显式、幂等初始化。 |
| `chips/esp32p4/common/espressif/esp_spiflash_mtd.c` | MTD 读、写、擦除失败诊断。 |
| `board/esp32p4/common/src/esp_board_spiflash.c` | MTD/LittleFS 创建前的 Flash 就绪检查。 |
| `chips/esp32p4/common/espressif/esp_i2c.c` | GPIO 属性修复和原始 I²C 错误诊断。 |
| `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_touch.c` | 100 kHz、上电等待与双地址探测。 |
