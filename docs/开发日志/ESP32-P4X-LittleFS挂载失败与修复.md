# ESP32-P4X LittleFS 挂载失败与修复

## 1. 文档范围与最终结论

P4X Smart Home 联调过程中曾同时出现两类故障：

1. ESP HAL 默认 Flash 芯片未完成初始化，导致 LittleFS 底层 MTD
   读取失败，`/data` 无法挂载。
2. P4 I²C 的 SCL/SDA 引脚属性使用了 C 逻辑或 `||`，属性值被折叠为
   `1`，丢失了上拉与开漏输出标志，GT911 首次 Product ID 读取可出现
   地址 NACK。

这是两个需要分别修复的问题，不应简化为“LittleFS 与 GT911 直接冲突”。
当前真机已在同一份固件中成功创建 `/dev/input0`并挂载 `/data`。

## 2. 当前稳定配置

| 项目 | 当前配置 |
| --- | --- |
| Flash 容量 | 16 MiB |
| LittleFS 资源分区偏移 | `0x00800000` |
| LittleFS 资源分区大小 | `0x00100000` (1 MiB) |
| MTD 设备 | `/dev/espflash` |
| 挂载点 | `/data` |
| GT911 总线 | I²C0，SCL=GPIO8，SDA=GPIO7 |
| GT911 速率 | 100 kHz |
| GT911 探测地址 | 主地址 `0x5d`，备用地址 `0x14` |
| 板级启动顺序 | DSI framebuffer → GT911 → Flash MTD/LittleFS |

> 历史版本曾使用 `0x00e00000`。当前固件与资源镜像必须统一使用
> `0x00800000`，不能混用新旧偏移。

## 3. 问题一：LittleFS 底层 Flash 未初始化

### 3.1 故障现象

初始实现在挂载 `/data` 时输出：

```text
P4X storage bring-up: offset=0x00e00000 size=0x00100000
P4X MTD partition: flash_size=0x1000000 offset=0xe00000 size=0x100000
P4X LittleFS register: source=/dev/espflash mount=/data
P4X flash read backend failed: address=0x00e00000 length=256 vendor_ret=24579
P4X LittleFS initial mount failed: ret=-1
ERROR: Failed to setup littlefs
```

`24579 = 0x6003`，对应 ESP HAL 的 `ESP_ERR_FLASH_NOT_INITIALISED`。故障发生在
LittleFS 解析镜像之前：

```text
LittleFS mount
  → MTD bread()
    → esp_spiflash_read()
      → esp_flash_read(NULL, ...)
        → ESP_ERR_FLASH_NOT_INITIALISED
```

### 3.2 根因

构建系统虽然编入 ESP-IDF Flash 相关源文件，但板级 Route A 不能仅依赖
`ESP_SYSTEM_INIT_FN(init_flash, ...)` 回调来保证存储已就绪。创建 MTD 分区时，
`esp_flash_default_chip` 仍可能未处于可读状态。

外部烧录工具能成功写入 Flash，只能证明硬件和目标地址可访问，不能证明
NuttX 运行时的 ESP HAL Flash 客户端已完成初始化。

### 3.3 正式修复

在 P4 芯片层增加显式且幂等的 `esp_spiflash_initialize()`：

```text
esp_spiflash_initialize()
  → 默认 Flash 已就绪：直接返回
  → esp_flash_app_init()
  → esp_flash_init_default_chip()
  → 校验 esp_flash_default_chip 及 chip_drv
```

板级在创建 MTD 分区前调用该入口：

```text
board_spiflash_init()
  → init_storage_partition()
    → esp_spiflash_initialize()
    → esp_spiflash_alloc_mtdpart()
    → register_mtddriver("/dev/espflash")
    → mount("/dev/espflash", "/data", "littlefs")
```

修复后的责任边界为：

- 芯片层负责 ESP HAL 默认 Flash 初始化和 MTD 访问。
- 板级负责分区、设备节点注册和 `/data` 挂载。
- Smart Home 应用只通过文件路径读取字体、PNG、技能和配置资源。

### 3.4 无效或高风险方案

- `forceformat` 不能修复 Flash 未初始化；格式化前仍要先读写 MTD。
- 不应为了规避联调问题跳过 `esp_flash_app_init()`。实测会导致
  `ESP_ERR_NO_MEM (0x101)`，原因是 Flash Guard 及内部 bounce buffer 未就绪。
- 预置资源镜像的设备不应在普通启动中随意强制格式化，否则会清空
  `/data/res` 内容。

## 4. 问题二：GT911 首次 I²C 读取 NACK

### 4.1 故障现象

LittleFS 完成修复后，GT911 在部分启动中无法注册：

```text
GT911 power-on wait: 100 ms before product-ID probe
ERROR: GT911 probe failed: stage=product-id-read
       i2c_addr=0x5d frequency=100000 reg=0x8140 ret=-5
ERROR: Failed to register GT911 at 0x5d: -5
```

P4 I²C 诊断日志进一步确认：

```text
I2C0 transfer failed: msg=0 addr=0x5d frequency=100000
raw=0x00000400 nack=1 timeout=0 arbitration_lost=0
```

这说明 `-EIO` 来自第一个地址消息的 NACK，不是超时或总线仲裁丢失。
此时 LittleFS 挂载还没有开始，所以 `mount("/data")` 不是该次 NACK 的直接触发点。

### 4.2 I²C 引脚属性的代码级根因

原实现错误地使用了逻辑或 `||`：

```c
#define SCL_PIN_ATTR (FUNCTION_2 || INPUT_PULLUP || OUTPUT_OPEN_DRAIN)
#define SDA_PIN_ATTR (FUNCTION_2 || INPUT_PULLUP || OUTPUT_OPEN_DRAIN)
```

`||` 返回布尔结果。只要任一宏为非零值，整个表达式就会折叠为整数 `1`，
并不会组合 GPIO 位标志。结果是 `INPUT_PULLUP` 和 `OUTPUT_OPEN_DRAIN` 未被
按预期配置，不符合 I²C 对上拉和开漏输出的要求。

修复后对齐 NuttX 通用 ESP I²C 实现：

```c
#define SCL_PIN_ATTR (INPUT_PULLUP | OUTPUT_OPEN_DRAIN)
#define SDA_PIN_ATTR (INPUT_PULLUP | OUTPUT_OPEN_DRAIN)
```

此处使用位或 `|` 组合引脚属性。同时移除 `FUNCTION_2`，原因是当前 P4
I²C 初始化已通过 GPIO Matrix 显式完成外设信号路由：

```c
esp_configgpio(pin, INPUT_PULLUP | OUTPUT_OPEN_DRAIN);
esp_gpio_matrix_out(pin, output_signal, 0, 0);
esp_gpio_matrix_in(pin, input_signal, 0);
```

因此引脚电气属性和外设路由应分开配置，不需要在属性中额外强制
`FUNCTION_2`。

### 4.3 联调收敛措施

除修复 I²C 引脚属性外，还实施了以下稳定性措施：

1. GT911 总线速率由 400 kHz 降为官方参考默认的 100 kHz。
2. Product ID 首次读取前等待 100 ms，让 LCD adapter 供电和控制器启动稳定。
3. 按 `0x5d` → `0x14` 顺序自动探测；仅在 `-EIO` 时尝试备用地址，
   避免掩盖内存、设备注册等其他错误。
4. P4 I²C 在任务上下文输出原始错误位，明确区分 NACK、timeout 和
   arbitration lost。
5. 板级保持 `DSI framebuffer → GT911 → Flash MTD/LittleFS` 顺序，
   先完成 LCD adapter 上的触摸探测，再挂载应用资源。

I²C 引脚属性是已确认的代码正确性问题，必须修复；但仅凭一次启动成功
不足以证明早期所有间歇性 NACK 都只由该处引起，仍需通过多次冷启动验证。

## 5. 修复后的完整启动链

```text
esp_bringup()
  → board_mipi_dsi_fb_initialize()
      → 创建 /dev/fb0
      → 完成 LCD adapter 显示侧供电与复位时序
  → board_gt911_initialize()
      → esp_i2cbus_initialize(I2C0)
      → 等待 100 ms
      → 用 100 kHz 探测 0x5d/0x14
      → 注册 /dev/input0
  → board_spiflash_init()
      → esp_spiflash_initialize()
      → 创建 0x00800000 + 1 MiB MTD 分区
      → 注册 /dev/espflash
      → 挂载 LittleFS 到 /data
  → 启动 NSH/应用
```

## 6. 主要修改文件

### 6.1 Flash/LittleFS

| 文件 | 修改内容 |
| --- | --- |
| `chips/esp32p4/common/espressif/esp_spiflash.h` | 声明 `esp_spiflash_initialize()`。 |
| `chips/esp32p4/common/espressif/esp_spiflash.c` | 实现默认 Flash 芯片的显式、幂等初始化。 |
| `chips/esp32p4/common/espressif/esp_spiflash_mtd.c` | 增加 MTD 读、写、擦除失败诊断。 |
| `board/esp32p4/common/src/esp_board_spiflash.c` | 在 MTD/LittleFS 创建前保证 Flash 就绪。 |
| `chips/esp32p4/common/espressif/Kconfig` | 增加存储分区和可关闭的诊断配置。 |
| `board/esp32p4/esp32p4-function-ev-board/configs/smart_home/defconfig` | 启用 LittleFS，将资源分区设为 `0x00800000`。 |

### 6.2 GT911/I²C

| 文件 | 修改内容 |
| --- | --- |
| `chips/esp32p4/common/espressif/esp_i2c.c` | 将 SCL/SDA 属性修正为 `INPUT_PULLUP \| OUTPUT_OPEN_DRAIN`，增加原始错误位诊断。 |
| `board/esp32p4/esp32p4-function-ev-board/include/board.h` | 定义 I²C0 引脚、100 kHz 及 `0x5d/0x14` 地址。 |
| `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_touch.c` | 增加上电等待、双地址探测和注册日志。 |
| `board/esp32p4/esp32p4-function-ev-board/src/esp32p4_bringup.c` | 固定 DSI、GT911、LittleFS 的板级启动顺序。 |
| `drivers/nuttx/drivers/input/gt911.c` | 完善 Product ID 探测、事件解码和诊断输出。 |

## 7. 真机验证结果

修复后的关键启动日志为：

```text
GT911 power-on wait: 100 ms before product-ID probe
GT911 probing I2C address 0x5d
GT911 product id: 39 31 31 00 (911)
GT911 registered at /dev/input0: I2C0 address=0x5d poll=20ms
P4X storage bring-up: offset=0x00800000 size=0x00100000
P4X default Flash chip initialized: size=0x01000000
P4X MTD partition: flash_size=0x1000000 offset=0x800000 size=0x100000
P4X LittleFS register: source=/dev/espflash mount=/data
P4X LittleFS mounted: source=/dev/espflash mount=/data
```

该日志证明本次启动中：

- GT911 在主地址 `0x5d` 响应，`/dev/input0` 注册成功。
- Flash MTD 创建成功，LittleFS 挂载到 `/data`。
- 备用地址 `0x14` 的回退逻辑已编译进固件，但此次真机启动未强制触发该路径。

## 8. 构建、生成资源镜像和烧录

### 8.1 构建 Smart Home 固件

```bash
cd ~/openvela

PATH="$PWD/myenv/bin:$PATH" ./build.sh \
  contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home \
  -j2
```

### 8.2 生成 LittleFS 资源镜像

```bash
cd ~/openvela
contest2026_031_niudanxianqianchong/scripts/make_p4x_littlefs_data_image.sh
```

默认输出为：

```text
out/p4x_littlefs_data/data_lfs.bin
```

### 8.3 烧录固件与资源

```bash
cd ~/openvela

esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m \
  0x2000 nuttx/nuttx.bin

esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m \
  0x800000 out/p4x_littlefs_data/data_lfs.bin
```

## 9. NSH 验证

复位进入 NSH 后执行：

```text
ls /dev/fb0
ls /dev/input0
ls /data
ls /data/res/fonts
gt911_probe 15
smart_home
```

预期结果：

- `/dev/fb0` 存在，LCD 可正常显示。
- `/dev/input0` 存在，`gt911_probe` 能输出 DOWN/MOVE/UP 事件。
- `/data/res/fonts/MiSans-Normal.ttf` 等资源存在。
- `smart_home` 能读取 `/data/res` 并启动 LVGL UI。

## 10. 回归要求与剩余风险

1. 至少执行 10 次断电冷启动，每次同时确认 `/dev/input0`、`/data/res`
   和 LVGL 触摸可用。
2. 验证单指点按、滑动、抬起及边缘坐标，防止只验证 Product ID 而遗漏
   事件链问题。
3. 保持固件 `CONFIG_ESPRESSIF_STORAGE_MTD_OFFSET=0x800000` 与烧录命令一致。
4. 不要将一次成功解读为“间歇性 NACK 的唯一根因已得到绝对证明”。
   当前结论是：Flash 初始化闭环、I²C 引脚属性、探测参数和启动顺序
   已经形成可共存的真机配置。
