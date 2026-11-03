/****************************************************************************
 * demos/smart_home/tests/kws_frontend/frontend_selftest.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * KWS 特征前端宿主自测。不依赖板子、不依赖模型，用合成 PCM 检查：
 *   1. 输出长度与取值范围
 *   2. 数字静音的特征值落在下界（clip 到 0）
 *   3. 单频正弦的 mel 峰值落在预期频带
 *   4. 差分通道首帧为 0，且二阶差分等于一阶差分的差分
 *   5. 量化后落在 int8 范围，且同一输入可重复
 *
 * 编译（在 tests/kws_frontend 下）：
 *   gcc -O2 -Wall -Wextra -I../../src/voice \
 *       frontend_selftest.c ../../src/voice/smart_home_kws_frontend.c \
 *       -lm -o frontend_selftest
 ****************************************************************************/

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smart_home_kws_frontend.h"

#define RATE 16000

static int g_failures;

static void check(int condition, const char *name)
{
  printf("%-46s %s\n", name, condition ? "OK" : "FAIL");

  if (!condition)
    {
      g_failures++;
    }
}

static void fill_silence(int16_t *pcm, size_t count)
{
  memset(pcm, 0, count * sizeof(int16_t));
}

static void fill_tone(int16_t *pcm, size_t count, double hz, double amplitude)
{
  size_t i;

  for (i = 0; i < count; i++)
    {
      pcm[i] = (int16_t)lrint(amplitude * 32767.0 *
                              sin(2.0 * M_PI * hz * (double)i / (double)RATE));
    }
}

/* 找 mel 能量最大的频带（用 base 通道的中段帧，避开首尾边界）。 */
static int dominant_mel(const int8_t *features, int frame)
{
  int mel;
  int best = -1;
  int best_value = -129;

  for (mel = 0; mel < KWS_FRONTEND_MEL_BINS; mel++)
    {
      int value = features[(frame * KWS_FRONTEND_MEL_BINS + mel) *
                           KWS_FRONTEND_STACK];

      if (value > best_value)
        {
          best_value = value;
          best = mel;
        }
    }

  return best;
}

int main(void)
{
  static int16_t pcm[KWS_FRONTEND_PCM_SAMPLES];
  static int8_t features[KWS_FRONTEND_FEATURE_COUNT];
  static int8_t features2[KWS_FRONTEND_FEATURE_COUNT];
  const float scale = 0.17718f;
  const int zp = -20;
  int i;
  int frame;
  int ok;

  printf("frontend selftest: samples=%d features=%d\n",
         KWS_FRONTEND_PCM_SAMPLES, KWS_FRONTEND_FEATURE_COUNT);

  check(KWS_FRONTEND_FEATURE_COUNT == 124 * 40 * 3, "feature count is 124*40*3");
  check(KWS_FRONTEND_PCM_SAMPLES == 40000, "pcm samples is 40000");

  /* 1. 数字静音：能量为 0，log 后 clip 到下界。 */
  fill_silence(pcm, KWS_FRONTEND_PCM_SAMPLES);
  ok = kws_frontend_compute(pcm, scale, zp, features) == 0;
  check(ok, "silence: compute returns 0");

  ok = 1;
  for (i = 0; i < KWS_FRONTEND_FEATURE_COUNT; i++)
    {
      if (features[i] != (int8_t)lrintf((0.0f / scale) + (float)zp))
        {
          /* 静音时 base=clip((ln(0+1e-6)+12)*1.625,0,26)=clip(4.3,0,26) */
          ok = 0;
          break;
        }
    }

  check(ok, "silence: all features equal clip lower bound");

  /* 2. 单频 1 kHz：能量应集中在低频 mel 带。 */
  fill_tone(pcm, KWS_FRONTEND_PCM_SAMPLES, 1000.0, 0.5);
  ok = kws_frontend_compute(pcm, scale, zp, features) == 0;
  check(ok, "1 kHz tone: compute returns 0");

  {
    int mel = dominant_mel(features, 60);

    printf("  1 kHz dominant mel = %d (expect low band)\n", mel);
    check(mel >= 0 && mel <= 12, "1 kHz: dominant mel in low band");
  }

  /* 3. 8 kHz 附近：应落在高频 mel 带，且明显高于 1 kHz 时同一帧的峰值。 */
  fill_tone(pcm, KWS_FRONTEND_PCM_SAMPLES, 6000.0, 0.5);
  ok = kws_frontend_compute(pcm, scale, zp, features) == 0;
  check(ok, "6 kHz tone: compute returns 0");

  {
    int mel = dominant_mel(features, 60);

    printf("  6 kHz dominant mel = %d (expect high band)\n", mel);
    check(mel >= 25, "6 kHz: dominant mel in high band");
  }

  /* 4. 差分：首帧 delta/delta2 的去量化值必须为 0。
   * 注意量化后 0 对应 int8 的 zero_point，而不是 0。
   */
  {
    int all_zero = 1;
    int expected = (int)zp;

    for (i = 0; i < KWS_FRONTEND_MEL_BINS; i++)
      {
        int base = (0 * KWS_FRONTEND_MEL_BINS + i) * KWS_FRONTEND_STACK;

        if (abs((int)features[base + 1] - expected) > 1 ||
            abs((int)features[base + 2] - expected) > 1)
          {
            all_zero = 0;
            break;
          }
      }

    check(all_zero, "first frame: delta and delta2 dequantize to zero");
  }

  /* 5. 二阶差分等于一阶差分的差分。
   *
   * 在量化域里 q(d2) = d2/scale + zp，而两个 delta 相减会消掉 zp，
   * 所以要补回 zp 才能比较：q(d2) ≈ q(d1[t]) - q(d1[t-1]) + zp。
   */
  {
    int max_error = 0;

    for (frame = 2; frame < KWS_FRONTEND_TIME_STEPS; frame++)
      {
        for (i = 0; i < KWS_FRONTEND_MEL_BINS; i++)
          {
            int cur = (frame * KWS_FRONTEND_MEL_BINS + i) * KWS_FRONTEND_STACK;
            int prev = ((frame - 1) * KWS_FRONTEND_MEL_BINS + i) *
                       KWS_FRONTEND_STACK;
            int expected = (int)features[cur + 1] -
                           (int)features[prev + 1] + zp;
            int error = abs((int)features[cur + 2] - expected);

            if (error > max_error)
              {
                max_error = error;
              }
          }
      }

    printf("  max delta2 reconstruction error = %d (tolerance 2)\n",
           max_error);
    check(max_error <= 2, "delta2 equals diff of delta (within 2)");
  }

  /* 6. 可重复性：同一输入两次结果逐字节一致。 */
  ok = kws_frontend_compute(pcm, scale, zp, features2) == 0;
  check(ok && memcmp(features, features2, KWS_FRONTEND_FEATURE_COUNT) == 0,
        "same input produces identical output");

  /* 7. 非法参数必须被拒绝。 */
  check(kws_frontend_compute(NULL, scale, zp, features) != 0,
        "reject null pcm");
  check(kws_frontend_compute(pcm, 0.0f, zp, features) != 0,
        "reject zero quant scale");

  printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
         g_failures, g_failures == 1 ? "" : "s");
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
