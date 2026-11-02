/****************************************************************************
 * demos/smart_home/src/voice/smart_home_kws_frontend.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/* wake_large 特征前端。
 *
 * 模型输入契约：int8[124 × 40 × 3]，NHWC（时间 × mel × 3 通道）。
 *
 * 按训练实现（train_s3.py）的契约实现：
 *   PCM        16000 Hz，40000 点（2.5 s）
 *   STFT       窗长 480，步长 320，FFT 512，periodic Hann，pad_end=False
 *   频谱       幅度谱 abs(STFT)，257 bin
 *   mel        40 维，125–7500 Hz，对照 linear_to_mel_weight_matrix
 *   基础特征   clip((ln(mel + 1e-6) + 12) * 1.625, 0, 26)
 *   差分       d[1:]=base[1:]-base[:-1]；d2[1:]=d[1:]-d[:-1]，首帧为 0
 *   布局       124×40×3，每 (t, mel) 依次为 base、d、d2，共 14880 值
 *
 * 与旧实现的差异（旧值 → 新值）：
 *   Hann 窗        N-1（symmetric）→ N（periodic）
 *   mel 频带       20–7990 Hz → 125–7500 Hz
 *   频谱           功率谱 re²+im² → 幅度谱 sqrt(re²+im²)
 *   归一化         无 → (ln+12)*1.625 后 clip(0,26)
 *   第 3 通道      相邻 3 帧堆叠 → base/delta/delta2
 *   输入长度       40480 点 / 126 帧 → 40000 点 / 124 帧
 *
 * 已知缺口：mel 滤波器按 Hz 轴三角加权，而 TensorFlow 的
 * linear_to_mel_weight_matrix 在 mel 轴等距取三角，两者不同。
 * 本实现的权重按 mel 轴计算（见 kws_frontend.c），与训练一致。
 * 若要完全逐字节对齐，仍需用 golden 数据核对权重矩阵。
 *
 * 量化参数（scale/zero_point）从模型张量实时读取，非本文件职责。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KWS_FRONTEND_SAMPLE_RATE     16000
#define KWS_FRONTEND_FRAME_SAMPLES   480     /* 30 ms */
#define KWS_FRONTEND_HOP_SAMPLES     320     /* 20 ms */
#define KWS_FRONTEND_FFT_SIZE        512
#define KWS_FRONTEND_MEL_BINS        40
#define KWS_FRONTEND_MEL_FMIN_HZ     125.0f
#define KWS_FRONTEND_MEL_FMAX_HZ     7500.0f

/* 训练实现：pad_end=False，2.5 s 音频产生 124 帧。 */
#define KWS_FRONTEND_TIME_STEPS      124
#define KWS_FRONTEND_STACK           3

/* 124 帧、步长 320、窗长 480 需要 (124-1)*320+480 = 39840 点；
 * 训练固定输入 40000 点（2.5 s），尾部 160 点不参与任何帧。
 */
#define KWS_FRONTEND_PCM_SAMPLES     40000
#define KWS_FRONTEND_FEATURE_COUNT   (KWS_FRONTEND_TIME_STEPS * \
                                      KWS_FRONTEND_MEL_BINS * \
                                      KWS_FRONTEND_STACK)

/* 构建 mel 滤波器组与 FFT 表。返回 0 成功（幂等）。 */
int kws_frontend_init(void);

/* pcm 为 KWS_FRONTEND_PCM_SAMPLES 个 PCM16 单声道采样；
 * out 为 KWS_FRONTEND_FEATURE_COUNT 个 int8（已按传入的量化参数量化）。
 * 返回 0 成功，负值为 errno 风格错误码。 */
int kws_frontend_compute(const int16_t *pcm, float quant_scale,
                         int quant_zero_point, int8_t *out);

#ifdef __cplusplus
}
#endif
