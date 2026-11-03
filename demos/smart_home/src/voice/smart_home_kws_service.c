/****************************************************************************
 * demos/smart_home/src/voice/smart_home_kws_service.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "smart_home_kws_service.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/irq.h>

#include "../smart_home_memory.h"
#include "../config/cjson_compat.h"
#include "../config/smart_home_config_store.h"
#include <agent/error.h>
#include "kws/kws_infer.h"
#include "smart_home_kws_frontend.h"
#include "smart_home_voice_capture.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define KWS_CONFIG_PATH SMART_HOME_CONFIG_DIR "/kws.json"

/* NuttX 数字越大优先级越高：推理是重后台任务，必须低于 UI(100)、
 * 采集分发(110)与播报(90 同级即可)，每次推理数百 ms 不得抢占界面。 */
#define KWS_WORKER_PRIORITY 90

#ifndef CONFIG_SMART_HOME_KWS_WORKER_STACKSIZE
#define CONFIG_SMART_HOME_KWS_WORKER_STACKSIZE 12288
#endif

#ifndef CONFIG_SMART_HOME_KWS_ARENA_SIZE
#define CONFIG_SMART_HOME_KWS_ARENA_SIZE 196608
#endif

#ifndef CONFIG_SMART_HOME_KWS_INTERVAL_MS
#define CONFIG_SMART_HOME_KWS_INTERVAL_MS 500
#endif

/* 阈值与冷却（permille/毫秒整数，避免浮点 Kconfig）。 */
#ifndef CONFIG_SMART_HOME_KWS_THRESHOLD
#define CONFIG_SMART_HOME_KWS_THRESHOLD 750
#endif

#ifndef CONFIG_SMART_HOME_KWS_COOLDOWN_MS
#define CONFIG_SMART_HOME_KWS_COOLDOWN_MS 3000
#endif

/* 连续命中窗数：滑窗推理间相关性高，2/3 多数即可触发。 */
#define KWS_TRIGGER_VOTE_TOTAL  3
#define KWS_TRIGGER_VOTE_NEED   2

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct kws_state_s
{
  pthread_mutex_t mutex;
  pthread_t worker;
  bool started;
  bool stop_requested;
  bool paused;

  kws_wake_cb_t cb;
  void *user_data;

  /* 资源（start 分配，stop 释放）。 */
  void *arena;
  int16_t *ring;                 /* KWS_FRONTEND_PCM_SAMPLES 采样 */
  int8_t *features;
  volatile size_t ring_written;  /* 已写入采样数（单调递增） */
  size_t ring_pos;
  uint64_t last_wake_ms;
  uint8_t vote_window;
  void *worker_stack_alloc;      /* bulk 原始指针（释放用） */
  void *worker_stack;            /* 16B 对齐后交给 pthread */

  /* worker 在正式进入循环前汇报初始化结果；start() 等待它，
   * 避免「返回 0 但模型没起来」以及 init 失败时的资源泄漏。 */
  bool init_done;
  int init_status;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct kws_state_s g_kws =
{
  .mutex = PTHREAD_MUTEX_INITIALIZER,
};

static uint64_t kws_monotonic_ms(void)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000u +
         (uint64_t)now.tv_nsec / 1000000u;
}

/* 采集回调：运行于 voice_capture worker，仅做拷贝入环。 */
static void kws_capture_cb(const int16_t *samples, size_t sample_count,
                           void *user_data)
{
  struct kws_state_s *state = user_data;
  size_t capacity = KWS_FRONTEND_PCM_SAMPLES;

  /* ring_pos / ring_written 由 worker 在读、本回调在写，必须同一把锁。 */
  pthread_mutex_lock(&state->mutex);

  while (sample_count > 0)
    {
      size_t chunk = capacity - state->ring_pos;

      if (chunk > sample_count)
        {
          chunk = sample_count;
        }

      memcpy(state->ring + state->ring_pos, samples,
             chunk * sizeof(int16_t));
      samples += chunk;
      sample_count -= chunk;
      state->ring_pos = (state->ring_pos + chunk) % capacity;
      state->ring_written += chunk;
    }

  pthread_mutex_unlock(&state->mutex);
}

static void *kws_worker_main(void *arg)
{
  struct kws_state_s *state = &g_kws;
  int16_t *window = NULL;
  float scores[KWS_INFER_CLASS_COUNT];
  float quant_scale = 1.0f;
  int quant_zp = 0;
  size_t capacity = KWS_FRONTEND_PCM_SAMPLES;
  size_t i;
  int status;
  unsigned int hit;

  (void)arg;

  status = kws_infer_init(state->arena, CONFIG_SMART_HOME_KWS_ARENA_SIZE);
  if (status != 0)
    {
      fprintf(stderr, "kws: model init failed: %d\n", status);
      goto out_init_failed;
    }

  kws_infer_input_params(&quant_scale, &quant_zp);

  window = smart_home_bulk_alloc(capacity * sizeof(int16_t));
  if (window == NULL)
    {
      fprintf(stderr, "kws: window alloc failed\n");
      status = -ENOMEM;
      goto out_init_failed;
    }

  printf("[kws] resident service ready: arena_used=%u backend=%s "
         "threshold=%d/1000\n",
         (unsigned)kws_infer_arena_used(),
         kws_infer_backend() == KWS_INFER_BACKEND_ESPNN ? "esp-nn" : "reference",
         CONFIG_SMART_HOME_KWS_THRESHOLD);

  /* 初始化完全成功后才通知 start() 返回，避免调用方拿到假成功。 */
  pthread_mutex_lock(&state->mutex);
  state->init_status = 0;
  state->init_done = true;
  pthread_mutex_unlock(&state->mutex);

  while (true)
    {
      bool stop;
      bool paused;
      uint64_t now_ms;

      pthread_mutex_lock(&state->mutex);
      stop = state->stop_requested;
      paused = state->paused;
      pthread_mutex_unlock(&state->mutex);
      if (stop)
        {
          break;
        }

      usleep(CONFIG_SMART_HOME_KWS_INTERVAL_MS * 1000);

      if (paused)
        {
          continue;
        }

      /* 取窗口快照与判断数据量必须在同一临界区：分开加锁会让
       * ring_pos 在两次加锁之间被回调推进，读到未写完的区域。
       */
      pthread_mutex_lock(&state->mutex);
      if (state->ring_written < capacity)
        {
          pthread_mutex_unlock(&state->mutex);
          continue;
        }

      {
        size_t head = state->ring_pos;

        for (i = 0; i < capacity; i++)
          {
            window[i] = state->ring[(head + i) % capacity];
          }
      }

      pthread_mutex_unlock(&state->mutex);

      if (kws_frontend_compute(window, quant_scale, quant_zp,
                               state->features) != 0)
        {
          continue;
        }

      if (kws_infer_run(state->features, scores) != 0)
        {
          continue;
        }

      now_ms = kws_monotonic_ms();
      hit = scores[KWS_INFER_CLASS_WAKE] * 1000.0f >=
            (float)CONFIG_SMART_HOME_KWS_THRESHOLD;

      /* 2/3 多数投票 + 冷却。
       * vote_window 只保留最近 KWS_TRIGGER_VOTE_TOTAL 次结果，先左移再
       * 取低位，超出位宽的部分被掩码丢弃，不会无限增长。
       */
      state->vote_window =
          ((state->vote_window << 1) | hit) &
          ((1u << KWS_TRIGGER_VOTE_TOTAL) - 1u);

      {
        int votes = 0;
        int v;

        for (v = 0; v < KWS_TRIGGER_VOTE_TOTAL; v++)
          {
            if (state->vote_window & (1u << v))
              {
                votes++;
              }
          }

        if (votes >= KWS_TRIGGER_VOTE_NEED &&
            now_ms - state->last_wake_ms >=
                (uint64_t)CONFIG_SMART_HOME_KWS_COOLDOWN_MS)
          {
            kws_wake_cb_t cb;
            void *user_data;

            state->last_wake_ms = now_ms;
            state->vote_window = 0;

            pthread_mutex_lock(&state->mutex);
            cb = state->cb;
            user_data = state->user_data;
            pthread_mutex_unlock(&state->mutex);

            printf("[kws] wake! score=%.3f\n",
                   (double)scores[KWS_INFER_CLASS_WAKE]);
            if (cb != NULL)
              {
                cb(scores[KWS_INFER_CLASS_WAKE], user_data);
              }
          }
      }
    }

  smart_home_bulk_free(window);

  /* 解释器持有 arena 指针：退出前必须销毁，否则 stop() 释放 arena 后
   * 下一次 start() 会命中残留实例（旧的「幂等」分支直接返回 0）。
   */
  (void)kws_infer_deinit(state->arena);
  goto out_done;

out_init_failed:
  /* 让 start() 知道初始化失败，由 start() 统一回收资源。 */
  (void)kws_infer_deinit(state->arena);
  pthread_mutex_lock(&state->mutex);
  state->init_status = status;
  state->init_done = true;
  pthread_mutex_unlock(&state->mutex);

out_done:
  pthread_mutex_lock(&state->mutex);
  state->started = false;
  state->stop_requested = false;
  pthread_mutex_unlock(&state->mutex);
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* ── kws.json 配置 ──────────────────────────────────────────── */

static cJSON *kws_config_make_default(void *user_data)
{
  cJSON *root;

  (void)user_data;
  root = cJSON_CreateObject();
  if (root == NULL ||
      !cJSON_AddNumberToObject(root, "version", 1) ||
      !cJSON_AddBoolToObject(root, "enabled", 1))
    {
      cJSON_Delete(root);
      return NULL;
    }

  return root;
}

static void kws_config_read(const cJSON *root,
                            smart_home_kws_config_t *config)
{
  const cJSON *item;

  config->enabled = 1;
  item = cJSON_GetObjectItemCaseSensitive(root, "enabled");
  if (cJSON_IsBool(item))
    {
      config->enabled = cJSON_IsTrue(item);
    }
}

static int kws_config_validate(const cJSON *root, void *user_data)
{
  smart_home_kws_config_t config;

  (void)user_data;
  kws_config_read(root, &config);
  return AGENT_OK;
}

void smart_home_kws_config_load(smart_home_kws_config_t *config)
{
  cJSON *root = NULL;
  char error[96];

  if (config == NULL)
    {
      return;
    }

  if (smart_home_config_store_load(KWS_CONFIG_PATH,
                                   kws_config_make_default,
                                   SMART_HOME_CONFIG_RECOVER_DEFAULT,
                                   kws_config_validate, NULL,
                                   &root, error, sizeof(error)) == AGENT_OK)
    {
      kws_config_read(root, config);
      cJSON_Delete(root);
    }
  else
    {
      config->enabled = 1;
    }
}

int smart_home_kws_config_save(const smart_home_kws_config_t *config)
{
  cJSON *root;
  int ret;

  if (config == NULL)
    {
      return AGENT_ERROR_INVALID;
    }

  root = cJSON_CreateObject();
  if (root == NULL ||
      !cJSON_AddNumberToObject(root, "version", 1) ||
      !cJSON_AddBoolToObject(root, "enabled", config->enabled ? 1 : 0))
    {
      cJSON_Delete(root);
      return AGENT_ERROR_NOMEM;
    }

  ret = smart_home_config_store_save(KWS_CONFIG_PATH, root);
  cJSON_Delete(root);
  return ret;
}

/* ── 服务生命周期 ───────────────────────────────────────────── */

int kws_service_start(kws_wake_cb_t cb, void *user_data)
{
  pthread_attr_t attr;
  int attr_ready = 0;
  int status;
  int ret;

  pthread_mutex_lock(&g_kws.mutex);
  if (g_kws.started)
    {
      g_kws.cb = cb;
      g_kws.user_data = user_data;
      pthread_mutex_unlock(&g_kws.mutex);
      return 0;
    }

  g_kws.arena = smart_home_bulk_alloc(CONFIG_SMART_HOME_KWS_ARENA_SIZE);
  g_kws.ring = smart_home_bulk_alloc(KWS_FRONTEND_PCM_SAMPLES *
                                     sizeof(int16_t));
  g_kws.features = smart_home_bulk_alloc(KWS_INFER_FEATURE_SIZE);
  if (g_kws.arena == NULL || g_kws.ring == NULL ||
      g_kws.features == NULL)
    {
      smart_home_bulk_free(g_kws.arena);
      smart_home_bulk_free(g_kws.ring);
      smart_home_bulk_free(g_kws.features);
      g_kws.arena = NULL;
      g_kws.ring = NULL;
      g_kws.features = NULL;
      pthread_mutex_unlock(&g_kws.mutex);
      return -ENOMEM;
    }

  g_kws.cb = cb;
  g_kws.user_data = user_data;
  g_kws.paused = false;
  g_kws.stop_requested = false;
  g_kws.ring_written = 0;
  g_kws.ring_pos = 0;
  g_kws.last_wake_ms = 0;
  g_kws.vote_window = 0;
  g_kws.init_done = false;
  g_kws.init_status = 0;

  /* RISC-V pthread 栈须 16B 对齐，bulk 堆不保证（同 voice_play）。 */
  g_kws.worker_stack_alloc = smart_home_bulk_alloc(
      CONFIG_SMART_HOME_KWS_WORKER_STACKSIZE + STACK_ALIGNMENT - 1u);
  if (g_kws.worker_stack_alloc == NULL)
    {
      smart_home_bulk_free(g_kws.arena);
      smart_home_bulk_free(g_kws.ring);
      smart_home_bulk_free(g_kws.features);
      g_kws.arena = NULL;
      g_kws.ring = NULL;
      g_kws.features = NULL;
      pthread_mutex_unlock(&g_kws.mutex);
      return -ENOMEM;
    }

  g_kws.worker_stack = (void *)STACK_ALIGN_UP(
      (uintptr_t)g_kws.worker_stack_alloc);

  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      attr_ready = 1;
      ret = pthread_attr_setstack(&attr, g_kws.worker_stack,
                                  CONFIG_SMART_HOME_KWS_WORKER_STACKSIZE);
    }

  if (ret == 0)
    {
      ret = pthread_attr_setschedparam(&attr,
          &(struct sched_param)
          {
            .sched_priority = KWS_WORKER_PRIORITY
          });
    }

  if (ret == 0)
    {
      g_kws.started = true;
      ret = pthread_create(&g_kws.worker, &attr, kws_worker_main, NULL);
    }

  if (attr_ready)
    {
      pthread_attr_destroy(&attr);
    }

  if (ret != 0)
    {
      g_kws.started = false;
      smart_home_bulk_free(g_kws.worker_stack_alloc);
      g_kws.worker_stack_alloc = NULL;
      g_kws.worker_stack = NULL;
      smart_home_bulk_free(g_kws.arena);
      smart_home_bulk_free(g_kws.ring);
      smart_home_bulk_free(g_kws.features);
      g_kws.arena = NULL;
      g_kws.ring = NULL;
      g_kws.features = NULL;
      pthread_mutex_unlock(&g_kws.mutex);
      return -ret;
    }

  /* 等待 worker 完成模型初始化：模型失败时不能返回 0，
   * 否则调用方认为服务已就绪，而 worker 已经退出、资源无人回收。
   */
  while (!g_kws.init_done)
    {
      pthread_mutex_unlock(&g_kws.mutex);
      usleep(1000);
      pthread_mutex_lock(&g_kws.mutex);
    }

  status = g_kws.init_status;
  if (status != 0)
    {
      pthread_mutex_unlock(&g_kws.mutex);
      /* worker 已因初始化失败退出，回收它没能自己释放的资源。 */
      pthread_join(g_kws.worker, NULL);
      smart_home_bulk_free(g_kws.worker_stack_alloc);
      g_kws.worker_stack_alloc = NULL;
      g_kws.worker_stack = NULL;
      smart_home_bulk_free(g_kws.arena);
      smart_home_bulk_free(g_kws.ring);
      smart_home_bulk_free(g_kws.features);
      g_kws.arena = NULL;
      g_kws.ring = NULL;
      g_kws.features = NULL;
      return status;
    }

  pthread_mutex_unlock(&g_kws.mutex);

  /* 采集 worker 原先只在 PTT 录音时按需启动，常驻监听因此收不到任何帧：
   * 冷启动后 kws_service 在跑，但环缓冲始终为空。这里显式拉起共享采集
   * 服务，KWS 才真正开始收音频。voice_capture_start() 幂等，重复调用安全。 */
  status = voice_capture_start();
  if (status != 0)
    {
      fprintf(stderr, "kws: capture start failed: %d\n", status);
      kws_service_stop();
      return status;
    }

  /* 注册为采集消费者（在锁外调用，避免与采集回调死锁）。 */
  if (voice_capture_listen(kws_capture_cb, &g_kws) != 0)
    {
      kws_service_stop();
      return -EIO;
    }

  return 0;
}

void kws_service_stop(void)
{
  pthread_mutex_lock(&g_kws.mutex);
  if (!g_kws.started)
    {
      pthread_mutex_unlock(&g_kws.mutex);
      return;
    }

  g_kws.stop_requested = true;
  pthread_mutex_unlock(&g_kws.mutex);

  voice_capture_stop_listening(kws_capture_cb);
  pthread_join(g_kws.worker, NULL);

  smart_home_bulk_free(g_kws.worker_stack_alloc);
  g_kws.worker_stack_alloc = NULL;
  g_kws.worker_stack = NULL;
  smart_home_bulk_free(g_kws.arena);
  smart_home_bulk_free(g_kws.ring);
  smart_home_bulk_free(g_kws.features);
  g_kws.arena = NULL;
  g_kws.ring = NULL;
  g_kws.features = NULL;
}

void kws_service_set_paused(int paused)
{
  pthread_mutex_lock(&g_kws.mutex);
  g_kws.paused = paused != 0;
  pthread_mutex_unlock(&g_kws.mutex);
}

int kws_service_running(void)
{
  int running;

  pthread_mutex_lock(&g_kws.mutex);
  running = g_kws.started;
  pthread_mutex_unlock(&g_kws.mutex);
  return running;
}
