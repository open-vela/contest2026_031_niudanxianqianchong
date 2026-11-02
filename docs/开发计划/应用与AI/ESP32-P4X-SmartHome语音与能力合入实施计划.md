# ESP32-P4X SmartHome 语音与能力合入实施计划

> 版本：v0.2（2026-11-02）
> 代码基线：`contest2026` @ `ceb1c48`
> 状态：实施中；P0 通过，P1 单核 voice 配置拍照失响应待定位
> 范围：ASR、TTS、VAD、KWS、ESP-NN（含 KWS 加速）、agent camera 工具
> 关联文档：[问题分析](../../架构设计/ESP32-P4X-SmartHome问题分析.md)、
> [卡顿线程内存优化方案](../../架构设计/ESP32-P4X-SmartHome卡顿线程内存优化方案.md)、
> [SMP 音频链路排查与修复方案](../ESP32-P4X-SMP音频链路排查与修复方案_20261101.md)

## 1. 背景与目标

本轮目标：按可验证性逐项合入并留下板端证据。代码合入、构建成功、
数据完整和物理听感分别验收，不将尚未验证的配置或组合标为完成。

## 2. 当前进度（2026-11-02，接管后复核）

| 阶段 | 代码与证据 | 验收状态 |
| --- | --- | --- |
| P0 音频 | `d3374a3` 修复 I2S/ES8311；`ee219f6` 同步 voice 缓冲。接管后独立构建、烧录 `smart_home_voice`，10 秒录音 320000 B，两条播放器均完成，固定语音 `hi_openvela.pcm` 也播放完成；用户确认都清楚、没有周期性咔哒 | 通过 |
| P1 camera | `f06d4ca`、`e04efab` 已进主线；历史 SMP 自测通过、4 次拍照各 307200 B。本轮单核 voice 首次 shot 启动 CSI 后无结果，随后串口 `ps` 不响应 | 未通过；保留现场，等待 JTAG USB 权限读取 |
| P2 VAD | `f0adbc2`、`ee219f6` 已进主线；本轮宿主四场景重新通过。历史板测固定录音 5.9 秒，VAD 三次为 1.6/3.8/5.9 秒 | 部分验证；固定话术 3/3、首尾字完整仍待测 |
| P3 KWS | `ceb1c48` 新增 smoke 配置；首次 reference Invoke 超过 230 秒未返回。真实 TFLM 宿主 ASan 首次 Invoke 已复现局部 resolver 的 stack-use-after-return；smoke 还缺 arena 释放前的解释器销毁 | 宿主已证实生命周期缺陷；修复候选通过 10 次销毁重建和 200 次一致输出，待板测确认与计时，不归为单纯算力不足 |
| P4 ESP-NN | 接管前已有未提交包和 apps 接线实验；出现过 DW scratch 断言，没有完整推理性能结果 | 实验中；正确性、真实命中及两态性能验收均未完成 |
| P5 云端 | ASR/TTS 在主线；历史日志取得 IP 后 DNS 校验失败 | 历史网络阻塞；本轮未复测服务域名，无 2/3 成功证据 |
| P6 SMP | `smart_home` 双核音频顺序录放已通过用户听感验收 | 基础通过；加入 KWS 的双核组合仍待验 |

本轮证据目录：`out/voice-gates-20261102/`（相对外层 openvela 根）。
`flash-plan-voice.json`、`voice.config`、`voice.bin` 和 `serial-v3-events.jsonl`
记录镜像与板端命令；`p0-voice-stats.json` 记录导出 PCM 的测量结果。

P0 镜像 SHA-256：
`1e827ab1537be37773d2fa3a0e4e91f1789a55b853db916e11838d4da041b37d`。
本次只写应用 `0x2000`，16MB/DIO/80MHz，保留 `/data`。
麦克风 PCM 满幅削波为 0%，零值约 0.529%，奇偶 RMS 约 96.67/95.34；
听感由用户确认，不以这些单次测量代替所有环境下的质量保证。

接管前主检出的三项 KWS 改动和未跟踪 `packages/esp-nn/` 已完整保存在
命名 stash `c5da9b035b2c2b82fa23980bbf8b28f1e1b593f8`，另有补丁和包归档；
没有丢弃实验。外层 apps 的既有改动保留，本轮 P0 未启用 ESP-NN。

构建隔离：脚本接受 worktree 配置路径，并不自动改写配置里的 chip/board 路径
或 `packages/demos/smart_home` 符号链接。本轮使用临时配置指向 worktree，
构建期间切换应用链接并在结束后恢复。不得仅凭 wrapper 路径认定源代码隔离。

详细过程见 [本轮验收记录](../../开发日志/ESP32-P4X-语音能力分阶段验收记录_20261102.md)。

## 3. 已定决策

1. 音频基线使用已合入并验收的 `d3374a3`；它已选择性复用音频分支中的修复，
   不再把是否整支合入 `2119ab6` 当作 P0 的前置决策。
2. KWS 新建 `smart_home_kws` 配置（`#include "../smart_home_voice/defconfig"`），
   `smart_home_voice` 保持为 ASR/TTS 的稳定镜像，故障可二分。
3. ESP-NN 的验收目标是**真正加速 KWS 推理**，不接受"只导入包"。
4. SMP 的音频 + KWS 组合在最后单独复验；已有双核音频证据不重复称为首次启用。

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
KWS 引入 C++/TFLM 且历史上有启动冻结；ESP-NN 的 P4 vendor 内核涉及可选工具链与 TFLM 接线，影响面较大；
SMP 与音频的组合风险最高。

## 5. 分阶段实施

### P0 音频闸门（不改代码）

| 项 | 内容 |
| --- | --- |
| 动作 | 构建并烧录 `smart_home_voice`；`audio_smoke play 3`；`audio_smoke record 10 /data/base.pcm`；`tts_smoke play /data/base.pcm 16000` |
| 通过标准 | 出声；10 秒文件恰为 320000 B 且可辨人声；播放完整、无周期咔哒 |
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
| 音频 | `audio_smoke play 3` / `record 10` | 出声；10 秒 320000 B；无咔哒 | 无降级 |
| camera | agent "拍照" | 307200 B、seq 递增、20 次不 OOM | 只验注册+preview |
| ASR | `asr_smoke record/recognize 4` | 120–135 KB；2/3 命中 | 无 DNS 只验 record |
| TTS | `tts_smoke play/speak` | 10±0.5 s；2/3 成功 | 无 DNS 只验 play |
| KWS | `kws_smoke --warmup 20 --repeat 100` | 100/100；P95 ≤ 20 ms | 无麦克风只跑 smoke |
| VAD | 宿主 selftest + 板端 A/B | 1.3–1.9 s 断句；3/3 不丢首字 | 只跑宿主自测 |
| ESP-NN | 同一 smoke 开关两态各 3 次 | P95 改善 ≥15%，分类一致 | 仅宿主探针 |

## 7. 风险登记

| 风险 | 触发条件 | 处置 |
| --- | --- | --- |
| ESP-NN 补丁再次不可重放 | 分支 0012 与主线 add/add | 不引入分支 0012；在干净 nuttx 基线按序 `git apply --check` 0001–0014 留证 |
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
