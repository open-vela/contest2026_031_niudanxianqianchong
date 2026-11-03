# GT911 触摸：轮询改中断 实施方案

> 版本：v1.0（2026-10-25）｜范围：ESP32-P4X Function EV Board
> 结论先行：**驱动层已支持中断模式，改造量集中在板级；但当前 LCD adapter 未把 INT 引出到 SoC，硬件不满足时本方案不成立。**

## 一、前置条件（Go / No-Go）

| 项 | 要求 | 不满足的后果 |
|---|---|---|
| INT 信号 | 必须已连到 P4 某个 GPIO，且可被配置为输入 | 无中断源，只能继续轮询；**不存在纯软件替代方案** |
| RESET 信号 | 可选。有则地址可控；无则沿用 `0x5d → 0x14` 探测 | 无 reset 时地址由上电时序决定，探测逻辑不能删 |

确认方法（任选其一，按可信度排序）：

1. 查 LCD adapter 原理图，确认 INT 网络与 P4 引脚号；
2. 万用表/示波器：触摸时测候选 GPIO，出现电平翻转即为 INT；
3. 软件探测：把候选脚配成 `INPUT | FALLING` 挂临时 ISR，触摸时打印计数，无计数即未接通。

> 若结论是"未引出"，本文档到此为止，保留 20 ms 轮询，并在 `board.h` 注释中记录该硬件限制。

## 二、现状（代码事实）

| 位置 | 现状 |
|---|---|
| `drivers/nuttx/drivers/input/gt911.c` | 已实现中断路径：`poll_interval_ms == 0` 时走 IRQ；ISR 关闭中断 → `work_queue(LPWORK)` → worker 处理完再打开（`gt911.c:627-686`） |
| 同上 | 板级接口已定义：`reset()` / `irq_attach()` / `irq_enable()`（`gt911.h` `struct gt911_board_s`） |
| `board/.../src/esp32p4_touch.c` | `.board = NULL`、`.poll_interval_ms = 20` → 强制走轮询 |
| `board/.../include/board.h` | 明确注释"adapter 未引出 reset/INT" |

结论：**不需要改驱动主体**，只需要把板级三个回调补上并把 `poll_interval_ms` 置 0。

## 三、改动清单

| # | 文件 | 改动 | 预估 |
|---|---|---|---|
| 1 | `board/.../include/board.h` | 新增 `BOARD_GT911_INT_GPIO`（必填）、`BOARD_GT911_RESET_GPIO`（可选）；保留 `BOARD_GT911_POLL_INTERVAL_MS` 作为兜底 | 6 行 |
| 2 | `board/.../src/esp32p4_touch.c` | 实现 `gt911_board_ops`（reset / irq_attach / irq_enable），`poll_interval_ms = 0` | ~70 行 |
| 3 | `board/.../Kconfig` | 新增 `ESP32P4_FUNCTION_EV_BOARD_GT911_IRQ`（bool）+ `..._INT_GPIO`（int）+ `..._RESET_GPIO`（int，-1 表示未接），`select ESPRESSIF_GPIO_IRQ` | ~20 行 |
| 4 | `configs/smart_home/defconfig`（及 voice/quickapp） | 打开 IRQ 开关并填 GPIO 号 | 3 行 |
| 5 | 驱动 | 默认不改；仅当实测 INT 无输出或丢点时按第六节补充 | 0~10 行 |

> `CONFIG_ESPRESSIF_GPIO_IRQ` 的初始化已由 `up_irqinitialize()` 调用 `esp_gpioirqinitialize()`（`chips/esp32p4/common/espressif/esp_irq.c:305`），板级无需再调。

## 四、关键实现

### 4.1 板级回调骨架

```c
static int gt911_board_reset(FAR const struct gt911_board_s *board)
{
#ifdef BOARD_GT911_RESET_GPIO
  /* 地址选择：GT911 在 RST 上升沿采样 INT 电平。
   * 0x5d = INT 低，0x14 = INT 高；按主地址 0x5d 处理。 */

  esp_configgpio(BOARD_GT911_INT_GPIO, OUTPUT);
  esp_gpiowrite(BOARD_GT911_INT_GPIO, false);
  esp_configgpio(BOARD_GT911_RESET_GPIO, OUTPUT);
  esp_gpiowrite(BOARD_GT911_RESET_GPIO, false);
  nxsig_usleep(10 * 1000);
  esp_gpiowrite(BOARD_GT911_RESET_GPIO, true);
  nxsig_usleep(60 * 1000);
#endif
  return OK;
}

static int gt911_board_irq_attach(FAR const struct gt911_board_s *board,
                                   xcpt_t isr, FAR void *arg)
{
  int ret = esp_configgpio(BOARD_GT911_INT_GPIO,
                           INPUT | PULLUP | FALLING);
  if (ret < 0)
    {
      return ret;
    }

  return esp_gpio_irq(BOARD_GT911_INT_GPIO, isr, arg);
}

static void gt911_board_irq_enable(FAR const struct gt911_board_s *board,
                                    bool enable)
{
  if (enable)
    {
      esp_gpioirqenable(BOARD_GT911_INT_GPIO);
    }
  else
    {
      esp_gpioirqdisable(BOARD_GT911_INT_GPIO);
    }
}
```

### 4.2 配置切换

```c
static const struct gt911_board_s g_gt911_board =
{
  .reset      = gt911_board_reset,
  .irq_attach = gt911_board_irq_attach,
  .irq_enable = gt911_board_irq_enable,
};

/* 中断模式：poll_interval_ms = 0 */
.board            = &g_gt911_board,
.poll_interval_ms = 0,
```

两个地址（`0x5d` / `0x14`）共用同一套 ops，探测顺序不变。

## 五、时序与并发约束

| 约束 | 说明 |
|---|---|
| ISR 只做三件事 | 关中断 → `work_queue` → 返回；I2C 读全部在 LPWORK |
| 清中断时机 | worker 读完 `0x814e` 状态后必须写 0，否则电平型中断会持续触发 |
| 重开顺序 | 必须先清状态、再 `irq_enable(true)`（驱动已按此顺序实现） |
| 与 ES8311 共用 I2C0 | worker 持 `priv->lock`，总线互斥由 I2C 驱动保证；单帧 8~40 字节，100 kHz 下 < 5 ms，不阻塞音频 |
| 边沿优先 | 默认 `FALLING`；若硬件为低电平保持型，改用 `ONLOW` 并确保清状态后重开 |

## 六、可选增强（实测有问题再加）

1. **INT 配置寄存器**：GT911 的 `0x804d`（INT 模式）/`0x804e`（极性）当前驱动未写。若实测 INT 无脉冲，在 reset 后补写这两项。
2. **兜底轮询**：IRQ 模式下再挂一个 200 ms 的 watchdog，仅用于补丢点/丢 UP；命中时置诊断计数。建议默认关闭，避免掩盖中断问题。
3. **多点**：改中断不改变 GT911 变长帧与 LVGL 固定采样长度的矛盾，`MAX_POINTS` 仍保持 1。

## 七、验收

| 项 | 方法 | 通过标准 |
|---|---|---|
| 基本事件 | `gt911_probe` 连续点击 100 次 | 无丢点，DOWN/UP 成对 |
| 拖拽 | UI 上连续拖拽 60 s | 无断线、无坐标跳变 |
| 空闲开销 | 对比轮询/中断两版 `ps` CPU 占用 | 空闲时 I2C 无事务，CPU 明显下降 |
| 竞态 | 触摸时并发播放音频 | 无 I2C 超时，音频无卡顿 |
| 回退 | 关闭 IRQ 开关重新编译 | 轮询模式行为与当前一致 |

## 八、工作量与风险

- 代码量：4 个文件约 100 行，驱动零改动。
- 工期：编码 0.5 天，真机验证 0.5 天（不含硬件确认）。
- 最大风险：**INT 未引出**。该风险无法用软件消除，必须在动工前用第一节的方法确认。
