/****************************************************************************
 * demos/smart_home/src/voice/smart_home_kws_frontend.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "smart_home_kws_frontend.h"

#include <errno.h>
#include <math.h>
#include <string.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define KWS_FFT_BINS        (KWS_FRONTEND_FFT_SIZE / 2 + 1)  /* 257 */
#define KWS_LOG_EPSILON     1e-6f
#define KWS_LOG_OFFSET      12.0f
#define KWS_LOG_SCALE       1.625f
#define KWS_FEATURE_MIN     0.0f
#define KWS_FEATURE_MAX     26.0f

/****************************************************************************
 * Private Data
 ****************************************************************************/

static float g_window[KWS_FRONTEND_FRAME_SAMPLES];
static float g_mel_weights[KWS_FRONTEND_MEL_BINS][KWS_FFT_BINS];
static float g_cos_table[KWS_FRONTEND_FFT_SIZE / 2];
static float g_sin_table[KWS_FRONTEND_FFT_SIZE / 2];
static uint16_t g_bit_reverse[KWS_FRONTEND_FFT_SIZE];
static int g_frontend_ready;

/* 逐帧中间结果（compute 由调用方串行化）。 */
static float g_fft_re[KWS_FRONTEND_FFT_SIZE];
static float g_fft_im[KWS_FRONTEND_FFT_SIZE];
static float g_magnitude[KWS_FFT_BINS];
static float g_base[KWS_FRONTEND_TIME_STEPS][KWS_FRONTEND_MEL_BINS];
static float g_delta[KWS_FRONTEND_TIME_STEPS][KWS_FRONTEND_MEL_BINS];
static float g_delta2[KWS_FRONTEND_TIME_STEPS][KWS_FRONTEND_MEL_BINS];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static float hz_to_mel(float hz)
{
  /* 与 TensorFlow linear_to_mel_weight_matrix 一致的 HTK 公式。 */
  return 1127.0f * logf(1.0f + hz / 700.0f);
}

static float mel_to_hz(float mel)
{
  return 700.0f * (expf(mel / 1127.0f) - 1.0f);
}

/* 迭代基-2 复数 FFT（就地）。旋转因子查表，避免每级重复三角函数。 */
static void fft(float *re, float *im)
{
  const int n = KWS_FRONTEND_FFT_SIZE;
  int i;
  int j;
  int len;
  int half;

  for (i = 0; i < n; i++)
    {
      j = (int)g_bit_reverse[i];
      if (j > i)
        {
          float tr = re[i];
          float ti = im[i];

          re[i] = re[j];
          im[i] = im[j];
          re[j] = tr;
          im[j] = ti;
        }
    }

  for (len = 2; len <= n; len <<= 1)
    {
      half = len >> 1;
      for (i = 0; i < n; i += len)
        {
          for (j = 0; j < half; j++)
            {
              int idx = j * (n / len);
              float w_re = g_cos_table[idx];
              float w_im = g_sin_table[idx];
              float u_re = re[i + j];
              float u_im = im[i + j];
              float v_re = re[i + j + half] * w_re - im[i + j + half] * w_im;
              float v_im = re[i + j + half] * w_im + im[i + j + half] * w_re;

              re[i + j] = u_re + v_re;
              im[i + j] = u_im + v_im;
              re[i + j + half] = u_re - v_re;
              im[i + j + half] = u_im - v_im;
            }
        }
    }
}

static float clip_feature(float value)
{
  if (value < KWS_FEATURE_MIN)
    {
      return KWS_FEATURE_MIN;
    }

  if (value > KWS_FEATURE_MAX)
    {
      return KWS_FEATURE_MAX;
    }

  return value;
}

static int8_t quantize_feature(float value, float quant_scale,
                               int quant_zero_point)
{
  float q = value / quant_scale + (float)quant_zero_point;
  long iq = lroundf(q);

  if (iq > 127)
    {
      iq = 127;
    }
  else if (iq < -128)
    {
      iq = -128;
    }

  return (int8_t)iq;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int kws_frontend_init(void)
{
  const float mel_min = hz_to_mel(KWS_FRONTEND_MEL_FMIN_HZ);
  const float mel_max = hz_to_mel(KWS_FRONTEND_MEL_FMAX_HZ);
  const float mel_step = (mel_max - mel_min) /
                         (float)(KWS_FRONTEND_MEL_BINS + 1);
  const float bin_hz = (float)KWS_FRONTEND_SAMPLE_RATE /
                       (float)KWS_FRONTEND_FFT_SIZE;
  int i;
  int bin;
  int mel;

  if (g_frontend_ready)
    {
      return 0;
    }

  /* periodic Hann：分母是 N 而非 N-1，与训练实现的 frame_length 一致。 */
  for (i = 0; i < KWS_FRONTEND_FRAME_SAMPLES; i++)
    {
      g_window[i] = 0.5f - 0.5f *
          cosf(2.0f * (float)M_PI * (float)i /
               (float)KWS_FRONTEND_FRAME_SAMPLES);
    }

  /* 位反转表。 */
  {
    int bits = 0;

    while ((1 << bits) < KWS_FRONTEND_FFT_SIZE)
      {
        bits++;
      }

    for (i = 0; i < KWS_FRONTEND_FFT_SIZE; i++)
      {
        unsigned int x = (unsigned int)i;
        unsigned int r = 0;
        int b;

        for (b = 0; b < bits; b++)
          {
            r = (r << 1) | (x & 1u);
            x >>= 1;
          }

        g_bit_reverse[i] = (uint16_t)r;
      }
  }

  /* 旋转因子表：只需前 N/2 项。 */
  for (i = 0; i < KWS_FRONTEND_FFT_SIZE / 2; i++)
    {
      float angle = -2.0f * (float)M_PI * (float)i /
                    (float)KWS_FRONTEND_FFT_SIZE;

      g_cos_table[i] = cosf(angle);
      g_sin_table[i] = sinf(angle);
    }

  /* 三角 mel 滤波器组：在 mel 轴等距取三点，再映射回 Hz 求斜率。
   * TensorFlow 的 linear_to_mel_weight_matrix 就是这么构造的，
   * 因此在 Hz 轴上看滤波器不等宽。
   */
  for (mel = 0; mel < KWS_FRONTEND_MEL_BINS; mel++)
    {
      float left_hz = mel_to_hz(mel_min + mel_step * (float)mel);
      float center_hz = mel_to_hz(mel_min + mel_step * (float)(mel + 1));
      float right_hz = mel_to_hz(mel_min + mel_step * (float)(mel + 2));

      memset(g_mel_weights[mel], 0, sizeof(g_mel_weights[mel]));

      for (bin = 0; bin < KWS_FFT_BINS; bin++)
        {
          float freq = (float)bin * bin_hz;

          if (freq <= left_hz || freq >= right_hz)
            {
              continue;
            }

          if (freq <= center_hz)
            {
              g_mel_weights[mel][bin] = (freq - left_hz) /
                                        (center_hz - left_hz);
            }
          else
            {
              g_mel_weights[mel][bin] = (right_hz - freq) /
                                        (right_hz - center_hz);
            }
        }
    }

  g_frontend_ready = 1;
  return 0;
}

int kws_frontend_compute(const int16_t *pcm, float quant_scale,
                         int quant_zero_point, int8_t *out)
{
  int frame;
  int mel;
  int i;
  int t;

  if (pcm == NULL || out == NULL || quant_scale <= 0.0f)
    {
      return -EINVAL;
    }

  if (kws_frontend_init() != 0)
    {
      return -EIO;
    }

  /* 逐帧：加窗 → FFT → 幅度谱 → mel 能量 → log → 归一化。 */
  for (frame = 0; frame < KWS_FRONTEND_TIME_STEPS; frame++)
    {
      const int16_t *src = pcm + frame * KWS_FRONTEND_HOP_SAMPLES;

      memset(g_fft_im, 0, sizeof(g_fft_im));
      for (i = 0; i < KWS_FRONTEND_FRAME_SAMPLES; i++)
        {
          g_fft_re[i] = (float)src[i] * g_window[i];
        }

      /* pad_end=False，FFT 512 > 窗长 480：尾部补零到 512。 */
      memset(g_fft_re + KWS_FRONTEND_FRAME_SAMPLES, 0,
             (KWS_FRONTEND_FFT_SIZE - KWS_FRONTEND_FRAME_SAMPLES) *
             sizeof(float));

      fft(g_fft_re, g_fft_im);

      /* 幅度谱，不是功率谱。 */
      for (i = 0; i < KWS_FFT_BINS; i++)
        {
          g_magnitude[i] = sqrtf(g_fft_re[i] * g_fft_re[i] +
                                 g_fft_im[i] * g_fft_im[i]);
        }

      for (mel = 0; mel < KWS_FRONTEND_MEL_BINS; mel++)
        {
          float energy = 0.0f;

          for (i = 0; i < KWS_FFT_BINS; i++)
            {
              energy += g_mel_weights[mel][i] * g_magnitude[i];
            }

          /* clip((ln(mel + 1e-6) + 12) * 1.625, 0, 26) */
          g_base[frame][mel] =
              clip_feature((logf(energy + KWS_LOG_EPSILON) + KWS_LOG_OFFSET) *
                           KWS_LOG_SCALE);
        }
    }

  /* 一阶差分：d[0]=0，d[t]=base[t]-base[t-1]。 */
  for (mel = 0; mel < KWS_FRONTEND_MEL_BINS; mel++)
    {
      g_delta[0][mel] = 0.0f;
    }

  for (t = 1; t < KWS_FRONTEND_TIME_STEPS; t++)
    {
      for (mel = 0; mel < KWS_FRONTEND_MEL_BINS; mel++)
        {
          g_delta[t][mel] = g_base[t][mel] - g_base[t - 1][mel];
        }
    }

  /* 二阶差分：d2[0]=0，d2[t]=d[t]-d[t-1]。 */
  for (mel = 0; mel < KWS_FRONTEND_MEL_BINS; mel++)
    {
      g_delta2[0][mel] = 0.0f;
    }

  for (t = 1; t < KWS_FRONTEND_TIME_STEPS; t++)
    {
      for (mel = 0; mel < KWS_FRONTEND_MEL_BINS; mel++)
        {
          g_delta2[t][mel] = g_delta[t][mel] - g_delta[t - 1][mel];
        }
    }

  /* 量化并写入 NHWC：out[(t*40+mel)*3 + ch]。 */
  for (t = 0; t < KWS_FRONTEND_TIME_STEPS; t++)
    {
      for (mel = 0; mel < KWS_FRONTEND_MEL_BINS; mel++)
        {
          int base_index = (t * KWS_FRONTEND_MEL_BINS + mel) *
                           KWS_FRONTEND_STACK;

          out[base_index + 0] = quantize_feature(g_base[t][mel],
                                                 quant_scale, quant_zero_point);
          out[base_index + 1] = quantize_feature(g_delta[t][mel],
                                                 quant_scale, quant_zero_point);
          out[base_index + 2] = quantize_feature(g_delta2[t][mel],
                                                 quant_scale, quant_zero_point);
        }
    }

  return 0;
}
