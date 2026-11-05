/****************************************************************************
 * demos/smart_home/src/voice/smart_home_voice_play.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/* 语音播报服务：合成 + 播放的队列化封装。
 *
 * 对外接口与 src/voice/smart_home_voice_stub.h 预留的签名一致
 * （voice_play_text / voice_play_stop），真实实现替换桩。调用方
 * （聊天 UI）只需在 LVGL 线程投递文本；合成与播放发生在独立低优先级
 * worker 中，绝不阻塞 UI 线程。
 *
 * "自动播报"开关（voice.json enabled）由调用方查询
 * voice_play_announce_enabled()；本服务自身不拦截该开关，便于
 * tts_smoke 等入口在开关关闭时仍能手动播报。
 */

#pragma once
#include "smart_home_voice_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 播报状态，供 UI 展示。 */
typedef enum
{
  VOICE_PLAY_IDLE = 0,
  VOICE_PLAY_SYNTHESIZING,
  VOICE_PLAY_PLAYING,
} voice_play_state_t;

/* 初始化播报服务（加载配置、启动 worker）。失败返回负值。
 * smart_home 启动时调用一次；重复调用返回 0。 */
int voice_play_init(void);

/* 停止服务并释放线程资源（smart_home 退出时调用）。 */
void voice_play_deinit(void);

/* 投递一段文本进行合成播报（异步）。文本会被复制，调用方缓冲可立即
 * 复用。返回 0 表示已入队；-ENOMEM 内存不足；-EAGAIN 服务未初始化。 */
int voice_play_text(const char *text);

/* 停止当前语音播报并清空文本队列，保留提醒响铃。 */
void voice_play_stop(void);

/* Local alarm jobs share the PCM owner with speech. Alarm jobs have priority;
 * a cloud request already in progress must return before the tone can start. */
int voice_play_alarm(int id);
void voice_play_alarm_stop(int id);
int voice_play_alarm_error(int id);

/* 当前状态。 */
voice_play_state_t voice_play_state(void);

/* 自动播报开关（voice.json enabled 的运行期快照）。 */
int voice_play_announce_enabled(void);

/* Consume the last unreported synthesis/playback error on the UI thread. */
int voice_play_take_error(smart_home_voice_result_t *result);

#ifdef __cplusplus
}
#endif
