/****************************************************************************
 * demos/smart_home/src/voice/smart_home_vad.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "smart_home_vad.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 帧几何：20 ms 帧 / 10 ms 步进（16 kHz）。 */
#define VAD_SAMPLE_RATE        16000
#define VAD_FRAME_SAMPLES      320          /* 20 ms */
#define VAD_HOP_SAMPLES        160          /* 10 ms */
#define VAD_FRAME_GAP          (VAD_FRAME_SAMPLES - VAD_HOP_SAMPLES)

/* 能量归一：sum(x^2) >> VAD_ENERGY_SHIFT，避免 32 位溢出。
 * 320 个满幅样本的平方和约 3.4e11，右移 12 位后约 8.4e7，仍有余量。
 */
#define VAD_ENERGY_SHIFT       12

/* 启动阶段用最小值跟踪噪声底，避免第一帧就是人声导致门限过高。 */
#ifndef CONFIG_SMART_HOME_VAD_INIT_FRAMES
#  define CONFIG_SMART_HOME_VAD_INIT_FRAMES 10
#endif

/* 相对噪声底的能量比（百分比）。400 = 比噪声高 6 dB。 */
#ifndef CONFIG_SMART_HOME_VAD_RATIO_PCT
#  define CONFIG_SMART_HOME_VAD_RATIO_PCT 400
#endif

/* 绝对能量地板：低于此值一律判为非语音（数字静音时噪声底可能趋近 0）。 */
#ifndef CONFIG_SMART_HOME_VAD_MIN_ENERGY
#  define CONFIG_SMART_HOME_VAD_MIN_ENERGY 400
#endif

/* 起始确认帧数 / 结束挂起帧数（每帧步进 10 ms）。 */
#ifndef CONFIG_SMART_HOME_VAD_START_FRAMES
#  define CONFIG_SMART_HOME_VAD_START_FRAMES 3
#endif

#ifndef CONFIG_SMART_HOME_VAD_END_FRAMES
#  define CONFIG_SMART_HOME_VAD_END_FRAMES 30
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct smart_home_vad_state_s
{
  int16_t frame[VAD_FRAME_SAMPLES];
  size_t fill;
  size_t gap;

  uint32_t noise;
  uint32_t energy;
  uint32_t init_frames;

  volatile int enabled;
  volatile int active;
  volatile int energy_snapshot;
  volatile int noise_snapshot;

  int speech;
  int start_count;
  int end_count;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct smart_home_vad_state_s g_vad =
{
  .enabled = 1,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 单帧判决：更新噪声底、判定本帧是否语音，并推进状态机。 */
static smart_home_vad_event_t vad_evaluate(void)
{
  int64_t acc = 0;
  uint32_t energy;
  uint32_t threshold;
  int i;

  for (i = 0; i < VAD_FRAME_SAMPLES; i++)
    {
      int32_t sample = g_vad.frame[i];

      acc += (int64_t)sample * (int64_t)sample;
    }

  energy = (uint32_t)(acc >> VAD_ENERGY_SHIFT);

  /* 噪声底：启动阶段取最小值，之后上升慢（1/64）、下降快（1/4），
   * 既跟随环境变化又不被一句话的峰值拉高。
   */

  if (g_vad.init_frames < CONFIG_SMART_HOME_VAD_INIT_FRAMES)
    {
      if (g_vad.init_frames == 0 || energy < g_vad.noise)
        {
          g_vad.noise = energy;
        }

      g_vad.init_frames++;
    }
  else if (energy < g_vad.noise)
    {
      g_vad.noise -= (g_vad.noise - energy) >> 2;
    }
  else if (!g_vad.speech)
    {
      g_vad.noise += (energy - g_vad.noise) >> 6;
    }

  if (g_vad.noise < 1)
    {
      g_vad.noise = 1;
    }

  threshold = (uint32_t)(((uint64_t)g_vad.noise *
                          CONFIG_SMART_HOME_VAD_RATIO_PCT) / 100u);

  g_vad.energy = energy;
  g_vad.energy_snapshot = (int)energy;
  g_vad.noise_snapshot = (int)g_vad.noise;

  g_vad.speech = (energy > threshold) &&
                 (energy > (uint32_t)CONFIG_SMART_HOME_VAD_MIN_ENERGY);

  if (!g_vad.enabled)
    {
      return SMART_HOME_VAD_EVENT_NONE;
    }

  if (g_vad.speech)
    {
      g_vad.start_count++;
      g_vad.end_count = 0;

      if (!g_vad.active &&
          g_vad.start_count >= CONFIG_SMART_HOME_VAD_START_FRAMES)
        {
          g_vad.active = 1;
          return SMART_HOME_VAD_EVENT_SPEECH_START;
        }
    }
  else
    {
      g_vad.end_count++;
      g_vad.start_count = 0;

      if (g_vad.active &&
          g_vad.end_count >= CONFIG_SMART_HOME_VAD_END_FRAMES)
        {
          g_vad.active = 0;
          return SMART_HOME_VAD_EVENT_SPEECH_END;
        }
    }

  return SMART_HOME_VAD_EVENT_NONE;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void smart_home_vad_reset(void)
{
  g_vad.fill = 0;
  g_vad.gap = 0;
  g_vad.noise = 0;
  g_vad.energy = 0;
  g_vad.init_frames = 0;
  g_vad.active = 0;
  g_vad.speech = 0;
  g_vad.start_count = 0;
  g_vad.end_count = 0;
  g_vad.energy_snapshot = 0;
  g_vad.noise_snapshot = 0;
  memset(g_vad.frame, 0, sizeof(g_vad.frame));
}

void smart_home_vad_set_enabled(int enabled)
{
  g_vad.enabled = enabled != 0;

  if (!enabled)
    {
      /* 关闭时立即回到静音态，避免恢复后被挂起的 end_count 立刻触发。 */
      g_vad.active = 0;
      g_vad.speech = 0;
      g_vad.start_count = 0;
      g_vad.end_count = 0;
    }
}

int smart_home_vad_enabled(void)
{
  return g_vad.enabled;
}

int smart_home_vad_active(void)
{
  return g_vad.active;
}

int smart_home_vad_last_energy(void)
{
  return g_vad.energy_snapshot;
}

int smart_home_vad_noise_floor(void)
{
  return g_vad.noise_snapshot;
}

smart_home_vad_event_t smart_home_vad_process(const int16_t *pcm,
                                              size_t sample_count)
{
  smart_home_vad_event_t event = SMART_HOME_VAD_EVENT_NONE;

  if (pcm == NULL || sample_count == 0)
    {
      return event;
    }

  while (sample_count > 0)
    {
      size_t chunk;

      /* 帧间跳步：上一帧取样点的一半被丢弃，实现 50% 重叠。 */

      if (g_vad.gap > 0)
        {
          chunk = g_vad.gap < sample_count ? g_vad.gap : sample_count;
          pcm += chunk;
          sample_count -= chunk;
          g_vad.gap -= chunk;
          continue;
        }

      chunk = VAD_FRAME_SAMPLES - g_vad.fill;
      if (chunk > sample_count)
        {
          chunk = sample_count;
        }

      memcpy(&g_vad.frame[g_vad.fill], pcm, chunk * sizeof(int16_t));
      g_vad.fill += chunk;
      pcm += chunk;
      sample_count -= chunk;

      if (g_vad.fill == VAD_FRAME_SAMPLES)
        {
          smart_home_vad_event_t produced = vad_evaluate();

          if (produced != SMART_HOME_VAD_EVENT_NONE)
            {
              event = produced;
            }

          g_vad.fill = 0;
          g_vad.gap = VAD_FRAME_GAP;
        }
    }

  return event;
}
