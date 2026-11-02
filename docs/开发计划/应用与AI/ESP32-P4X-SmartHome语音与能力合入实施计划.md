# ESP32-P4X SmartHome 语音与能力合入实施计划

> 版本：v0.1（2026-11-02）
> 代码基线：`contest2026` @ `4d46aab`
> 状态：计划，未开始实施
> 范围：ASR、TTS、VAD、KWS、ESP-NN（含 KWS 加速）、agent camera 工具
> 关联文档：[问题分析](../../架构设计/ESP32-P4X-SmartHome问题分析.md)、
> [卡顿线程内存优化方案](../../架构设计/ESP32-P4X-SmartHome卡顿线程内存优化方案.md)、
> [SMP 音频链路排查与修复方案](../ESP32-P4X-SMP音频链路排查与修复方案_20261101.md)

## 1. 背景与目标

主线上 ASR/TTS 代码已经合入，但**从未真机闭环**；KWS 代码完整却没有任何配置启用；
VAD 与 ESP-NN 只在分支上；camera 工具做完后按当时要求未合并。

本轮目标：按"可验证性"从高到低依次合入，并在板子上逐项留下证据
（串口日志 + `ls -l` + `ps`），不把"没验过"写成"已完成"。
SMP 放到最后一轮单独验证，避免"首次 SMP"与"首次音频/KWS"叠加导致无法归因。

## 2. 现状（已核实）

| 项 | 主线状态 | 缺口 |
| --- | --- | --- |
| ASR | `smart_home_voice` 已开 `VOICE_ASR`+`ASR_SMOKE` | 无真机 record/recognize 记录 |
| TTS | `smart_home_voice` 已开 `VOICE_TTS`+`TTS_SMOKE` | 无真机 play/speak 记录 |
| 音频数据面 | 已修 `aff9b2c`(缓冲 4096)、`48d9556`(RX EOF)、`5240257`(TX carry) | 三项均未上板验收；`2119ab6`(录音卡死/咔哒) 仍在分支 |
| KWS | 服务/前端/模型数组在主线（模型编译进固件） | 无 defconfig 启用；前端未与训练管线对拍 |
| VAD | 无 | 分支 `feat/p4x-vad` 2 个提交，无 NSH 命令 |
| ESP-NN | 无 `packages/esp-nn`；主线已有 `patches/nuttx/0012` | 分支的 0012 与主线冲突；TFLM 的 `TFLITEMICRO_ESP_NN` 只允许 ESP32S3 |
| camera 工具 | 无 | 分支 1 个提交，需导出 `smart_home_tool_register_local` |

环境事实：板子 `/dev/ttyACM1`；现场 DNS 不可达（`api.deepseek.com` 解析失败），
云端用例会受影响；`board/esp32p4/common/board` 是**指向主检出的绝对路径符号链接**，
worktree 内构建会读主检出的 board 源码，worktree 里不要改 board 文件。

## 3. 已定决策

1. `2119ab6` 本轮不合，只用主线现有音频修复；P0 不达标再回头评估。
2. KWS 新建 `smart_home_kws` 配置（`#include "../smart_home_voice/defconfig"`），
   `smart_home_voice` 保持为 ASR/TTS 的稳定镜像，故障可二分。
3. ESP-NN 的验收目标是**真正加速 KWS 推理**，不接受"只导入包"。
4. SMP 最后一轮单独做（音频 + KWS 复验）。

## 4. 总体顺序与依赖

```text
P0 音频闸门（只验证）
   └─ P1 camera 工具（独立，先合）
        └─ P2 VAD（依赖采集链路稳定）
             └─ P3 KWS（配置 + 前端对齐）
                  └─ P4 ESP-NN 接线并加速 KWS
                       ├─ P5 ASR/TTS 云端闭环（依赖网络）
                       └─ P6 SMP 复验（最后，一次一个变量）
```

排序理由：camera 完全本地、无外部依赖；VAD 只挂在采集分发路径；
KWS 引入 C++/TFLM 且历史上有启动冻结；ESP-NN 改平台工具链，影响面最大；
SMP 与音频的组合风险最高。

## 5. 分阶段实施

### P0 音频闸门（不改代码）

| 项 | 内容 |
| --- | --- |
| 动作 | 构建并烧录 `smart_home_voice`；`audio_smoke play 3`；`audio_smoke record 10 /data/base.pcm`；`tts_smoke play /data/base.pcm 16000` |
| 通过标准 | 出声；文件 300–330 KB 且可辨人声；播放完整、无周期咔哒 |
| 失败处置 | 停在此处，记录现象与日志；再决定是否引入 `2119ab6` |
| 回退 | 无代码变更 |

### P1 camera 工具

| 项 | 内容 |
| --- | --- |
| 动作 | `worktrees/camera-tool-codebuddy` 内 rebase 到主线 → 主检出 `merge --ff-only`；提交前打 tag `pre-camera-tool` |
| 产出 | `tools/smart_home_camera_tools.[ch]`、`camera/smart_home_snapshot_store.[ch]`、`SMART_HOME_CAMERA_TOOL` 开关、导出 `smart_home_tool_register_local()` |
| 板端验收 | agent 说"拍照"；`shot-0.rgb565` 恰为 307200 B；`latest.json` seq 递增；连拍 20 次不 OOM |
| 无网络降级 | 只验工具注册 + preview 出图 + 本地落盘 |
| 回退 | `git revert` 该提交 |

### P2 VAD

| 项 | 内容 |
| --- | --- |
| 动作 | rebase `b900f41`+`ce3b17f`，压成一个 `feat(vad)` 提交；保留 `voice_capture_record()`，新增 `voice_capture_record_vad()` |
| 宿主验收 | `tests/vad/vad_selftest` 四场景全过 |
| 板端验收 | 关闭 VAD 时 `asr_smoke record 6` ≈ 6.0 s；开启后说 1 s 再静音应在 1.3–1.9 s 结束，3/3 不丢首字 |
| 前提 | P0 采集吞吐达标，否则断句与采集问题无法区分 |
| 回退 | revert VAD 提交 |

### P3 KWS

| 项 | 内容 |
| --- | --- |
| 3a 配置 | 新建 `configs/smart_home_kws/defconfig`：include voice，追加 `TFLITEMICRO`/`FLATBUFFERS`/`GEMMLOWP`/`KISSFFT`/`RUY`/`HAVE_CXX`/`LIBCXX`/`TLS_NELEM=4`/`TLS_TASK_NELEM=4`/`SMART_HOME_KWS_SMOKE` |
| 3a 验收 | `kws_smoke --warmup 20 --repeat 100` 100/100、P95 ≤ 20 ms；打印 arena 地址确认在 PSRAM |
| 3b 前端 | 先 host A/B（同一组 wav 跑新旧前端比对特征/分数），再合 `ee274d7` |
| 3b 验收 | 开 `SMART_HOME_KWS` 后 10 次冷启动不冻结；1.5 m 内 8/10 唤醒；10 min 静音 ≤ 1 次误唤醒 |
| 回退 | 保持 `SMART_HOME_KWS=n`，只保留 smoke 与前端对齐 |

### P4 ESP-NN 接线并加速 KWS

| 项 | 内容 |
| --- | --- |
| 补丁裁决 | 丢弃分支 `0012`（给所有 P4 强制 `xespv`、缺 CMake），保留主线版本；`patches/nuttx/README.md` 取主线 |
| 包导入 | `packages/esp-nn`（`upstream/` 布局）纳入构建，或改 `apps/mlearning/tflite-micro/Makefile` 的 `ESP_NN_DIR` 指向它；P4 内核源必须进编译 |
| 开关 | `TFLITEMICRO_ESP_NN` 依赖从 `ARCH_CHIP_ESP32S3` 扩为 `S3 || P4` |
| 接线验证 | 确认 KWS 模型算子表含 Conv2D/DepthwiseConv2D；smoke 里打点统计 esp-nn 内核命中次数（0 命中视为未完成） |
| 性能验收 | `kws_smoke --repeat 100` 在开关 `n/y` 下各 3 次：P95 改善 ≥ 15%，5 类概率逐次一致 |
| 回退 | 接线单独一个提交；`ESP_NN_P4` 默认关 |

### P5 ASR/TTS 云端闭环

| 项 | 内容 |
| --- | --- |
| 预检 | `ping` / DNS 解析；不通则云端记 BLOCKED |
| ASR | `asr_smoke record 4` → 120–135 KB；`asr_smoke recognize 4` 2/3 次命中关键短语，失败 < 30 s 不挂死 |
| TTS | `tts_smoke speak 你好，我是智能家居管家` 2/3 次成功；合成+播放 10 s 内无咔哒 |

### P6 SMP 复验（最后一轮）

| 项 | 内容 |
| --- | --- |
| 动作 | `smp_smart_home` 基座 + 音频，一次只加一个变量：先进 NSH → 再 record → 再 kws_smoke |
| 记录 | CPU0/CPU1 负载、线程分布、是否冻结 |
| 回退 | 任一冻结先回退配置并保留日志 |

## 6. 测试矩阵

| 项 | 命令 | 通过标准 | 外部条件缺失 |
| --- | --- | --- | --- |
| 音频 | `audio_smoke play 3` / `record 10` | 出声；300–330 KB；无咔哒 | 无降级 |
| camera | agent "拍照" | 307200 B、seq 递增、20 次不 OOM | 只验注册+preview |
| ASR | `asr_smoke record/recognize 4` | 120–135 KB；2/3 命中 | 无 DNS 只验 record |
| TTS | `tts_smoke play/speak` | 10±0.5 s；2/3 成功 | 无 DNS 只验 play |
| KWS | `kws_smoke --warmup 20 --repeat 100` | 100/100；P95 ≤ 20 ms | 无麦克风只跑 smoke |
| VAD | 宿主 selftest + 板端 A/B | 1.3–1.9 s 断句；3/3 不丢首字 | 只跑宿主自测 |
| ESP-NN | 同一 smoke 开关两态各 3 次 | P95 改善 ≥15%，分类一致 | 仅宿主探针 |

## 7. 风险登记

| 风险 | 触发条件 | 处置 |
| --- | --- | --- |
| ESP-NN 补丁再次不可重放 | 分支 0012 与主线 add/add | 不引入分支 0012；在干净 nuttx 基线按序 `git apply --check` 0001–0013 留证 |
| ESP-NN 内核未被调用 | 模型算子不含 Conv2D/DW 或接线遗漏 | smoke 打点计数；0 命中即未完成 |
| KWS 唤醒率无法解释 | 前端与训练域仍有差异 | 先 host A/B 与模型 SHA 比对，禁止先调阈值 |
| VAD 与采集问题混淆 | 采集吞吐不足时提前断句 | 先过 P0 闸门；开/关必须成对测量 |
| SMP+音频进不去 NSH | 首次把 SMP、音频、KWS 一起开 | 本轮不叠加；P6 一次一个变量 |
| DNS 不可用 | 云端 ASR/TTS 全部失败 | 记 BLOCKED，本地子集照跑，不写成代码缺陷 |
| worktree 读到主检出 board 源码 | 绝对路径符号链接 | worktree 内不改 board；构建前确认主检出干净 |

## 8. 命令速查

```bash
# 构建
./scripts/build_p4x.sh smart_home_voice -j8
./scripts/build_p4x.sh smart_home_kws   -j8

# 烧录
esptool --chip esp32p4 --port /dev/ttyACM1 --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m 0x2000 nuttx/nuttx.bin

# 串口（带时间戳采集）
python3 /tmp/fbtime.py 60 /tmp/run.log "smart_home &"
```

每阶段结束在 `docs/开发日志/` 记一页：通过/失败/阻塞 + 原始日志；
全部完成后更新问题分析文档里对应的 P1/P2 条目状态。
