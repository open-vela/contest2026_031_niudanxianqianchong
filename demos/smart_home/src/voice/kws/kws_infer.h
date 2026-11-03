/****************************************************************************
 * demos/smart_home/src/voice/kws/kws_infer.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/* wake_large int8 模型的 TFLite Micro 推理封装（C API）。
 *
 * 模型契约：输入 int8[124×40×3]，输出 int8 五分类
 * （wake / hard_neg / other_speech / background / silence）。
 * 后端由编译配置决定：默认 reference kernel，启用 ESP-NN 时走
 * Conv2D/DepthwiseConv2D 加速内核。
 *
 * 所有权：arena 由调用方分配并持有。初始化成功后解释器内部保存指向
 * arena 与 resolver 的指针，因此**必须先 kws_infer_deinit() 再释放 arena**。
 * 同一时刻只允许一个活实例；换 arena 需先 deinit。
 *
 * 线程模型：单实例接口，非线程安全，调用方自行串行化。
 * 所有权检查不等于线程安全，常驻服务与 smoke 不可并发调用。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KWS_INFER_FEATURE_SIZE (124 * 40 * 3)
#define KWS_INFER_CLASS_COUNT  5
#define KWS_INFER_CLASS_WAKE   0

/* 推理后端标识（仅用于日志与验收，不参与计算）。 */
#define KWS_INFER_BACKEND_REFERENCE 0
#define KWS_INFER_BACKEND_ESPNN     1

/* 初始化：加载模型、注册算子、AllocateTensors 并校验张量契约。
 *
 * arena 由调用方分配（建议 PSRAM、16 字节对齐）。
 * 同一 arena 重复调用返回 0；已被其他 arena 持有时返回 -EBUSY；
 * 失败时不留半初始化状态，调用方可直接释放 arena。
 * 返回 0 成功，负值为 errno 风格错误码。 */
int kws_infer_init(void *arena, size_t arena_size);

/* 销毁解释器并释放其对 arena 的所有引用。
 *
 * 必须在使用该 arena 的调用方释放 arena 之前调用。
 * arena 为 NULL 时无条件销毁；非 NULL 且与当前实例不匹配时返回 -EBUSY。
 * 可重复调用（幂等）。 */
int kws_infer_deinit(void *arena);

/* 当前是否已初始化。 */
int kws_infer_ready(void);

/* AllocateTensors 实际使用的 arena 字节数（init 成功后有效）。 */
size_t kws_infer_arena_used(void);

/* 本构建实际编入的推理后端（KWS_INFER_BACKEND_*）。 */
int kws_infer_backend(void);

/* 输入张量量化参数（供前端量化特征使用）。 */
void kws_infer_input_params(float *scale, int *zero_point);

/* 单次推理：features 为 KWS_INFER_FEATURE_SIZE 个 int8；
 * scores 输出 KWS_INFER_CLASS_COUNT 个去量化浮点分数。
 * 返回 0 成功。 */
int kws_infer_run(const int8_t *features, float *scores);

#ifdef __cplusplus
}
#endif
