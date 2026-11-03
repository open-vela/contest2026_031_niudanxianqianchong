/****************************************************************************
 * demos/smart_home/src/voice/smart_home_vad.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/* 采集链路上的能量型 VAD（语音活动检测）。
 *
 * 不依赖模型、不依赖浮点：整条判决用整数运算，适配 P4 当前"单核 400 MHz +
 * 软浮点"的构建（无 CONFIG_ARCH_FPU）。作为 voice_capture 分发路径的一部分
 * 运行，先于消费者回调，因此 ASR 让位给 KWS 时 VAD 依然有效。
 *
 * 算法：20 ms 帧 / 10 ms 步进的短时能量，配慢速自适应噪声底与双门限
 * （相对噪声比 + 绝对能量地板），再经"连续 N 帧起始 + M 帧挂起"状态机
 * 输出 SPEECH_START / SPEECH_END 事件。
 *
 * 线程模型：smart_home_vad_process() 只允许在采集 worker 中调用（单线程）；
 * 其余查询接口可从任意线程读取（volatile 快照，允许一帧的滞后）。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
  SMART_HOME_VAD_EVENT_NONE = 0,
  SMART_HOME_VAD_EVENT_SPEECH_START,
  SMART_HOME_VAD_EVENT_SPEECH_END
} smart_home_vad_event_t;

/* 复位到初始状态（噪声底重新估计，状态机回到静音）。 */
void smart_home_vad_reset(void);

/* 运行期开关：关闭时 process() 仍消费样本但不产生事件。 */
void smart_home_vad_set_enabled(int enabled);
int  smart_home_vad_enabled(void);

/* 当前是否处于"检测到人声"状态（含挂起期）。 */
int  smart_home_vad_active(void);

/* 最近一帧的能量（Q: 平方和 >> 10），仅用于诊断打印。 */
int  smart_home_vad_last_energy(void);

/* 当前噪声底估计（同上量纲），仅用于诊断打印。 */
int  smart_home_vad_noise_floor(void);

/* 喂入任意长度的 PCM16 单声道 16 kHz 采样，内部自行拼帧/跳步。
 * 返回本次调用中产生的最后一个事件（无事件返回 NONE）。 */
smart_home_vad_event_t smart_home_vad_process(const int16_t *pcm,
                                              size_t sample_count);

#ifdef __cplusplus
}
#endif
