# 存储 LittleFS 与系统集成（ESP32-P4X · SmartHome 资源链路）

| 元信息 | 内容 |
| --- | --- |
| 适配对象 | ESP32-P4X-Function-EV-Board（Route A：custom chip + custom board），16 MiB Flash |
| 本域范围 | 数据存储（LittleFS 数据分区挂载 `/data`）与系统集成（SmartHome 资源部署：完整 MiSans 字体 PSRAM 预加载、PNG 图标、编译烧录与数据镜像流程） |
| 当前真机状态 | 同一固件中 `/dev/input0` 注册与 `/data` LittleFS 挂载成功；完整 `MiSans-Normal.ttf`（7,943,504 B）PSRAM 预加载完成，12/14/16/20/32 px 五个字体实例创建，8 个 LVGL 页面全部构建完成（逐字日志见第四章） |
| 关键入口 | `scripts/make_p4x_littlefs_data_image.sh`（已核实存在）；`board/esp32p4/esp32p4-function-ev-board/configs/smart_home/defconfig`（已核实存在）；`demos/smart_home/`（已核实存在） |
| 当前分区布局 | 固件 `0x002000` 起（最大 6 MiB）+ LittleFS `0x600000`–`0xFFFFFF`（10 MiB，`0x00a00000`） |

> 路径核实：仓库根目录不存在 `configs/smart_home`，smart_home 配置实际位于 `board/esp32p4/esp32p4-function-ev-board/configs/smart_home/defconfig`，本文一律使用该路径。
> README「快速开始」的 `0x800000` 烧录命令与「默认打包字体子集」为旧布局遗留；当前以 `0x600000` + 完整字体为准（defconfig、板端日志、部署指南与打包脚本一致，见第六章）。

## 一、背景与目标

SmartHome 应用运行时经文件路径从 `/data` 读取资源：`/data/res/fonts/MiSans-Normal.ttf`
（完整中文字体）、`/data/res/icons/*.png`、`/data/res/skills/*.md`、`/data/smart_home/*.json`
（可选 `secrets.json`）。`/data` 由 LittleFS 数据分区提供：16 MiB Flash 划为固件 `0x002000` 起（最大 6 MiB）+ LittleFS 偏移 `0x00600000`、大小 `0x00a00000`（10 MiB），MTD 设备 `/dev/espflash`。偏移经历过演进：早期 `0x00e00000`（1 MiB）、`0x00800000 + 1 MiB`，README 又记载 `0x800000`；当前固件与资源镜像必须统一 `0x00600000 + 0x00a00000`，不能混用旧偏移。

目标：(1) 真机稳定挂载 `/data`；(2) 完整 MiSans 支撑任意中文；(3) 构建/打包/烧录/验证流程可重复并固化为部署指南。

## 二、适配流程

**阶段 1 数据分区与镜像方案**：确定「6 MiB 固件 + 10 MiB LittleFS」布局；镜像由
`scripts/make_p4x_littlefs_data_image.sh` 调 mklittlefs 生成 `out/p4x_littlefs_data/data_lfs.bin`；
`secrets.json` 默认排除，仅 `WITH_SECRETS=1` 加入。

**阶段 2 LittleFS 挂载**：芯片层提供显式幂等的 `esp_spiflash_initialize()`，板级按
`board_spiflash_init() → esp_spiflash_initialize() → esp_spiflash_alloc_mtdpart() →
register_mtddriver("/dev/espflash") → mount("/dev/espflash", "/data", "littlefs")` 挂载；
启动顺序固定 DSI framebuffer → GT911 → Flash MTD/LittleFS。责任边界：芯片层管 ESP HAL
默认 Flash 初始化与 MTD 访问，板级管分区/设备节点/挂载，应用只经文件路径读资源。

**阶段 3 挂载失败排障闭环**：首次挂载失败（`ESP_ERR_FLASH_NOT_INITIALISED`），沿
`mount → MTD bread() → esp_spiflash_read() → esp_flash_read(NULL, ...)` 定位到默认 Flash
芯片未初始化，显式幂等初始化修复（第五章问题 1），同一固件完成输入设备与 `/data` 共存验证。

**阶段 4 完整 MiSans PSRAM 预加载**：TinyTTF 文件流先后暴露「POSIX 路径被解释为盘符」
与「完整 CJK 字体细粒度 seek/read 阻塞」，改为启动时一次性读入 7,943,504 B 到 PSRAM
用户堆，`lv_tiny_ttf_create_data()` 建五个字号实例（第五章问题 2、3）。

**阶段 5 部署指南固化**：`docs/开发指南/ESP32-P4X-SmartHome编译烧录与资源部署指南.md` 固化「构建 → 烧录固件 → 打包资源（可选 secrets）→ 烧录镜像 → NSH 验证」闭环，含常见问题速查与「改什么 → 重建固件还是仅重刷镜像」的迭代决策表。

## 三、关键脚本与配置

### 3.1 `scripts/make_p4x_littlefs_data_image.sh`（已核实存在）

```bash
cd ~/openvela
contest2026_031_niudanxianqianchong/scripts/make_p4x_littlefs_data_image.sh
```

脚本默认值（已核实）：`DATA_SIZE=0xa00000`、`BLOCK_SIZE=4096`、`PAGE_SIZE=256`、`FLASH_OFFSET=0x600000`、`FLASH_SIZE_BYTES=0x1000000`、`WITH_FONTS=1`、`WITH_ICONS=1`、`WITH_SECRETS=0`、`FONT_SOURCE=demos/smart_home/res/fonts/MiSans-Normal.ttf`（完整字体）；mklittlefs 取自 `vendor/artinchip/tools/scripts/mklittlefs`。脚本检查分区范围不越界、技能与配置目录非空（否则失败退出），结束时打印镜像内 staged 文件清单和建议烧录命令。

默认打包范围：`/data/res/fonts/MiSans-Normal.ttf`、`/data/res/icons/*.png`、
`/data/res/skills/*.md`、`/data/smart_home/` 下 backends/settings/mcp_bridge 等非敏感 JSON；
内嵌 LVGL 图标字体与 C 图像编入固件不重复打包；调试最小镜像可传 `WITH_FONTS=0
WITH_ICONS=0`（README）。输出 `out/p4x_littlefs_data/data_lfs.bin`。`WITH_SECRETS=1` 仅把
`demos/smart_home/res/config/secrets.json` 加入镜像、不重建固件；镜像是整个 `/data` 快照，
烧录会覆盖运行时写入文件；不得用 `cat` 验证密钥。

### 3.2 烧录与构建命令（当前布局）

```bash
cd ~/openvela
esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m \
  0x2000 nuttx/nuttx.bin

esptool --chip esp32p4 --port /dev/ttyACM0 --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m \
  0x600000 out/p4x_littlefs_data/data_lfs.bin
```

```bash
cd ~/openvela
./build.sh \
  contest2026_031_niudanxianqianchong/board/esp32p4/esp32p4-function-ev-board/configs/smart_home \
  -j8
```

指南要求构建后核对 `nuttx/.config` 至少含 `CONFIG_ESPRESSIF_SIMPLE_BOOT=y`、
`CONFIG_ESPRESSIF_STORAGE_MTD_OFFSET=0x600000`、`CONFIG_ESPRESSIF_STORAGE_MTD_SIZE=0xa00000`、
`CONFIG_ESPRESSIF_FLASH_MODE_DIO=y`。

### 3.3 目录规划

| 位置 | 内容 | 归属 |
| --- | --- | --- |
| `/data/res/fonts/MiSans-Normal.ttf` | 完整 MiSans 字体（7,943,504 B） | 资源镜像 |
| `/data/res/icons/*.png`、`/data/res/skills/*.md` | PNG 图标、技能 Markdown | 资源镜像 |
| `/data/smart_home/*.json` | backends/settings/mcp_bridge 等，可选 secrets | 资源镜像 |
| 固件内（`src/ui/lvgl/icons/*.c` 等 C 源） | LVGL 图标字体、转换 C 图像 | nuttx.bin |

源码入口（已核实存在）：`demos/smart_home/src/ui/lvgl/smart_home_lvgl_style.c`（字体预加载、
实例创建与释放）、`demos/smart_home/src/smart_home_memory.[ch]`（统一大块内存接口）。

## 四、适配证据（真机验收）

**证据 1：`/data` 挂载成功与 GT911 共存（逐字摘录，出处：
`docs/开发日志/ESP32-P4X-LittleFS挂载失败与修复.md` 第 7 节）：**

```text
GT911 power-on wait: 100 ms before product-ID probe
GT911 probing I2C address 0x5d
GT911 product id: 39 31 31 00 (911)
GT911 registered at /dev/input0: I2C0 address=0x5d poll=20ms
P4X storage bring-up: offset=0x00600000 size=0x00a00000
P4X default Flash chip initialized: size=0x01000000
P4X MTD partition: flash_size=0x1000000 offset=0x600000 size=0xa00000
P4X LittleFS register: source=/dev/espflash mount=/data
P4X LittleFS mounted: source=/dev/espflash mount=/data
```

**证据 2：`/data/res` 资源核验命令**（出处：LittleFS 日志第 9 节、部署指南第 9 节）：

```text
nsh> ls /data
nsh> ls /data/res
nsh> ls /data/res/fonts
nsh> ls /data/smart_home
nsh> smart_home
```

文档记载预期：LittleFS 日志含 `offset=0x00600000 size=0x00a00000` 与 `mounted`；`/data/res/fonts/MiSans-Normal.ttf` 存在；`smart_home` 输出页面构建完成日志并显示首页。说明：开发日志未保存 `ls /data/res` 屏幕输出存档，现场按上述命令核验；镜像侧清单由打包脚本 staged files 输出给出（见 3.1）；资源确实可读的逐字证据见证据 3。

**证据 3：完整 MiSans PSRAM 预加载与五个字体实例（逐字摘录，出处：
`docs/开发日志/ESP32-P4X-SmartHome-完整MiSans-PSRAM预加载修复.md` 第 4 节；
地址为该次启动的 PSRAM 分配结果）：**

```text
[smart_home_lvgl] font preload begin path=/data/res/fonts/MiSans-Normal.ttf
[smart_home_lvgl] font preload done size=7943504 buffer=0x4831b348 region=PSRAM(user-heap)
[smart_home_lvgl] font instances source=PSRAM-data size=7943504 font12=0x48316b28 font14=0x4831b2e0 font16=0x4831b308 font20=0x48aaea80 font32=0x48aaeb48
[smart_home_lvgl] build screensaver begin
[smart_home_lvgl] build screensaver done
[smart_home_lvgl] build home begin
[smart_home_lvgl] build home done
[smart_home_lvgl] build panel begin
[smart_home_lvgl] build panel done
[smart_home_lvgl] build scenes begin
[smart_home_lvgl] build scenes done
[smart_home_lvgl] build security begin
[smart_home_lvgl] build security done
[smart_home_lvgl] build more begin
[smart_home_lvgl] build more done
[smart_home_lvgl] build chat begin
[smart_home_lvgl] build chat done
[smart_home_lvgl] build settings begin
[smart_home_lvgl] build settings done
[lvgl] ui init done
[lvgl] show done, entering run loop
```

证明：完整字体确从 LittleFS 读完且缓冲区位于 PSRAM 用户堆；五个字号实例全部创建、无静默回退；8 个页面全部构建完成，未再卡在第一个中文字形；`renew wlan0` 的 DHCP 失败与该问题无关（原文注明）。

## 五、遇到的问题与解决

**问题 1：ESP HAL 默认 Flash 芯片未初始化，LittleFS 挂载失败。**
现象（逐字，出处：LittleFS 日志 3.1 节）：

```text
P4X storage bring-up: offset=0x00e00000 size=0x00100000
P4X MTD partition: flash_size=0x1000000 offset=0xe00000 size=0x100000
P4X LittleFS register: source=/dev/espflash mount=/data
P4X flash read backend failed: address=0x00e00000 length=256 vendor_ret=24579
P4X LittleFS initial mount failed: ret=-1
ERROR: Failed to setup littlefs
```

定位：`24579 = 0x6003` 即 `ESP_ERR_FLASH_NOT_INITIALISED`，故障发生在 LittleFS 解析镜像
之前（`mount → MTD bread() → esp_spiflash_read() → esp_flash_read(NULL, ...)`）。根因：
Route A 板级不能仅依赖 `ESP_SYSTEM_INIT_FN(init_flash, ...)` 回调保证存储就绪，创建 MTD
分区时 `esp_flash_default_chip` 可能仍不可读；外部烧录工具写 Flash 成功只证明硬件与地址
可访问，不证明运行时 ESP HAL Flash 客户端已初始化。修复：芯片层增加显式幂等的
`esp_spiflash_initialize()`（`esp_flash_app_init() → esp_flash_init_default_chip() → 校验
esp_flash_default_chip 及 chip_drv`），板级创建 MTD 分区前调用；复测即证据 1。无效/高风险
方案：`forceformat` 不能修复未初始化；跳过 `esp_flash_app_init()` 实测导致
`ESP_ERR_NO_MEM (0x101)`（Flash Guard 与 bounce buffer 未就绪）；预置镜像设备不得随意
强制格式化，否则清空 `/data/res`。

**问题 2：TinyTTF POSIX 路径被解释为 LVGL 盘符。**
现象：`lv_tiny_ttf_create_file()` 以无盘符路径 `/data/...` 调用时，LVGL POSIX FS 把其解释
为驱动名，字体对象创建结果为 `0`，页面退回 Montserrat、中文显示缺字方框。定位：改用
`A:/data/res/fonts/MiSans-Normal.ttf` 后五个对象可创建。根因：LVGL FS 盘符约定。修复：
TTF 使用 `A:` 盘符路径；问题 3 改为数据预加载后，PNG 图标仍保留
`CONFIG_LV_USE_FS_POSIX=y` 与盘符 `A:`。

**问题 3：完整 CJK 字体走 TinyTTF 文件流，首个中文字形处阻塞。**
现象：五个对象创建成功后卡在 `[smart_home_lvgl] build screensaver begin`。定位：用
`pyftsubset` 制作仅含屏保文案的小字体后全部页面可构建，证明显示、触摸、页面结构与资源
挂载链路正常，问题集中在完整 TTF 的文件流访问。根因：完整 MiSans 约 2.9 万字形，CJK
字形定位与轮廓读取非连续，大量细粒度 seek/read 使 LittleFS 文件流在首个中文标签渲染时
不可接受阻塞；`CONFIG_LV_FS_POSIX_CACHE_SIZE` 增至 `4096` 仍复现，非根治方案。修复：
`demos/smart_home/src/ui/lvgl/smart_home_lvgl_style.c` 实现 `open + fstat →
smart_home_bulk_alloc(7,943,504) → 循环 read（处理短读与 EINTR）→ PSRAM 缓冲区 →
lv_tiny_ttf_create_data(buffer, size, 12/14/16/20/32)`；`CONFIG_MM_KERNEL_HEAP=y` 与
`CONFIG_ESPRESSIF_SPIRAM_USER_HEAP=y` 使用户堆位于 PSRAM；同时移除
`CONFIG_LV_TINY_TTF_FILE_SUPPORT`。五个实例共享同一份只读数据，退出顺序固定为销毁五个
`lv_font_t` 后再释放缓冲区；文件不存在、`fstat` 失败、分配失败或读取不完整时回退内置
Montserrat，不卡启动。复测即证据 3。

**问题 4（同固件共存）：GT911 首次 I²C 读取 NACK。**
现象（逐字，出处：LittleFS 日志 4.1 节）：`ERROR: GT911 probe failed: stage=product-id-read
i2c_addr=0x5d frequency=100000 reg=0x8140 ret=-5`；`I2C0 transfer failed: msg=0 addr=0x5d
frequency=100000 raw=0x00000400 nack=1 timeout=0 arbitration_lost=0`。根因：SCL/SDA 引脚
属性误用逻辑或 `||`，`(FUNCTION_2 || INPUT_PULLUP || OUTPUT_OPEN_DRAIN)` 折叠为 `1`，
丢失上拉与开漏标志；修复为位或 `(INPUT_PULLUP | OUTPUT_OPEN_DRAIN)` 并移除 `FUNCTION_2`
（GPIO Matrix 已显式路由），配套 400 kHz→100 kHz、Product ID 读取前等 100 ms、
`0x5d → 0x14` 双地址探测。NACK 时挂载尚未开始，与 LittleFS 无直接因果，但共用启动顺序，
修复后同一固件两者共存（证据 1）；原文提示一次成功不足以证明早期所有间歇性 NACK 均只由
该处引起，仍需多次冷启动验证。

## 六、当前验收状态与边界

已验收（真机）：`/data` 于 `0x00600000 + 0x00a00000` 挂载成功（与 `/dev/input0` 同一固件
共存）；完整 MiSans 7,943,504 B 读入 PSRAM、五个字号实例创建、8 个页面构建完成并进入
运行循环。边界与约束：

1. 回归要求：至少 10 次断电冷启动，每次确认 `/dev/input0`、`/data/res` 与 LVGL 触摸可用。
2. 一致性：`CONFIG_ESPRESSIF_STORAGE_MTD_OFFSET=0x600000` 与烧录命令必须一致，镜像不超过 `CONFIG_ESPRESSIF_STORAGE_MTD_SIZE=0xa00000`；不得再用 `0x800000`、`0xE00000` 旧地址（指南明文警告；README 快速开始的 `0x800000` 为旧值）。
3. 数据安全：预置镜像设备不得随意强制格式化；日常资源更新不执行 `erase_flash`；
   `WITH_SECRETS=1` 镜像覆盖 `/data` 运行时状态，Wi-Fi 凭据需重配。
4. 内存预算：完整字体常驻 PSRAM 至少 7,943,504 B（另加 TinyTTF 字形缓存与 LVGL 对象
   开销）；加摄像头帧缓冲、大 PNG、网络缓冲或更多字重前须重测 PSRAM 剩余量与最大连续块；
   不得退回完整 TTF 的 LittleFS 文件流路径，也不得在退出 LVGL 前释放其数据缓冲区。
5. 待办验收（MiSans 文档第 6 节）：Agent 页长中文/罕见汉字/中英混排、20 次页面往返、
   网络音频摄像头组合固件的内存复测。
6. 备用地址 `0x14` 回退逻辑已编入固件，但所引真机日志未触发该路径，未单独验收。

## 附录：原始文档索引

| 文档（仓库相对路径） | 用途 |
| --- | --- |
| `docs/开发日志/ESP32-P4X-LittleFS挂载失败与修复.md` | 分区布局、Flash 初始化根因与修复、启动顺序、真机挂载日志、构建烧录命令、回归要求 |
| `docs/开发日志/ESP32-P4X-SmartHome-完整MiSans-PSRAM预加载修复.md` | 完整字体加载问题定位、PSRAM 预加载实现、真机预加载日志、存储与内存边界 |
| `docs/开发指南/ESP32-P4X-SmartHome编译烧录与资源部署指南.md` | 镜像布局、构建/烧录/打包/部署 secrets/NSH 验证、常见问题与迭代决策 |
| `README.md` | 辅助事实源：快速开始的数据镜像命令（含 `0x800000` 旧值与最小镜像参数） |
| `scripts/make_p4x_littlefs_data_image.sh` | 资源镜像打包脚本（已核实：默认 `0x600000`/`0xa00000`、完整字体、staged 清单输出） |
