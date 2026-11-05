# SmartHome 一次性提醒实现与验证

日期：2026-11-04。基线：`0f64854`（开发期间已同步最新设备来源区分提交）。工作树：`worktrees/smarthome-alarms-codex`，分支：`feat/smarthome-alarms-codex`。不合并主分支；固件构建、烧录和实板验收由用户执行。

## 功能与接口

| 工具 | 参数 | 行为 |
| --- | --- | --- |
| `set_timer` | `delay_seconds`、`message`、可选 `name` | 倒计时提醒，1 秒至 366 天 |
| `set_alarm` | `at_local`、`message`、可选 `name` | 指定日期时间的一次性闹钟 |
| `list_timers` | 无 | 当前北京时间、时钟有效性、全部任务和状态 |
| `cancel_timer` | `id` | 持久化删除倒计时或闹钟，包括已触发提醒 |

`at_local` 格式为 `YYYY-MM-DDTHH:MM:SS`，固定北京时间 UTC+08:00，与现有 UI 一致。最多 8 个未删除任务；名称最多 31 字节，消息最多 127 字节。通过聊天创建、查看、取消；首版没有手动编辑闹钟页面。

创建和取消只有文件保存成功才返回成功。到期显示弹窗，播放 5 秒本地提示音；点击“知道了”立即停止声音，后台保存确认后关闭弹窗。保存失败保留弹窗并允许重试。

## 修改内容

| 文件或模块 | 修改 |
| --- | --- |
| `src/reminder/smart_home_reminder.c/.h` | 单个调度线程、命令串行处理、缓存快照、持久化与恢复 |
| `src/reminder/smart_home_reminder_tools.c/.h` | 替换旧计时器工具；增加闹钟及运行期提醒策略，避免旧技能文档误导模型 |
| `src/tools/smart_home_tools.c` | 按配置选择新服务或旧演示工具 |
| `src/ui/lvgl/smart_home_lvgl_reminder.c` | LVGL 线程轮询缓存、跨页面弹窗、异步确认和响铃错误提示 |
| `src/voice/smart_home_voice_play.c/.h` | 本地 PCM 提示音、与 TTS 共用有界队列、按提醒 ID 停止 |
| `src/app/smart_home_main.c`、LVGL 主入口 | 服务启停和弹窗清理 |
| Kconfig、Makefile、CMakeLists.txt | 新增提醒及本地响铃配置；TTS 关闭时也能响铃 |
| `res/skills/smart_home_timer.md`、`smart_home_safety.md` | 工具用法和能力边界 |
| `configs/smart_home_alarms/defconfig` | 基于已有 `smart_home` 的独立验证配置，保留现有双核及 ES8311 音频配置 |
| `scripts/build_smart_home_alarms.sh` | 临时选择本工作树应用/Agent 源码，成功或失败退出后恢复共享软链接 |

任务文件：`/data/smart_home/reminders.json`，版本 1。保存复用配置模块的同目录临时文件、完整写入、`fsync` 和 `rename`；损坏或未知版本禁用提醒服务并保留原文件，不自动覆盖。仅调度线程访问任务文件，UI 只读取缓存并投递确认。

运行中倒计时使用 `CLOCK_MONOTONIC`；校时后更新持久化截止时间。重启后按墙上时钟恢复，因此新建任务要求北京时间在 2024–2037 范围内（兼容当前 32 位 `time_t` 配置）；范围检查不能证明时间准确，需核对 `list_timers.now_local`。启动日期仍为 2019 时先校时。恢复的未来任务等待有效时钟，不按错误启动日期提前触发。

未触发任务重启后迟到不超过 5 分钟则补提醒，超过 5 分钟显示“已错过”并静音。已经落盘为到期、但未确认的任务重启后恢复静音弹窗。程序在保存到期与开始声音之间掉电，也采用静音恢复；不保证掉电期间仍能响铃。任务确认或取消后删除，其余状态占用任务名额。

本地提示音不访问网络或模型。若 TTS 请求正在进行，提示音优先排队并取消旧语音播放，仍须等待该网络请求返回。提示音只在对应弹窗首次展示时播放；多个到期任务逐个确认。

不支持关机唤醒、应用退出后的通知、重复日程或定时设备控制。`smart_home "一句话"` 执行完会退出；验收到期通知请无参数启动 UI。

## 用户构建和验收

不要同时进行其他固件构建：底层仍共用 OpenVela 的 NuttX 构建目录。专用入口临时切换 `packages/demos/smart_home`、`packages/cAGENT`、`packages/cagent_addons` 三个软链接，不切换或合并主分支。

```bash
cd /home/arongw/openvela/worktrees/smarthome-alarms-codex
scripts/build_smart_home_alarms.sh -j8
```

构建后核对实际配置及产物：

```bash
cd /home/arongw/openvela
rg '^CONFIG_(BASE_DEFCONFIG|SMART_HOME_REMINDERS|SMART_HOME_REMINDER_RING|ESPRESSIF_SIMPLE_BOOT|ESPRESSIF_FLASH_)' nuttx/.config
ls -lh nuttx/nuttx.bin
```

应包含 `CONFIG_SMART_HOME_REMINDERS=y`、`CONFIG_SMART_HOME_REMINDER_RING=y`。烧录使用这次构建生成的 Flash 布局；确认 P4 Simple Boot、16 MiB、DIO/80 MHz 和 `0x2000` 地址后，固件命令为：

```bash
cd /home/arongw/openvela
source myenv/bin/activate
esptool --chip esp32p4 --port /dev/ttyACM1 --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m 0x2000 nuttx/nuttx.bin
```

串口设备以实际枚举为准。保留现有 LittleFS，验证重启持久化时不要重刷数据分区。运行期策略已覆盖旧计时器说明，验证本功能无需重刷整个资源分区；更新技能文档可随后按文件部署。

NSH 启动：`smart_home`。串口应出现 `[reminder] start ready=1`，创建后出现 `created id=...`，触发后出现 `due id=... state=2`。

| 验收 | 操作与预期 |
| --- | --- |
| 倒计时 | 聊天输入“10 秒后提醒我喝水”；到期出现弹窗和本地提示音 |
| 一次性闹钟 | 输入“查看当前时间和提醒”，核对北京时间；设置明确日期、两分钟后时间的闹钟，到点触发 |
| 重启恢复 | 创建两分钟提醒，重启并重新启动 `smart_home`；校时后按原截止时间触发 |
| 确认持久化 | 点击“知道了”，重启后该提醒不再出现 |
| 取消 | 创建提醒，按工具返回 ID 取消；重启后不再触发 |
| 多任务 | 同时创建两条提醒，分别确认，第二条随后展示 |
| 过期策略 | 保存任务后停机超过 5 分钟，启动后静音显示已错过；已触发未确认任务重启也静音 |
| 离线响铃 | 在线创建后断开网络，应用保持运行；到期仍弹窗和响铃 |
| 页面切换 | 在首页、聊天、设置及屏保等待到期；弹窗跨页面显示，确认可触摸 |

失败时保留完整构建日志、实际 `.config`、启动至失败的串口日志，以及是否出现弹窗/声音/确认成功。

## 已执行检查

主机 ASan/UBSan：调度与恢复、真实工具回调、本地音频队列及 TTS 队列；另有构建链接恢复测试。真实 LVGL 回归覆盖弹窗、跨页面显示、多个任务、确认保存失败重试和清理。固件编译、实际 LittleFS 断电行为、扬声器、物理触摸和模型对话仍待用户验证。

```bash
cmake -S demos/smart_home/tests/reminder -B /tmp/smarthome-alarms-core -DOPENVELA_ROOT=/home/arongw/openvela
cmake --build /tmp/smarthome-alarms-core -j8
ctest --test-dir /tmp/smarthome-alarms-core --output-on-failure
cmake -S demos/smart_home/tests/lvgl -B /tmp/smarthome-alarms-lvgl -DOPENVELA_ROOT=/home/arongw/openvela
cmake --build /tmp/smarthome-alarms-lvgl -j8
ctest --test-dir /tmp/smarthome-alarms-lvgl --output-on-failure
```
