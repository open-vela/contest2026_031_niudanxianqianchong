/****************************************************************************
 * demos/smart_home/src/voice/kws/kws_infer.cc
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "kws_infer.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <new>

#include "model.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 后端由构建配置决定；日志需要如实反映，不能硬编码成 reference。 */
#if defined(CONFIG_TFLITEMICRO_ESP_NN)
#  define KWS_INFER_BUILD_BACKEND KWS_INFER_BACKEND_ESPNN
#else
#  define KWS_INFER_BUILD_BACKEND KWS_INFER_BACKEND_REFERENCE
#endif

/* arena 需要 16 字节对齐：bulk 堆不保证，MicroInterpreter 也不会补齐。 */
#define KWS_ARENA_ALIGN 16u

/****************************************************************************
 * Private Types
 ****************************************************************************/

using KwsResolver = tflite::MicroMutableOpResolver<9>;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static tflite::MicroInterpreter *g_interpreter = nullptr;
static TfLiteTensor *g_input = nullptr;
static TfLiteTensor *g_output = nullptr;
static size_t g_arena_used = 0;

/* 解释器保存 resolver 引用与注册项指针，因此 resolver 必须存活到解释器
 * 销毁之后。原实现把 resolver 放在 init 的栈上，首次 Invoke 即产生
 * stack-use-after-return。这里改用静态存储 + placement new。
 */
static KwsResolver *g_resolver = nullptr;
alignas(KwsResolver) static uint8_t g_resolver_storage[sizeof(KwsResolver)];

/* 解释器存储同理：静态存储避免函数局部动态初始化的 guard 依赖。 */
alignas(8) static uint8_t g_interpreter_storage[sizeof(tflite::MicroInterpreter)];

/* 已初始化的 arena，用于所有权判定。 */
static void *g_arena = nullptr;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int tensor_elements(const TfLiteTensor *tensor)
{
  int count = 1;
  int index;

  if (tensor == nullptr || tensor->dims == nullptr)
    {
      return 0;
    }

  for (index = 0; index < tensor->dims->size; index++)
    {
      count *= tensor->dims->data[index];
    }

  return count;
}

/* 反初始化：显式析构，顺序必须是「先解释器、后 resolver」。
 *
 * 解释器析构仍会读取 resolver 的注册项与 arena，所以不能反过来。
 * 也不能只把指针置空——那样 arena 引用会残留在已释放的内存上，
 * 服务重启时表现为读到野指针。
 */
static void release_instance(void)
{
  if (g_interpreter != nullptr)
    {
      g_interpreter->~MicroInterpreter();
      g_interpreter = nullptr;
    }

  if (g_resolver != nullptr)
    {
      g_resolver->~KwsResolver();
      g_resolver = nullptr;
    }

  g_input = nullptr;
  g_output = nullptr;
  g_arena = nullptr;
  g_arena_used = 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

extern "C" int kws_infer_init(void *arena, size_t arena_size)
{
  const tflite::Model *model;
  uintptr_t base;
  uintptr_t aligned;
  size_t usable;

  if (arena == nullptr || arena_size < 32)
    {
      return -EINVAL;
    }

  /* 同一 arena 重复初始化幂等；其他 arena 必须先 deinit，否则后续
   * deinit / 释放会把别人的内存当自己的。 */
  if (g_interpreter != nullptr)
    {
      return g_arena == arena ? 0 : -EBUSY;
    }

  base = (uintptr_t)arena;
  aligned = (base + (KWS_ARENA_ALIGN - 1u)) &
            ~(uintptr_t)(KWS_ARENA_ALIGN - 1u);
  usable = arena_size - (size_t)(aligned - base);

  model = tflite::GetModel(g_s3_model);
  if (model == nullptr || model->version() != TFLITE_SCHEMA_VERSION)
    {
      return -EINVAL;
    }

  g_resolver = new (g_resolver_storage) KwsResolver();
  if (g_resolver->AddShape() != kTfLiteOk ||
      g_resolver->AddStridedSlice() != kTfLiteOk ||
      g_resolver->AddPack() != kTfLiteOk ||
      g_resolver->AddReshape() != kTfLiteOk ||
      g_resolver->AddConv2D() != kTfLiteOk ||
      g_resolver->AddDepthwiseConv2D() != kTfLiteOk ||
      g_resolver->AddMean() != kTfLiteOk ||
      g_resolver->AddFullyConnected() != kTfLiteOk ||
      g_resolver->AddSoftmax() != kTfLiteOk)
    {
      release_instance();
      return -EINVAL;
    }

  g_interpreter = new (g_interpreter_storage)
      tflite::MicroInterpreter(model, *g_resolver, (uint8_t *)aligned, usable);
  g_arena = arena;

  if (g_interpreter->AllocateTensors() != kTfLiteOk)
    {
      release_instance();
      return -ENOMEM;
    }

  g_arena_used = g_interpreter->arena_used_bytes();

  g_input = g_interpreter->input(0);
  g_output = g_interpreter->output(0);
  if (g_input == nullptr || g_output == nullptr ||
      g_input->type != kTfLiteInt8 || g_output->type != kTfLiteInt8 ||
      tensor_elements(g_input) != KWS_INFER_FEATURE_SIZE ||
      tensor_elements(g_output) != KWS_INFER_CLASS_COUNT)
    {
      release_instance();
      return -EINVAL;
    }

  return 0;
}

extern "C" int kws_infer_deinit(void *arena)
{
  /* arena 非空时必须匹配当前实例，避免误销毁别人的解释器。 */
  if (arena != nullptr && g_interpreter != nullptr && g_arena != arena)
    {
      return -EBUSY;
    }

  release_instance();
  return 0;
}

extern "C" int kws_infer_ready(void)
{
  return g_interpreter != nullptr ? 1 : 0;
}

extern "C" size_t kws_infer_arena_used(void)
{
  return g_arena_used;
}

extern "C" int kws_infer_backend(void)
{
  return KWS_INFER_BUILD_BACKEND;
}

extern "C" void kws_infer_input_params(float *scale, int *zero_point)
{
  if (scale != nullptr)
    {
      *scale = g_input != nullptr ? g_input->params.scale : 1.0f;
    }

  if (zero_point != nullptr)
    {
      *zero_point = g_input != nullptr ? g_input->params.zero_point : 0;
    }
}

extern "C" int kws_infer_run(const int8_t *features, float *scores)
{
  int index;

  if (g_interpreter == nullptr || g_input == nullptr || g_output == nullptr ||
      features == nullptr || scores == nullptr)
    {
      return -EINVAL;
    }

  memcpy(g_input->data.int8, features, KWS_INFER_FEATURE_SIZE);
  if (g_interpreter->Invoke() != kTfLiteOk)
    {
      return -EIO;
    }

  for (index = 0; index < KWS_INFER_CLASS_COUNT; index++)
    {
      scores[index] = (g_output->data.int8[index] -
                       g_output->params.zero_point) *
                      g_output->params.scale;
    }

  return 0;
}
