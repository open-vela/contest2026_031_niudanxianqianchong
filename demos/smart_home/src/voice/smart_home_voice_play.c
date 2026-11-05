/****************************************************************************
 * demos/smart_home/src/voice/smart_home_voice_play.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "smart_home_voice_play.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../smart_home_memory.h"
#ifdef CONFIG_SMART_HOME_VOICE_TTS
#include "smart_home_tts.h"
#endif
#include "smart_home_voice_player.h"

#ifdef __NuttX__
#include <nuttx/irq.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_SMART_HOME_VOICE_TEXT_MAX
#define CONFIG_SMART_HOME_VOICE_TEXT_MAX 512
#endif

/* worker 栈走 bulk（PSRAM）堆，与 agent worker 同策略；mbedTLS 合成
 * 需要 32 KiB 量级。 */
#ifndef CONFIG_SMART_HOME_VOICE_WORKER_STACKSIZE
#define CONFIG_SMART_HOME_VOICE_WORKER_STACKSIZE 32768
#endif

/* NuttX 数字越大优先级越高：后台播报须低于 UI 主任务(100)，
 * 避免合成/播放抢占界面渲染。 */
#define VOICE_WORKER_PRIORITY 90

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef struct voice_job_s
{
  struct voice_job_s *next;
  int alarm_id;
  char text[CONFIG_SMART_HOME_VOICE_TEXT_MAX];
} voice_job_t;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_voice_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_voice_cond = PTHREAD_COND_INITIALIZER;
static voice_job_t *g_voice_queue;
static voice_job_t *g_voice_queue_tail;
static voice_play_state_t g_voice_state = VOICE_PLAY_IDLE;
static smart_home_voice_abort_t g_voice_abort;
static pthread_t g_voice_worker;
static void *g_voice_worker_stack_alloc;   /* bulk 原始指针（释放用） */
static void *g_voice_worker_stack;         /* 16B 对齐后交给 pthread */
static int g_voice_active_alarm;
static int g_voice_alarm_error_id;
static int g_voice_alarm_error;
static bool g_voice_started;
static bool g_voice_stop_requested;
static bool g_voice_error_pending;
static smart_home_voice_result_t g_voice_last_error;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Five seconds of local PCM; no file, model or network dependency. */
static int voice_alarm_tone(void)
{
  static const int16_t wave[32] = {
    0,312,612,889,1131,1330,1478,1569,1600,1569,1478,1330,1131,889,612,312,
    0,-312,-612,-889,-1131,-1330,-1478,-1569,-1600,-1569,-1478,-1330,-1131,-889,-612,-312
  };
  const size_t samples = 16000u * 5u;
  uint8_t *pcm = smart_home_bulk_alloc(samples * 2u);
  size_t i;
  int ret;
  if (!pcm) return -ENOMEM;
  for (i = 0; i < samples; ++i) {
    size_t phase = i % 8000u;
    int value = phase < 3200u ? wave[i % 32u] : 0;
    if (phase < 64u) value = value * (int)phase / 64;
    if (phase >= 3136u && phase < 3200u) value = value * (int)(3200u-phase) / 64;
    pcm[i*2u] = (uint8_t)value;
    pcm[i*2u+1u] = (uint8_t)((uint16_t)value >> 8);
  }
  ret = smart_home_voice_player_play_mem(pcm,samples*2u,16000,&g_voice_abort);
  smart_home_bulk_free(pcm);
  return ret;
}

static int voice_queue_count(void)
{
  int count = 0;
  voice_job_t *job;
  for (job = g_voice_queue; job; job = job->next) ++count;
  return count;
}

static void *voice_worker_main(void *arg)
{
  voice_job_t *job;
#ifdef CONFIG_SMART_HOME_VOICE_TTS
  uint8_t *pcm;
  size_t bytes;
  uint32_t rate;
#endif
  smart_home_voice_result_t result;
  int ret;

  (void)arg;
  pthread_mutex_lock(&g_voice_mutex);
  while (!g_voice_stop_requested)
    {
      while (g_voice_queue == NULL && !g_voice_stop_requested)
        {
          pthread_cond_wait(&g_voice_cond, &g_voice_mutex);
        }

      if (g_voice_stop_requested)
        {
          break;
        }

      job = g_voice_queue;
      g_voice_queue = job->next;
      if (g_voice_queue == NULL)
        {
          g_voice_queue_tail = NULL;
        }

      g_voice_active_alarm = job->alarm_id;
      g_voice_state = job->alarm_id ? VOICE_PLAY_PLAYING : VOICE_PLAY_SYNTHESIZING;
      g_voice_abort.abort_flag = 0;
      pthread_mutex_unlock(&g_voice_mutex);

#ifdef CONFIG_SMART_HOME_VOICE_TTS
      pcm = NULL;
      bytes = 0;
      rate = 0;
#endif
      memset(&result,0,sizeof(result));
      if (job->alarm_id) {
          ret = voice_alarm_tone();
          result.code = ret;
          result.stage = SMART_HOME_VOICE_PLAYBACK;
      } else {
#ifdef CONFIG_SMART_HOME_VOICE_TTS
      printf("[voice] job: text_len=%zu\n", strlen(job->text));
      ret = smart_home_tts_synth_ex(job->text, &pcm, &bytes, &rate, &result);
      printf("[voice] synth: ret=%d pcm=%p bytes=%zu rate=%lu\n",
             ret, (void *)pcm, bytes, (unsigned long)rate);
      if (ret == 0)
        {
          pthread_mutex_lock(&g_voice_mutex);
          bool cancelled = g_voice_abort.abort_flag != 0;
          g_voice_state = VOICE_PLAY_PLAYING;
          pthread_mutex_unlock(&g_voice_mutex);

          if (!cancelled)
            {
              int play_ret = smart_home_voice_player_play_mem(
                  pcm, bytes, rate, &g_voice_abort);
              printf("[voice] play: ret=%d\n", play_ret);
              if (play_ret != -EINTR) {
                ret = play_ret;
                result.code = play_ret;
                result.stage = SMART_HOME_VOICE_PLAYBACK;
              }
            }
          else
            {
              printf("[voice] play: skipped (cancelled)\n");
            }

          smart_home_bulk_free(pcm);
        }
      else
        {
          fprintf(stderr, "voice: tts synth failed: %d\n", ret);
        }

#else
      ret = -ENOTSUP;
#endif
      }

      pthread_mutex_lock(&g_voice_mutex);
      if (job->alarm_id) {
          g_voice_alarm_error_id = job->alarm_id;
          g_voice_alarm_error = ret == -EINTR ? 0 : ret;
      }
      free(job);
      g_voice_active_alarm = 0;
      g_voice_state = VOICE_PLAY_IDLE;
      if (ret != 0 && ret != -EINTR) {
        g_voice_last_error = result;
        g_voice_error_pending = true;
      }
    }

  pthread_mutex_unlock(&g_voice_mutex);
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int voice_play_init(void)
{
  pthread_attr_t attr;
  int attr_ready = 0;
  int ret;

  pthread_mutex_lock(&g_voice_mutex);
  if (g_voice_started)
    {
      pthread_mutex_unlock(&g_voice_mutex);
      return 0;
    }

  printf("[voice] init: entry\n");

#ifdef CONFIG_SMART_HOME_VOICE_TTS
  /* 预热配置缓存，播报开关立即可查询。 */
  smart_home_tts_config_t config;
  smart_home_tts_config_load(&config);
  printf("[voice] init: config loaded (enabled=%d backend=%s)\n",
         config.enabled, config.backend_id);
#endif

  g_voice_active_alarm = 0;
  g_voice_alarm_error_id = 0;
  g_voice_alarm_error = 0;
  g_voice_stop_requested = false;
  g_voice_error_pending = false;
  g_voice_queue = NULL;
  g_voice_queue_tail = NULL;
  g_voice_state = VOICE_PLAY_IDLE;
  g_voice_abort.abort_flag = 0;
  pthread_mutex_unlock(&g_voice_mutex);

  /* pthread 栈在 RISC-V 上要求 16 字节对齐，bulk(malloc) 堆不保证；
   * 必须向上对齐（miloco/网络配网 worker 同款做法，少了这步会冻死）。 */
#ifdef __NuttX__
  g_voice_worker_stack_alloc = smart_home_bulk_alloc(
      CONFIG_SMART_HOME_VOICE_WORKER_STACKSIZE + STACK_ALIGNMENT - 1u);
  if (g_voice_worker_stack_alloc == NULL)
    {
      return -ENOMEM;
    }

  g_voice_worker_stack = (void *)STACK_ALIGN_UP(
      (uintptr_t)g_voice_worker_stack_alloc);
  printf("[voice] init: stack=%p\n", g_voice_worker_stack);

#endif
  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      attr_ready = 1;
#ifdef __NuttX__
      ret = pthread_attr_setstack(&attr, g_voice_worker_stack,
                                  CONFIG_SMART_HOME_VOICE_WORKER_STACKSIZE);
#endif
    }

#ifdef __NuttX__
  if (ret == 0)
    {
      ret = pthread_attr_setschedparam(&attr,
          &(struct sched_param)
          {
            .sched_priority = VOICE_WORKER_PRIORITY
          });
    }

#endif
  if (ret == 0)
    {
      ret = pthread_create(&g_voice_worker, &attr, voice_worker_main, NULL);
    }

  if (attr_ready)
    {
      pthread_attr_destroy(&attr);
    }

  if (ret != 0)
    {
      smart_home_bulk_free(g_voice_worker_stack_alloc);
      g_voice_worker_stack_alloc = NULL;
      g_voice_worker_stack = NULL;
      return -ret;
    }

  pthread_mutex_lock(&g_voice_mutex);
  g_voice_started = true;
  pthread_mutex_unlock(&g_voice_mutex);
  printf("[voice] init: complete\n");
  return 0;
}

void voice_play_deinit(void)
{
  voice_job_t *job;
  voice_job_t *next;

  pthread_mutex_lock(&g_voice_mutex);
  if (!g_voice_started)
    {
      pthread_mutex_unlock(&g_voice_mutex);
      return;
    }

  g_voice_stop_requested = true;
  g_voice_abort.abort_flag = 1;
  pthread_cond_broadcast(&g_voice_cond);
  pthread_mutex_unlock(&g_voice_mutex);

  pthread_join(g_voice_worker, NULL);

  pthread_mutex_lock(&g_voice_mutex);
  job = g_voice_queue;
  g_voice_queue = NULL;
  g_voice_queue_tail = NULL;
  g_voice_started = false;
  g_voice_state = VOICE_PLAY_IDLE;
  pthread_mutex_unlock(&g_voice_mutex);

  while (job != NULL)
    {
      next = job->next;
      free(job);
      job = next;
    }

  smart_home_bulk_free(g_voice_worker_stack_alloc);
  g_voice_worker_stack_alloc = NULL;
  g_voice_worker_stack = NULL;
}

int voice_play_text(const char *text)
{
  voice_job_t *job;
  size_t length;

  if (text == NULL || text[0] == '\0')
    {
      return -EINVAL;
    }

  length = strlen(text);
  if (length >= CONFIG_SMART_HOME_VOICE_TEXT_MAX)
    {
      length = CONFIG_SMART_HOME_VOICE_TEXT_MAX - 1;
    }

  pthread_mutex_lock(&g_voice_mutex);
  if (!g_voice_started)
    {
      pthread_mutex_unlock(&g_voice_mutex);
      return -EAGAIN;
    }

  if (voice_queue_count() >= 8) {
      pthread_mutex_unlock(&g_voice_mutex);
      return -ENOSPC;
  }
  job = calloc(1, sizeof(*job));
  if (job == NULL)
    {
      pthread_mutex_unlock(&g_voice_mutex);
      return -ENOMEM;
    }

  memcpy(job->text, text, length);
  job->text[length] = '\0';

  job->next = NULL;
  if (g_voice_queue_tail != NULL)
    {
      g_voice_queue_tail->next = job;
    }
  else
    {
      g_voice_queue = job;
    }

  g_voice_queue_tail = job;
  pthread_cond_signal(&g_voice_cond);
  pthread_mutex_unlock(&g_voice_mutex);
  return 0;
}

void voice_play_stop(void)
{
  voice_job_t **link, *job;
  pthread_mutex_lock(&g_voice_mutex);
  if (g_voice_started && !g_voice_active_alarm) g_voice_abort.abort_flag = 1;
  link = &g_voice_queue;
  while ((job = *link) != NULL) {
    if (!job->alarm_id) { *link = job->next; free(job); }
    else link = &job->next;
  }
  g_voice_queue_tail = g_voice_queue;
  while (g_voice_queue_tail && g_voice_queue_tail->next)
    g_voice_queue_tail = g_voice_queue_tail->next;
  pthread_mutex_unlock(&g_voice_mutex);
}

int voice_play_alarm(int id)
{
  voice_job_t *job;
  if (id <= 0) return -EINVAL;
  pthread_mutex_lock(&g_voice_mutex);
  if (!g_voice_started || g_voice_stop_requested) {
    pthread_mutex_unlock(&g_voice_mutex); return -EAGAIN;
  }
  if (voice_queue_count() >= 8) {
    pthread_mutex_unlock(&g_voice_mutex); return -ENOSPC;
  }
  job = calloc(1,sizeof(*job));
  if (!job) { pthread_mutex_unlock(&g_voice_mutex); return -ENOMEM; }
  job->alarm_id = id;
  job->next = g_voice_queue;
  g_voice_queue = job;
  if (!g_voice_queue_tail) g_voice_queue_tail = job;
  g_voice_abort.abort_flag = 1;
  g_voice_alarm_error_id = id;
  g_voice_alarm_error = 0;
  pthread_cond_signal(&g_voice_cond);
  pthread_mutex_unlock(&g_voice_mutex);
  return 0;
}

void voice_play_alarm_stop(int id)
{
  voice_job_t **link, *job;
  pthread_mutex_lock(&g_voice_mutex);
  if (g_voice_active_alarm == id) g_voice_abort.abort_flag = 1;
  link = &g_voice_queue;
  while ((job = *link) != NULL) {
    if (job->alarm_id == id) { *link = job->next; free(job); }
    else link = &job->next;
  }
  g_voice_queue_tail = g_voice_queue;
  while (g_voice_queue_tail && g_voice_queue_tail->next)
    g_voice_queue_tail = g_voice_queue_tail->next;
  pthread_mutex_unlock(&g_voice_mutex);
}

int voice_play_alarm_error(int id)
{
  int ret;
  pthread_mutex_lock(&g_voice_mutex);
  ret = id == g_voice_alarm_error_id ? g_voice_alarm_error : 0;
  pthread_mutex_unlock(&g_voice_mutex);
  return ret;
}

voice_play_state_t voice_play_state(void)
{
  voice_play_state_t state;
  pthread_mutex_lock(&g_voice_mutex);
  state = g_voice_state;
  pthread_mutex_unlock(&g_voice_mutex);
  return state;
}

int voice_play_announce_enabled(void)
{
#ifdef CONFIG_SMART_HOME_VOICE_TTS
  smart_home_tts_config_t config;

  smart_home_tts_config_load(&config);
  return config.enabled;
#else
  return 0;
#endif
}

int voice_play_take_error(smart_home_voice_result_t *result)
{
  int pending;
  if (result == NULL) return 0;
  pthread_mutex_lock(&g_voice_mutex);
  pending = g_voice_error_pending;
  if (pending) *result = g_voice_last_error;
  g_voice_error_pending = false;
  pthread_mutex_unlock(&g_voice_mutex);
  return pending;
}
