/****************************************************************************
 * demos/smart_home/src/voice/smart_home_voice_capture.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/* 统一录音采集服务：独占持有 /dev/audio/pcm_in0。
 *
 * ASR 录音会话与 KWS 常驻监听共用这一个录音通道，通过"单活跃消费者"
 * 让位协议协作：后注册的 listen 回调替换前一个；会话结束后由调用方
 * 重新注册（KWS 在 ASR 结束后恢复监听）。
 *
 * 回调运行在采集 worker 线程中，只允许轻量操作（memcpy/入队）；
 * 需要重处理（KWS 推理）的消费者必须把帧转入自己的线程。
 *
 * 线程安全：所有接口可在任意线程调用。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 每帧回调：samples 为 PCM16 little-endian 单声道样本（16 kHz），
 * sample_count 为样本数（字节数的一半）。 */
typedef void (*voice_capture_frame_cb_t)(const int16_t *samples,
                                         size_t sample_count,
                                         void *user_data);

/* 启动采集服务（打开设备、拉起 worker）。幂等：已启动返回 0。 */
int voice_capture_start(void);

/* 停止服务并释放设备与线程资源。 */
void voice_capture_deinit(void);

/* 注册当前消费者（替换已有回调）。cb 为 NULL 等同于清空。 */
int voice_capture_listen(voice_capture_frame_cb_t cb, void *user_data);

/* 注销消费者：仅当当前回调与 cb 一致时清空。 */
int voice_capture_stop_listening(voice_capture_frame_cb_t cb);

/* 阻塞式录制一段音频（便捷封装，ASR 会话与 asr_smoke 共用）。
 *
 * seconds:  最长录制秒数（提前 abort 时更短）
 * abort:    可为 NULL；指向的标志置非 0 时尽快结束
 * out_pcm:  成功时输出 bulk 堆缓冲（PCM16 单声道 16 kHz），
 *           调用方 smart_home_bulk_free 释放
 * out_bytes: 输出字节数
 *
 * 返回 0 成功；录音期间服务与消费者注册状态会被临时占用并恢复。
 */
int voice_capture_record(int seconds, const volatile int *abort,
                         uint8_t **out_pcm, size_t *out_bytes);

/* 带 VAD 自动断句的录制（CONFIG_SMART_HOME_VAD 关闭时等价于上者）。
 *
 * 检测到人声之后，若连续 silence_ms 毫秒处于非语音状态，则提前结束录音，
 * 用于"唤醒后说完自动发送"。未检测到任何人声时不提前结束，只受 seconds
 * 上限约束，避免把整段录音裁成空。
 */
int voice_capture_record_vad(int seconds, const volatile int *abort,
                             int silence_ms,
                             uint8_t **out_pcm, size_t *out_bytes);

/* 当前 VAD 是否认为有人在说话（未编入 VAD 时恒返回 0）。 */
int voice_capture_speech_active(void);

#ifdef __cplusplus
}
#endif
