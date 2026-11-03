/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// ESP-NN dispatch implementation adapted for the openvela TFLM port.

#include "tensorflow/lite/micro/kernels/conv.h"

#include <cstdint>
#include <cstring>

#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/kernels/internal/portable_tensor_utils.h"
#include "tensorflow/lite/kernels/internal/reference/conv.h"
#include "tensorflow/lite/kernels/internal/reference/integer_ops/conv.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/micro/micro_common.h"
#include "tensorflow/lite/micro/kernels/kernel_util.h"
#include "tensorflow/lite/micro/micro_log.h"

#if ESP_NN
#include <esp_nn.h>
#endif


namespace tflite {
namespace {

struct NodeData {
  OpDataConv op_data;
#if ESP_NN
  int buffer_idx;
  int verify_buffer_idx;
  int output_tensor_id;
  int8_t* esp_nn_filter_data;
  bool esp_nn_candidate;
  bool use_esp_nn;
  bool uses_aligned_filter_copy;
  bool runtime_trace_logged;
#endif
};

static void* Init(TfLiteContext* context, const char* buffer, size_t length) {
  TFLITE_DCHECK(context->AllocatePersistentBuffer != nullptr);
  return context->AllocatePersistentBuffer(context, sizeof(NodeData));
}

#if ESP_NN
static bool IsAligned(const void* pointer, uintptr_t alignment) {
  return pointer != nullptr &&
         (reinterpret_cast<uintptr_t>(pointer) & (alignment - 1)) == 0;
}

static int TensorId(const TfLiteIntArray* tensors, int position) {
  return tensors != nullptr && position < tensors->size ?
             tensors->data[position] :
             -1;
}

static bool IsEspNnConvOutputSelected(int output_tensor_id) {
  if (output_tensor_id == CONFIG_TFLITEMICRO_ESP_NN_CONV2D_OUTPUT_TENSOR) {
    return true;
  }

  if (output_tensor_id < 0 || output_tensor_id >= 32) {
    return false;
  }

  return (static_cast<uint32_t>(
              CONFIG_TFLITEMICRO_ESP_NN_CONV2D_OUTPUT_TENSOR_MASK) &
          (static_cast<uint32_t>(1) << output_tensor_id)) != 0;
}

static const char* GetEspNnConvCandidateReason(
    const TfLiteTensor* input, const TfLiteTensor* filter,
    const TfLiteTensor* output, const TfLiteConvParams& params,
    const OpDataConv& op_data) {
  if (input->type != kTfLiteInt8 || filter->type != kTfLiteInt8 ||
      output->type != kTfLiteInt8) {
    return "type";
  }

  if (input->dims->size != 4 || filter->dims->size != 4 ||
      output->dims->size != 4 || input->dims->data[0] != 1 ||
      output->dims->data[0] != 1) {
    return "shape";
  }

  if (filter->dims->data[3] != input->dims->data[3]) {
    return "grouped";
  }

  if (params.dilation_width_factor != 1 ||
      params.dilation_height_factor != 1) {
    return "dilation";
  }

#ifndef CONFIG_TFLITEMICRO_ESP_NN_CONV2D_GENERAL
  /*
   * The default bring-up path is deliberately limited to the smallest,
   * well-understood ESP-NN fast path. General Conv2D dispatch, including
   * S3 im2col for small input-channel kernels, requires explicit opt-in.
   */
  if (filter->dims->data[1] != 1 || filter->dims->data[2] != 1) {
    return "kernel";
  }

  if (params.stride_width != 1 || params.stride_height != 1) {
    return "stride";
  }

  if (op_data.padding.width != 0 || op_data.padding.height != 0) {
    return "padding";
  }

  if (input->dims->data[3] % 8 != 0) {
    return "input_channels";
  }
#endif

  return nullptr;
}

static const char* GetEspNnConvRuntimeReason(const void* input,
                                              const void* filter,
                                              const void* output,
                                              const void* scratch) {
  if (!IsAligned(input, 16)) {
    return "input_alignment";
  }

  if (!IsAligned(filter, 8)) {
    return "filter_alignment";
  }

  if (!IsAligned(output, 16)) {
    return "output_alignment";
  }

  if (!IsAligned(scratch, 16)) {
    return "scratch_alignment";
  }

  return nullptr;
}

static void TraceConvPrepare(const TfLiteNode* node, const TfLiteTensor* input,
                             const TfLiteTensor* filter,
                             const TfLiteTensor* output,
                             const TfLiteConvParams& params,
                             const NodeData& data,
                             const char* reason) {
#ifdef CONFIG_TFLITEMICRO_ESP_NN_TRACE
  MicroPrintf(
      "[espnn-trace] Conv2D in_t=%d out_t=%d in=[%d,%d,%d,%d] "
      "filter=[%d,%d,%d,%d] out=[%d,%d,%d,%d] stride=%dx%d pad=%dx%d "
      "dilation=%dx%d model_filter_mod8=%d esp_filter_mod8=%d "
      "filter_copy=%d candidate=%d selected=%d reason=%s",
      TensorId(node->inputs, kConvInputTensor),
      TensorId(node->outputs, kConvOutputTensor), input->dims->data[0],
      input->dims->data[1], input->dims->data[2], input->dims->data[3],
      filter->dims->data[0], filter->dims->data[1], filter->dims->data[2],
      filter->dims->data[3], output->dims->data[0], output->dims->data[1],
      output->dims->data[2], output->dims->data[3], params.stride_width,
      params.stride_height, data.op_data.padding.width,
      data.op_data.padding.height, params.dilation_width_factor,
      params.dilation_height_factor,
      static_cast<int>(reinterpret_cast<uintptr_t>(filter->data.int8) & 7),
      static_cast<int>(reinterpret_cast<uintptr_t>(data.esp_nn_filter_data) &
                       7),
      data.uses_aligned_filter_copy ? 1 : 0,
      data.esp_nn_candidate ? 1 : 0, data.use_esp_nn ? 1 : 0,
      reason == nullptr ? "ok" : reason);
#endif
}
#endif

static TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node) {
  TFLITE_DCHECK(node->user_data != nullptr);
  TFLITE_DCHECK(node->builtin_data != nullptr);

  NodeData* data = static_cast<NodeData*>(node->user_data);
  const auto& params =
      *(static_cast<const TfLiteConvParams*>(node->builtin_data));

  MicroContext* micro_context = GetMicroContext(context);

  TfLiteTensor* output =
      micro_context->AllocateTempOutputTensor(node, kConvOutputTensor);
  TF_LITE_ENSURE(context, output != nullptr);
  TfLiteTensor* input =
      micro_context->AllocateTempInputTensor(node, kConvInputTensor);
  TF_LITE_ENSURE(context, input != nullptr);
  TfLiteTensor* filter =
      micro_context->AllocateTempInputTensor(node, kConvWeightsTensor);
  TF_LITE_ENSURE(context, filter != nullptr);

  // Check input channels matching filter
  const int input_channels = input->dims->data[3];
  const int filter_input_channels = filter->dims->data[3];
  TF_LITE_ENSURE(context, filter_input_channels > 0);
  TF_LITE_ENSURE_EQ(context, input_channels % filter_input_channels, 0);

  TF_LITE_ENSURE_EQ(context, input->type, output->type);
  TF_LITE_ENSURE_MSG(
      context,
      (input->type == kTfLiteFloat32 && filter->type == kTfLiteFloat32) ||
          (input->type == kTfLiteInt16 && filter->type == kTfLiteInt8) ||
          (input->type == kTfLiteInt8 &&
           (filter->type == kTfLiteInt4 || filter->type == kTfLiteInt8)),
      "Hybrid models are not supported on TFLite Micro.");

  const int input_width = input->dims->data[2];
  const int input_height = input->dims->data[1];
  const int filter_width = filter->dims->data[2];
  const int filter_height = filter->dims->data[1];
  const int output_width = output->dims->data[2];
  const int output_height = output->dims->data[1];

  // Dynamically allocate per-channel quantization parameters.
  const int num_channels = filter->dims->data[kConvQuantizedDimension];
  data->op_data.per_channel_output_multiplier =
      static_cast<int32_t*>(context->AllocatePersistentBuffer(
          context, num_channels * sizeof(int32_t)));
  data->op_data.per_channel_output_shift =
      static_cast<int32_t*>(context->AllocatePersistentBuffer(
          context, num_channels * sizeof(int32_t)));

  // All per-channel quantized tensors need valid zero point and scale arrays.
  if (input->type == kTfLiteInt8 || input->type == kTfLiteInt16) {
    TF_LITE_ENSURE_EQ(context, filter->quantization.type,
                      kTfLiteAffineQuantization);

    const auto* affine_quantization =
        static_cast<TfLiteAffineQuantization*>(filter->quantization.params);
    TFLITE_DCHECK(affine_quantization != nullptr);
    TFLITE_DCHECK(affine_quantization->scale != nullptr);
    TFLITE_DCHECK(affine_quantization->zero_point != nullptr);

    TF_LITE_ENSURE(context,
                   affine_quantization->scale->size == 1 ||
                       affine_quantization->scale->size ==
                           filter->dims->data[kConvQuantizedDimension]);
  }

  TF_LITE_ENSURE_STATUS(CalculateOpDataConv(
      context, node, params, input_width, input_height, filter_width,
      filter_height, output_width, output_height, input->type, &data->op_data));

#if ESP_NN
  data->buffer_idx = -1;
  data->verify_buffer_idx = -1;
  data->output_tensor_id = TensorId(node->outputs, kConvOutputTensor);
  data->esp_nn_filter_data = filter->data.int8;
  data->uses_aligned_filter_copy = false;
  data->runtime_trace_logged = false;

  const char* esp_nn_reason = GetEspNnConvCandidateReason(
      input, filter, output, params, data->op_data);
  data->esp_nn_candidate = esp_nn_reason == nullptr;
  data->use_esp_nn =
      data->esp_nn_candidate &&
      IsEspNnConvOutputSelected(data->output_tensor_id);

  if (data->use_esp_nn) {
    constexpr uintptr_t kEspNnFilterAlignment = 8;
    TF_LITE_ENSURE(context, filter->bytes > 0);
    uint8_t* filter_copy_storage = static_cast<uint8_t*>(
        context->AllocatePersistentBuffer(
            context, filter->bytes + kEspNnFilterAlignment - 1));
    TF_LITE_ENSURE(context, filter_copy_storage != nullptr);
    uintptr_t aligned_filter =
        (reinterpret_cast<uintptr_t>(filter_copy_storage) +
         kEspNnFilterAlignment - 1) &
        ~(kEspNnFilterAlignment - 1);
    data->esp_nn_filter_data = reinterpret_cast<int8_t*>(aligned_filter);
    std::memcpy(data->esp_nn_filter_data, filter->data.int8, filter->bytes);
    data->uses_aligned_filter_copy = true;

    data_dims_t input_dims = {
        .width = input_width, .height = input_height,
        .channels = input_channels, .extra = 1};
    data_dims_t output_dims = {
        .width = output_width, .height = output_height,
        .channels = output->dims->data[3], .extra = 1};
    data_dims_t filter_dims = {
        .width = filter_width, .height = filter_height,
        .channels = filter->dims->data[3], .extra = 0};
    conv_params_t conv_params = {
        .in_offset = 0, .out_offset = 0,
        .stride = {params.stride_width, params.stride_height},
        .padding = {data->op_data.padding.width, data->op_data.padding.height},
        .dilation = {0, 0}, .activation = {-128, 127}};
    int scratch_size = esp_nn_get_conv_scratch_size(
        &input_dims, &filter_dims, &output_dims, &conv_params);

    if (scratch_size > 0) {
      TF_LITE_ENSURE_STATUS(context->RequestScratchBufferInArena(
          context, scratch_size, &data->buffer_idx));
    }
#ifdef CONFIG_TFLITEMICRO_ESP_NN_CONV2D_VERIFY
    TF_LITE_ENSURE_STATUS(context->RequestScratchBufferInArena(
        context, output->bytes, &data->verify_buffer_idx));
#endif
  }

  TraceConvPrepare(node, input, filter, output, params, *data,
                   esp_nn_reason);
#endif

  if (filter->type == kTfLiteInt4) {
    int filter_size =
        RuntimeShape(filter->dims->size,
                     reinterpret_cast<const int32_t*>(filter->dims->data))
            .FlatSize();
    context->RequestScratchBufferInArena(context, filter_size,
                                         &data->op_data.filter_buffer_index);
  }

  micro_context->DeallocateTempTfLiteTensor(output);
  micro_context->DeallocateTempTfLiteTensor(input);
  micro_context->DeallocateTempTfLiteTensor(filter);

  return kTfLiteOk;
}

#if ESP_NN
static void EvalReferenceQuantizedPerChannel(
    const TfLiteConvParams& params, const NodeData& data,
    const TfLiteEvalTensor* input, const TfLiteEvalTensor* filter,
    const TfLiteEvalTensor* bias, TfLiteEvalTensor* output,
    int8_t* output_data) {
  reference_integer_ops::ConvPerChannel(
      ConvParamsQuantized(params, data.op_data),
      data.op_data.per_channel_output_multiplier,
      data.op_data.per_channel_output_shift,
      tflite::micro::GetTensorShape(input),
      tflite::micro::GetTensorData<int8_t>(input),
      tflite::micro::GetTensorShape(filter),
      tflite::micro::GetTensorData<int8_t>(filter),
      tflite::micro::GetTensorShape(bias),
      tflite::micro::GetOptionalTensorData<int32_t>(bias),
      tflite::micro::GetTensorShape(output), output_data);
}

// Fixed-point per-channel-quantization convolution Int8 function wrapper.
static void EvalEspNnQuantizedPerChannel(
    TfLiteContext* context, TfLiteNode* node, const TfLiteConvParams& params,
    const NodeData& data, const TfLiteEvalTensor* input,
    const TfLiteEvalTensor* filter, const TfLiteEvalTensor* bias,
    TfLiteEvalTensor* output) {
  const int dilation_width_factor = params.dilation_width_factor;
  const int dilation_height_factor = params.dilation_height_factor;

  if (dilation_width_factor == 1 && dilation_height_factor == 1) {
    // Get parameters.
    RuntimeShape filter_shape = tflite::micro::GetTensorShape(filter);
    RuntimeShape input_shape = tflite::micro::GetTensorShape(input);
    RuntimeShape output_shape = tflite::micro::GetTensorShape(output);
    RuntimeShape bias_shape = tflite::micro::GetTensorShape(bias);

    const int8_t *input_data = tflite::micro::GetTensorData<int8_t>(input);
    int8_t *output_data = tflite::micro::GetTensorData<int8_t>(output);

    const int32_t input_offset = -data.op_data.input_zero_point;
    const int32_t output_offset = data.op_data.output_zero_point;
    const int stride_width = params.stride_width;
    const int stride_height = params.stride_height;
    const int pad_width = data.op_data.padding.width;
    const int pad_height = data.op_data.padding.height;

    const int input_height = input_shape.Dims(1);
    const int input_width = input_shape.Dims(2);
    const int filter_height = filter_shape.Dims(1);
    const int filter_width = filter_shape.Dims(2);
    const int output_height = output_shape.Dims(1);
    const int output_width = output_shape.Dims(2);

    // Set min and max value of the output.
    const int32_t activation_min = data.op_data.output_activation_min;
    const int32_t activation_max = data.op_data.output_activation_max;

    // Consistency check.
    TFLITE_DCHECK_LE(activation_min, activation_max);
    TFLITE_DCHECK_EQ(input_shape.DimensionsCount(), 4);
    TFLITE_DCHECK_EQ(filter_shape.DimensionsCount(), 4);
    TFLITE_DCHECK_EQ(output_shape.DimensionsCount(), 4);
    const int batch_size = MatchingDim(input_shape, 0, output_shape, 0);
    const int input_depth = MatchingDim(input_shape, 3, filter_shape, 3);
    const int output_depth = MatchingDim(filter_shape, 0, output_shape, 3);

    if (tflite::micro::GetTensorData<int8_t>(bias)) {
      TFLITE_DCHECK_EQ(bias_shape.FlatSize(), output_depth);
    }

    void *scratch_buf = NULL;
    if (data.buffer_idx > -1) {
      scratch_buf = context->GetScratchBuffer(context, data.buffer_idx);
    }
    /* Only the nodes that actually dispatch to ESP-NN may touch the
     * kernel-global scratch pointer: an unconditional call lets a
     * reference-kernel node overwrite it with NULL, and the next ESP-NN
     * node then runs with no scratch at all. */
    if (data.use_esp_nn) {
      esp_nn_set_conv_scratch_buf(scratch_buf);
    }

    const int input_size = input_width * input_height * input_depth;
    const int output_size = output_width * output_height * output_depth;

    data_dims_t input_dims =  {
                                .width = input_width, .height = input_height,
                                .channels = input_depth, .extra = 1
                              };
    data_dims_t output_dims = {
                                .width = output_width, .height = output_height,
                                .channels = output_depth, .extra = 1
                              };
    data_dims_t filter_dims = {
                                .width = filter_width, .height = filter_height,
                                .channels = filter->dims->data[3], .extra = 0
                              };
    conv_params_t conv_params = {
                                  .in_offset = input_offset, .out_offset = output_offset,
                                  .stride = {stride_width, stride_height},
                                  .padding = {pad_width, pad_height},
                                  .dilation = {0, 0},
                                  .activation = {activation_min, activation_max}
                                };
    quant_data_t quant_data = {
                                .shift = data.op_data.per_channel_output_shift,
                                .mult = data.op_data.per_channel_output_multiplier
                              };
    const int32_t* bias_data =
        tflite::micro::GetOptionalTensorData<int32_t>(bias);

    for (int i_batch = 0; i_batch < batch_size; i_batch++) {
#ifdef CONFIG_TFLITEMICRO_ESP_NN_TRACE
      MicroPrintf(
          "[espnn-trace] Conv2D out_t=%d esp-nn enter input=0x%08x "
          "filter=0x%08x bias=0x%08x output=0x%08x scratch=0x%08x "
          "mult=0x%08x shift=0x%08x batch=%d",
          data.output_tensor_id,
          static_cast<unsigned int>(reinterpret_cast<uintptr_t>(
              input_data + i_batch * input_size)),
          static_cast<unsigned int>(
              reinterpret_cast<uintptr_t>(data.esp_nn_filter_data)),
          static_cast<unsigned int>(reinterpret_cast<uintptr_t>(bias_data)),
          static_cast<unsigned int>(reinterpret_cast<uintptr_t>(
              output_data + i_batch * output_size)),
          static_cast<unsigned int>(reinterpret_cast<uintptr_t>(scratch_buf)),
          static_cast<unsigned int>(reinterpret_cast<uintptr_t>(quant_data.mult)),
          static_cast<unsigned int>(
              reinterpret_cast<uintptr_t>(quant_data.shift)),
          i_batch);
#endif
      esp_nn_conv_s8(&input_dims, input_data + i_batch * input_size,
                     &filter_dims, data.esp_nn_filter_data, bias_data,
                     &output_dims, output_data + i_batch * output_size,
                     &conv_params, &quant_data);
#ifdef CONFIG_TFLITEMICRO_ESP_NN_TRACE
      MicroPrintf("[espnn-trace] Conv2D out_t=%d esp-nn return batch=%d",
                  data.output_tensor_id, i_batch);
#endif
    }
  }
}
#endif

static TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  const TfLiteEvalTensor* input =
      tflite::micro::GetEvalInput(context, node, kConvInputTensor);
  const TfLiteEvalTensor* filter =
      tflite::micro::GetEvalInput(context, node, kConvWeightsTensor);
  const TfLiteEvalTensor* bias =
      (NumInputs(node) == 3)
          ? tflite::micro::GetEvalInput(context, node, kConvBiasTensor)
          : nullptr;
  TfLiteEvalTensor* output =
      tflite::micro::GetEvalOutput(context, node, kConvOutputTensor);

  TFLITE_DCHECK(node->builtin_data != nullptr);
  const auto& params =
      *(reinterpret_cast<TfLiteConvParams*>(node->builtin_data));
  TFLITE_DCHECK(node->user_data != nullptr);
  auto& data = *(static_cast<NodeData*>(node->user_data));

#if ESP_NN
  void* scratch = data.buffer_idx >= 0 ?
                      context->GetScratchBuffer(context, data.buffer_idx) :
                      nullptr;
  const char* runtime_reason = data.use_esp_nn ?
      GetEspNnConvRuntimeReason(tflite::micro::GetTensorData<int8_t>(input),
                                 data.esp_nn_filter_data,
                                 tflite::micro::GetTensorData<int8_t>(output),
                                 scratch) :
      "reference";
#ifdef CONFIG_TFLITEMICRO_ESP_NN_TRACE
  if (!data.runtime_trace_logged) {
    MicroPrintf(
        "[espnn-trace] Conv2D out_t=%d input_mod16=%d "
        "model_filter_mod8=%d esp_filter_mod8=%d output_mod16=%d "
        "scratch_mod16=%d filter_copy=%d backend=%s reason=%s",
        data.output_tensor_id,
        static_cast<int>(reinterpret_cast<uintptr_t>(
                             tflite::micro::GetTensorData<int8_t>(input)) & 15),
        static_cast<int>(reinterpret_cast<uintptr_t>(
                             tflite::micro::GetTensorData<int8_t>(filter)) & 7),
        static_cast<int>(reinterpret_cast<uintptr_t>(
                             data.esp_nn_filter_data) & 7),
        static_cast<int>(reinterpret_cast<uintptr_t>(
                             tflite::micro::GetTensorData<int8_t>(output)) & 15),
        static_cast<int>(reinterpret_cast<uintptr_t>(scratch) & 15),
        data.uses_aligned_filter_copy ? 1 : 0,
        runtime_reason == nullptr ? "esp-nn" : "reference",
        runtime_reason == nullptr ? "ok" : runtime_reason);
    data.runtime_trace_logged = true;
  }
#endif
#endif

  switch (input->type) {  // Already know in/out types are same.
    case kTfLiteFloat32: {
      tflite::reference_ops::Conv(
          ConvParamsFloat(params, data.op_data),
          tflite::micro::GetTensorShape(input),
          tflite::micro::GetTensorData<float>(input),
          tflite::micro::GetTensorShape(filter),
          tflite::micro::GetTensorData<float>(filter),
          tflite::micro::GetTensorShape(bias),
          tflite::micro::GetOptionalTensorData<float>(bias),
          tflite::micro::GetTensorShape(output),
          tflite::micro::GetTensorData<float>(output),
          tflite::micro::GetTensorShape(nullptr), nullptr);
      break;
    }
    case kTfLiteInt16: {
      if (bias == nullptr || bias->type == kTfLiteInt32) {
        reference_integer_ops::ConvPerChannel(
            ConvParamsQuantized(params, data.op_data),
            data.op_data.per_channel_output_multiplier,
            data.op_data.per_channel_output_shift,
            tflite::micro::GetTensorShape(input),
            tflite::micro::GetTensorData<int16_t>(input),
            tflite::micro::GetTensorShape(filter),
            tflite::micro::GetTensorData<int8_t>(filter),
            tflite::micro::GetTensorShape(bias),
            tflite::micro::GetOptionalTensorData<std::int32_t>(bias),
            tflite::micro::GetTensorShape(output),
            tflite::micro::GetTensorData<int16_t>(output));
      } else if (bias->type == kTfLiteInt64) {
        reference_integer_ops::ConvPerChannel(
            ConvParamsQuantized(params, data.op_data),
            data.op_data.per_channel_output_multiplier,
            data.op_data.per_channel_output_shift,
            tflite::micro::GetTensorShape(input),
            tflite::micro::GetTensorData<int16_t>(input),
            tflite::micro::GetTensorShape(filter),
            tflite::micro::GetTensorData<int8_t>(filter),
            tflite::micro::GetTensorShape(bias),
            tflite::micro::GetOptionalTensorData<std::int64_t>(bias),
            tflite::micro::GetTensorShape(output),
            tflite::micro::GetTensorData<int16_t>(output));
      } else {
        MicroPrintf("Bias type %s (%d) not supported.",
                    TfLiteTypeGetName(bias->type), bias->type);
        return kTfLiteError;
      }
      break;
    }
    case kTfLiteInt8: {
      switch (filter->type) {
        case kTfLiteInt4: {
          int8_t* unpacked_filter_data = static_cast<int8_t*>(
              context->GetScratchBuffer(context, data.op_data.filter_buffer_index));
          tflite::tensor_utils::UnpackDenseInt4IntoInt8(
              tflite::micro::GetTensorData<int8_t>(filter),
              tflite::micro::GetTensorShape(filter).FlatSize(),
              unpacked_filter_data);
          reference_integer_ops::ConvPerChannel(
              ConvParamsQuantized(params, data.op_data),
              data.op_data.per_channel_output_multiplier,
              data.op_data.per_channel_output_shift,
              tflite::micro::GetTensorShape(input),
              tflite::micro::GetTensorData<int8_t>(input),
              tflite::micro::GetTensorShape(filter), unpacked_filter_data,
              tflite::micro::GetTensorShape(bias),
              tflite::micro::GetOptionalTensorData<int32_t>(bias),
              tflite::micro::GetTensorShape(output),
              tflite::micro::GetTensorData<int8_t>(output));
          break;
        }
        case kTfLiteInt8: {
#if ESP_NN
          if (data.use_esp_nn && runtime_reason == nullptr) {
#ifdef CONFIG_TFLITEMICRO_ESP_NN_CONV2D_VERIFY
            int8_t* reference_output = static_cast<int8_t*>(
                context->GetScratchBuffer(context, data.verify_buffer_idx));
            EvalReferenceQuantizedPerChannel(
                params, data, input, filter, bias, output, reference_output);
            EvalEspNnQuantizedPerChannel(context, node, params, data, input,
                                         filter, bias, output);

            const size_t output_bytes =
                tflite::micro::GetTensorShape(output).FlatSize() *
                sizeof(int8_t);
            int mismatch = -1;
            int8_t* esp_nn_output = tflite::micro::GetTensorData<int8_t>(output);
            for (size_t i = 0; i < output_bytes; i++) {
              if (reference_output[i] != esp_nn_output[i]) {
                mismatch = static_cast<int>(i);
                break;
              }
            }

            if (mismatch >= 0) {
              MicroPrintf(
                  "[espnn-verify] Conv2D out_t=%d mismatch=%d ref=%d esp=%d; "
                  "restoring reference output",
                  data.output_tensor_id, mismatch, reference_output[mismatch],
                  esp_nn_output[mismatch]);
              std::memcpy(esp_nn_output, reference_output, output_bytes);
            } else {
              MicroPrintf("[espnn-verify] Conv2D out_t=%d match bytes=%d",
                          data.output_tensor_id,
                          static_cast<int>(output_bytes));
            }
#else
            EvalEspNnQuantizedPerChannel(context, node, params, data, input,
                                         filter, bias, output);
#endif
          } else {
            EvalReferenceQuantizedPerChannel(
                params, data, input, filter, bias, output,
                tflite::micro::GetTensorData<int8_t>(output));
          }
#else
          reference_integer_ops::ConvPerChannel(
              ConvParamsQuantized(params, data.op_data),
              data.op_data.per_channel_output_multiplier,
              data.op_data.per_channel_output_shift,
              tflite::micro::GetTensorShape(input),
              tflite::micro::GetTensorData<int8_t>(input),
              tflite::micro::GetTensorShape(filter),
              tflite::micro::GetTensorData<int8_t>(filter),
              tflite::micro::GetTensorShape(bias),
              tflite::micro::GetTensorData<int32_t>(bias),
              tflite::micro::GetTensorShape(output),
              tflite::micro::GetTensorData<int8_t>(output));
#endif
        break;
        }
        default:
          MicroPrintf("Weight type %s (%d) not supported.",
                      TfLiteTypeGetName(filter->type), filter->type);
          return kTfLiteError;
      }
      break;
    }
    default:
      MicroPrintf("Type %s (%d) not supported.", TfLiteTypeGetName(input->type),
                  input->type);
      return kTfLiteError;
  }
  return kTfLiteOk;
}

}  // namespace

TFLMRegistration Register_CONV_2D() {
  return tflite::micro::RegisterOp(Init, Prepare, Eval);
}

}  // namespace tflite
