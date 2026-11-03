# ESP32-P4X 驱动缺口与 BUG 清单

> 版本：v1.0（2026-10-25）
> 范围：`chips/esp32p4/` + `board/esp32p4/esp32p4-function-ev-board/` + 相关 NuttX 驱动
> 性质：静态代码审查结论，未构建、未上板；严重度按"能否阻塞验收"划分
> 状态口径：**已验收** = 有真机日志；**代码就绪** = 可编译、无实板证据；**缺陷** = 静态可证

## 一、总览

| 域 | 状态 | 结论 |
|---|---|---|
| USB Serial/JTAG + NSH | 已验收 | — |
| MIPI-DSI EK79007 1024×600 | 已验收 | 双缓冲与 cache 同步正确 |
| GT911 触摸 | 已验收 | 但入库 defconfig 未开启（G-03） |
| SC2336 MIPI-CSI V4L2 | 已验收 | 30 fps 通路可用 |
| C6 WiFi（ESP-Hosted SDIO） | 已验收 | 初始化回滚完善 |
| LittleFS 数据分区 | 已验收 | — |
| ES8311 音频 | **缺陷** | 当前配置下播放必失败、录音会截断 |
| 以太网 EMAC | 代码就绪 | 无实板日志 |

**核心结论**：音频不是"偶发卡死"，是 `8192 > 8064` 的配置越界（B-01）+ 采集包被误标 FINAL（B-02）。修这两条之前，16 kHz 单声道录放验收无法成立。

## 二、驱动缺口（G）

| ID | 缺口 | 现状 | 补齐条件 |
|---|---|---|---|
| G-01 | ES8311 播放/录音真机闭环 | 仅 codec/I2S 初始化通过 | 修 B-01/B-02 后，`play 3` + `record 10` 稳定，PCM 质量报告合格 |
| G-02 | 以太网 `eth0` 真机验证 | 代码与 defconfig 就绪 | link → DHCP → DNS → TLS 有串口日志 |
| G-03 | `smart_home`/`smart_home_voice` defconfig 缺 GT911 | `.config` 有、入库 defconfig 无 | 用 `savedefconfig` 回写，干净构建后 `/dev/input0` 存在 |
| G-04 | NuttX 侧改动未固化为 patch | `drivers/input/{Kconfig,Make.defs,CMakeLists}`、`v4l2_cap.c`、`imgdata.h`、`net_procfs.c` 等只在工作树 | 新增 `patches/nuttx/0010+`，`link_nuttx_display_drivers.sh --check` 能校验 |
| G-05 | 源码树残留构建产物 | 534 个 `.o`/`.a`，含 `board/esp32p4/common/libboard.a` | 清理并从交付包排除 |
| G-06 | 语音链路（TTS/ASR/KWS）真机验证 | 仅构建通过 | 随 G-01 一起上板 |
| G-07 | 板级初始化无健康汇总 | 各域失败仅打日志、继续启动 | bringup 输出 `ready_mask` 或等价状态 |

## 三、BUG 清单（B）

### 音频

| ID | 级别 | 位置 | 现象 | 根因 | 建议 |
|---|---|---|---|---|---|
| B-01 | Critical | `chips/esp32p4/common/espressif/esp_i2s.c:2999` | `audio_smoke play` 必失败，返回 `-EFBIG` | 单描述符上限 4032、`I2S_DMADESC_NUM=2` → 8064；`CONFIG_ES8311_BUFFER_SIZE` 默认 8192，越界 | `ES8311_BUFFER_SIZE=4096`（推荐）或 `I2S_DMADESC_NUM=3` |
| B-02 | High | `esp_i2s.c:1500`、`esp_i2s.c:3201` | 录音只写约 4 KB 就"完成" | `priv->streaming` 只能由 `AUDIOIOC_START` 置位，ES8311 从不下发 START/STOP → 每包都带 `AUDIO_APB_FINAL` | `es8311_start/stop` 补 `I2S_IOCTL(START/STOP)` |
| B-03 | High | `esp_i2s.c:548-563` | 潜在死锁 / 空指针 | 持 `spin_lock_irqsave` 时 `kmm_memalign` 且不判返回值 | 描述符改为初始化时一次性分配 |
| B-04 | High | `esp_i2s.c:540`、`nuttx/drivers/audio/es8311.c:1668` | 丢一次 DMA EOF 即永久挂死，STOP 时 `pthread_join` 连带挂住调用者 | `nxsem_wait_uninterruptible` + `bfcontainer->timeout` 从未使用 | 改可超时等待，或加 DMA 看门狗 |
| B-05 | Medium | `esp_i2s.c:1063-1069` | 每发生一次部分入队泄漏一块 DMA 缓冲 | 提前 return，未释放 `bfcontainer->buf` | 统一走 `errout` |
| B-06 | Medium | `board/.../esp32p4_es8311.c:114-149` | 注册失败后 codec/PCM 对象泄漏 | 失败分支只返回，不注销已注册节点 | 增加逐级回滚 |
| B-07 | Medium | `board/.../esp32p4_es8311.c:152` | 上电即开功放，STOP 不静音 | PA 拉高后再无拉低路径 | STOP/close 时拉低 PA |
| B-08 | Medium | `nuttx/drivers/audio/es8311.c:1512-1537` | 错误码统一变成 `-EIO` | `ret \|= writereg()` 按位或负 errno | 首个错误即返回并保留原码 |
| B-09 | Low | `esp_i2s.c:3548-3556` | 每次初始化新建 PM lock | 取到局部变量后未写回 `priv->config->pm_lock` | 写回句柄（当前 `CONFIG_PM` 未开，暂不触发） |
| B-10 | Low | `esp_i2s.c` `i2s_tx_schedule` | 理论空指针解引用 | `while (bfdesc->eof == 0 && bfdesc != NULL)` 先解引用后判空 | 调换判断顺序 |

### 显示 / 触摸

| ID | 级别 | 位置 | 现象 | 建议 |
|---|---|---|---|---|
| B-11 | Medium | `board/.../configs/smart_home/defconfig` | 干净构建无触摸 | 补 `CONFIG_ESP32P4_FUNCTION_EV_BOARD_GT911=y` |
| B-12 | Low | `board/.../esp32p4_fb.c:152-174` | `updatearea` 按整页 clean，脏区小也清 1.2 MB | 性能阶段再按脏矩形优化 |

### 网络

| ID | 级别 | 位置 | 现象 | 建议 |
|---|---|---|---|---|
| B-13 | Low | `board/.../esp32p4_hosted.c:412-416` | SSID/密码经 Kconfig 编进固件 | 量产改走 `/data` 运行时下发（已有 `board_esp_hosted_wifi_connect()`） |
| B-14 | Low | `board/.../esp32p4_ethernet.c` | 仅 `esp_hr_timer_init` + `esp_emac_init`，无 PHY 复位/链路等待 | 上板前补 PHY reset 与 link up 超时 |

## 四、16 kHz 单声道录放验收判定

| 项 | 当前 | 修 B-01/B-02 后应满足 |
|---|---|---|
| 播放 | 必失败（`-EFBIG`） | `play 3` 返回 OK，喇叭有 1 kHz 音调 |
| 录音 | 约 4 KB 后"成功"退出 | `record 10` 输出 ≥ 320000 B，时长误差 < 5% |
| 测试可信度 | `audio_smoke` 打印期望字节数，假成功 | 打印实际写入字节数 |
| 音频质量 | 无数据 | 无削顶、无周期性断裂，底噪与 THD 有报告 |

## 五、建议修复顺序

1. **B-01**（一个 Kconfig 数值）→ 复测播放是否仍卡；若仍卡，再按 B-04 取证。
2. **B-02** + `audio_smoke` 实际字节数 → 录音结果才可信。
3. **B-11 / G-03** → 用已验收 `.config` 回写 defconfig，恢复可复现。
4. **G-04** → 固化 patch，避免 `repo sync` 后回归。
5. **G-05 / G-07 / G-02** → 交付清理与剩余验收。
