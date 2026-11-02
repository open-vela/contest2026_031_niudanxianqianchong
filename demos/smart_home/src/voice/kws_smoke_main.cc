/****************************************************************************
 * demos/smart_home/src/voice/kws_smoke_main.cc
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/* kws_smoke：wake_large 模型冒烟与基准命令。
 *
 * 验证 TFLite FlatBuffer、算子注册、arena 用量与张量契约，随后以
 * 确定性 int8 伪特征做 warmup + 计时 Invoke，报告 min/P50/mean/P95/max。
 * 不打开音频设备、不做特征提取、不触发任何 UI/网络行为。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../smart_home_memory.h"
#include "kws/kws_infer.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define KWS_SMOKE_DEFAULT_WARMUP     5
#define KWS_SMOKE_DEFAULT_REPEAT     50
#define KWS_SMOKE_MAX_REPEAT         200

#ifndef CONFIG_SMART_HOME_KWS_SMOKE_ARENA_SIZE
#define CONFIG_SMART_HOME_KWS_SMOKE_ARENA_SIZE 196608
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int8_t g_features[KWS_INFER_FEATURE_SIZE];
static float g_scores[KWS_INFER_CLASS_COUNT];
static uint32_t g_invoke_us[KWS_SMOKE_MAX_REPEAT];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void kws_smoke_usage(const char *program)
{
  printf("Usage: %s [--warmup N] [--repeat N]\n", program);
  printf("  --warmup N  Invokes excluded from timing (default: %d)\n",
         KWS_SMOKE_DEFAULT_WARMUP);
  printf("  --repeat N  Timed invokes, 1-%d (default: %d)\n",
         KWS_SMOKE_MAX_REPEAT, KWS_SMOKE_DEFAULT_REPEAT);
}

static int kws_smoke_parse_count(const char *text, unsigned int maximum,
                                 unsigned int *value)
{
  char *end;
  unsigned long parsed;

  if (text == NULL || value == NULL)
    {
      return -EINVAL;
    }

  errno = 0;
  parsed = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || parsed > maximum)
    {
      return -EINVAL;
    }

  *value = (unsigned int)parsed;
  return 0;
}

static int kws_smoke_parse_options(int argc, char *argv[],
                                   unsigned int *warmup,
                                   unsigned int *repeat)
{
  int index;

  *warmup = KWS_SMOKE_DEFAULT_WARMUP;
  *repeat = KWS_SMOKE_DEFAULT_REPEAT;

  for (index = 1; index < argc; index++)
    {
      if (strcmp(argv[index], "--warmup") == 0 && index + 1 < argc)
        {
          if (kws_smoke_parse_count(argv[++index], KWS_SMOKE_MAX_REPEAT,
                                    warmup) < 0)
            {
              return -EINVAL;
            }
        }
      else if (strcmp(argv[index], "--repeat") == 0 && index + 1 < argc)
        {
          if (kws_smoke_parse_count(argv[++index], KWS_SMOKE_MAX_REPEAT,
                                    repeat) < 0 || *repeat == 0)
            {
              return -EINVAL;
            }
        }
      else if (strcmp(argv[index], "--help") == 0 ||
               strcmp(argv[index], "-h") == 0)
        {
          kws_smoke_usage(argv[0]);
          return 1;
        }
      else
        {
          return -EINVAL;
        }
    }

  return 0;
}

static uint64_t kws_smoke_monotonic_us(void)
{
  struct timespec time;

  if (clock_gettime(CLOCK_MONOTONIC, &time) < 0)
    {
      return 0;
    }

  return (uint64_t)time.tv_sec * UINT64_C(1000000) +
         (uint64_t)time.tv_nsec / UINT64_C(1000);
}

static int kws_smoke_compare_u32(const void *left, const void *right)
{
  uint32_t a = *(const uint32_t *)left;
  uint32_t b = *(const uint32_t *)right;

  return (a > b) - (a < b);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

extern "C" int main(int argc, char *argv[])
{
  static const char *const labels[KWS_INFER_CLASS_COUNT] =
  {
    "wake", "hard_neg", "other_speech", "background", "silence"
  };
  unsigned int warmup;
  unsigned int repeat;
  unsigned int run;
  uint64_t total_us = 0;
  void *arena;
  float scale;
  int zp;
  int status;
  int ret;

  ret = kws_smoke_parse_options(argc, argv, &warmup, &repeat);
  if (ret != 0)
    {
      if (ret < 0)
        {
          kws_smoke_usage(argv[0]);
        }

      return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  arena = smart_home_bulk_alloc(CONFIG_SMART_HOME_KWS_SMOKE_ARENA_SIZE);
  if (arena == NULL)
    {
      fprintf(stderr, "[kws_smoke] arena alloc failed (%u)\n",
              (unsigned)CONFIG_SMART_HOME_KWS_SMOKE_ARENA_SIZE);
      return EXIT_FAILURE;
    }

  printf("[kws_smoke] arena_reserved=%u address=%p backend=%s\n",
         (unsigned)CONFIG_SMART_HOME_KWS_SMOKE_ARENA_SIZE, arena,
         kws_infer_backend() == KWS_INFER_BACKEND_ESPNN ? "esp-nn" :
                                                          "reference");

  status = kws_infer_init(arena, CONFIG_SMART_HOME_KWS_SMOKE_ARENA_SIZE);
  if (status != 0)
    {
      /* -EINVAL：模型版本/算子注册/张量契约不符；-ENOMEM：arena 不足；
       * -EBUSY：已有其他 arena 的解释器。init 失败时内部已干净退出，
       * 这里只需释放自己分配的 arena。 */
      fprintf(stderr,
              "[kws_smoke] model init failed: %d (arena_reserved=%u, "
              "arena_used=%u)\n",
              status, (unsigned)CONFIG_SMART_HOME_KWS_SMOKE_ARENA_SIZE,
              (unsigned)kws_infer_arena_used());
      smart_home_bulk_free(arena);
      return EXIT_FAILURE;
    }

  fprintf(stderr, "[kws_smoke] stage=allocated arena_used=%u\n",
          (unsigned)kws_infer_arena_used());
  kws_infer_input_params(&scale, &zp);
  fprintf(stderr, "[kws_smoke] stage=params scale=%d zp=%d\n",
          (int)(scale * 1000000.0f), zp);

  for (run = 0; run < (unsigned)KWS_INFER_FEATURE_SIZE; run++)
    {
      g_features[run] = (int8_t)(((uint32_t)run * UINT32_C(37) +
                                  UINT32_C(19)) >> 1);
    }

  fprintf(stderr, "[kws_smoke] stage=features ready\n");
  for (run = 0; run < warmup; run++)
    {
      uint64_t wstart = kws_smoke_monotonic_us();
      uint64_t wend;

      if (kws_infer_run(g_features, g_scores) != 0)
        {
          fprintf(stderr, "[kws_smoke] warmup invoke %u failed\n", run + 1);
          (void)kws_infer_deinit(arena);
          smart_home_bulk_free(arena);
          return EXIT_FAILURE;
        }

      wend = kws_smoke_monotonic_us();
      fprintf(stderr, "[kws_smoke] warmup %u took %llu us\n", run + 1,
              (unsigned long long)(wend - wstart));
    }

  for (run = 0; run < repeat; run++)
    {
      uint64_t start = kws_smoke_monotonic_us();
      uint64_t end;

      if (kws_infer_run(g_features, g_scores) != 0)
        {
          fprintf(stderr, "[kws_smoke] timed invoke %u failed\n", run + 1);
          (void)kws_infer_deinit(arena);
          smart_home_bulk_free(arena);
          return EXIT_FAILURE;
        }

      end = kws_smoke_monotonic_us();
      if (start == 0 || end < start || end - start > UINT32_MAX)
        {
          fprintf(stderr, "[kws_smoke] monotonic timing unavailable\n");
          (void)kws_infer_deinit(arena);
          smart_home_bulk_free(arena);
          return EXIT_FAILURE;
        }

      g_invoke_us[run] = (uint32_t)(end - start);
      total_us += g_invoke_us[run];
    }

  qsort(g_invoke_us, repeat, sizeof(g_invoke_us[0]), kws_smoke_compare_u32);
  printf("[kws_smoke] invoke_us warmup=%u repeat=%u min=%" PRIu32
         " p50=%" PRIu32 " mean=%" PRIu64 " p95=%" PRIu32
         " max=%" PRIu32 "\n",
         warmup, repeat, g_invoke_us[0],
         g_invoke_us[(repeat - 1) / 2], total_us / repeat,
         g_invoke_us[(repeat * 95 + 99) / 100 - 1],
         g_invoke_us[repeat - 1]);

  printf("[kws_smoke] output");
  for (run = 0; run < (unsigned)KWS_INFER_CLASS_COUNT; run++)
    {
      printf(" %s=%.4f", labels[run], (double)g_scores[run]);
    }

  printf("\n");

  /* 解释器持有 arena 指针，必须先销毁再释放内存。 */
  (void)kws_infer_deinit(arena);
  smart_home_bulk_free(arena);
  return EXIT_SUCCESS;
}
