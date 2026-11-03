/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/lite/micro/kernels/depthwise_conv.h"

#include <cstdint>
#include <cstring>

#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/kernels/internal/portable_tensor_utils.h"
#include "tensorflow/lite/kernels/internal/reference/depthwiseconv_float.h"
#include "tensorflow/lite/kernels/internal/reference/integer_ops/depthwise_conv.h"
#include "tensorflow/lite/kernels/internal/tensor_ctypes.h"
#include "tensorflow/lite/kernels/kernel_util.h"
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
  bool esp_nn_candidate;
  bool use_esp_nn;
  bool runtime_trace_logged;
#endif
};

void* Init(TfLiteContext* context, const char* buffer,
                        size_t length) {
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

static bool IsEspNnDepthwiseOutputSelected(int output_tensor_id) {
  if (output_tensor_id ==
      CONFIG_TFLITEMICRO_ESP_NN_DEPTHWISE_CONV2D_OUTPUT_TENSOR) {
    return true;
  }

  if (output_tensor_id < 0 || output_tensor_id >= 32) {
    return false;
  }

  return (static_cast<uint32_t>(
              CONFIG_TFLITEMICRO_ESP_NN_DEPTHWISE_CONV2D_OUTPUT_TENSOR_MASK) &
          (static_cast<uint32_t>(1) << output_tensor_id)) != 0;
}

static const char* GetEspNnDepthwiseCandidateReason(
    const TfLiteTensor* input, const TfLiteTensor* filter,
    const TfLiteTensor* output, const TfLiteDepthwiseConvParams& params,
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

  if (params.depth_multiplier != 1 ||
      filter->dims->data[3] != input->dims->data[3] ||
      output->dims->data[3] != input->dims->data[3]) {
    return "depth_multiplier";
  }

  if (params.dilation_width_factor != 1 ||
      params.dilation_height_factor != 1) {
    return "dilation";
  }

  /*
   * Admit only shapes assigned to model-specific hardware verify profiles.
   * The 4-class model uses 12/16 channels; S3-large uses the native
   * 32/64-channel padded s8 assembly paths. Selection by tensor ID remains
   * mandatory.
   */
  if (filter->dims->data[1] != 3 || filter->dims->data[2] != 3) {
    return "kernel";
  }

  if (params.stride_width != 1 || params.stride_height != 1) {
    return "stride";
  }

  if (op_data.padding.width != 1 || op_data.padding.height != 1) {
    return "padding";
  }

  const int channels = input->dims->data[3];
  if (channels != 12 && channels != 16 && channels != 32 && channels != 64) {
    return "channels";
  }

  return nullptr;
}

static const char* GetEspNnDepthwiseRuntimeReason(const void* input,
                                                   const void* output,
                                                   const void* scratch) {
  if (!IsAligned(input, 16)) {
    return "input_alignment";
  }

  if (!IsAligned(output, 16)) {
    return "output_alignment";
  }

  if (!IsAligned(scratch, 16)) {
    return "scratch_alignment";
  }

  return nullptr;
}

static void TraceDepthwisePrepare(
    const TfLiteNode* node, const TfLiteTensor* input,
    const TfLiteTensor* filter, const TfLiteTensor* output,
    const TfLiteDepthwiseConvParams& params, const NodeData& data,
    const char* reason) {
#ifdef CONFIG_TFLITEMICRO_ESP_NN_TRACE
  MicroPrintf(
      "[espnn-trace] DepthwiseConv2D in_t=%d out_t=%d "
      "in=[%d,%d,%d,%d] filter=[%d,%d,%d,%d] out=[%d,%d,%d,%d] "
      "stride=%dx%d pad=%dx%d dilation=%dx%d depth_mult=%d "
      "filter_mod8=%d candidate=%d selected=%d reason=%s",
      TensorId(node->inputs, kDepthwiseConvInputTensor),
      TensorId(node->outputs, kDepthwiseConvOutputTensor), input->dims->data[0],
      input->dims->data[1], input->dims->data[2], input->dims->data[3],
      filter->dims->data[0], filter->dims->data[1], filter->dims->data[2],
      filter->dims->data[3], output->dims->data[0], output->dims->data[1],
      output->dims->data[2], output->dims->data[3], params.stride_width,
      params.stride_height, data.op_data.padding.width,
      data.op_data.padding.height, params.dilation_width_factor,
      params.dilation_height_factor, params.depth_multiplier,
      static_cast<int>(reinterpret_cast<uintptr_t>(filter->data.int8) & 7),
      data.esp_nn_candidate ? 1 : 0, data.use_esp_nn ? 1 : 0,
      reason == nullptr ? "ok" : reason);
#endif
}
#endif

#if ESP_NN
inline void EvalQuantizedPerChannel(TfLiteContext* context, TfLiteNode* node,
                                    const TfLiteDepthwiseConvParams& params,
                                    const NodeData& data,
                                    const TfLiteEvalTensor* input,
                                    const TfLiteEvalTensor* filter,
                                    const TfLiteEvalTensor* bias,
                                    TfLiteEvalTensor* output) {
  const int dilation_width_factor = params.dilation_width_factor;
  const int dilation_height_factor = params.dilation_height_factor;

  if (dilation_width_factor == 1 && dilation_height_factor == 1) {
    // Get parameters.
    RuntimeShape input_shape = tflite::micro::GetTensorShape(input);
    RuntimeShape filter_shape = tflite::micro::GetTensorShape(filter);
    RuntimeShape output_shape = tflite::micro::GetTensorShape(output);
    RuntimeShape bias_shape = tflite::micro::GetTensorShape(bias);

    TFLITE_DCHECK_EQ(input_shape.DimensionsCount(), 4);
    TFLITE_DCHECK_EQ(filter_shape.DimensionsCount(), 4);
    TFLITE_DCHECK_EQ(output_shape.DimensionsCount(), 4);

    const int8_t *input_data = tflite::micro::GetTensorData<int8_t>(input);
    int8_t *output_data = tflite::micro::GetTensorData<int8_t>(output);

    const int depth_multiplier = params.depth_multiplier;
    const int32_t input_offset = -data.op_data.input_zero_point;
    const int32_t output_offset = data.op_data.output_zero_point;
    const int stride_width = params.stride_width;
    const int stride_height = params.stride_height;
    const int pad_width = data.op_data.padding.width;
    const int pad_height = data.op_data.padding.height;

    const int input_height = input_shape.Dims(1);
    const int input_width = input_shape.Dims(2);
    const int input_depth = input_shape.Dims(3);
    const int filter_height = filter_shape.Dims(1);
    const int filter_width = filter_shape.Dims(2);
    const int output_height = output_shape.Dims(1);
    const int output_width = output_shape.Dims(2);

    // Set min and max value of the output.
    const int32_t activation_min = data.op_data.output_activation_min;
    const int32_t activation_max = data.op_data.output_activation_max;

    // Consistency check.
    TFLITE_DCHECK_LE(activation_min, activation_max);
    const int batch_size = MatchingDim(input_shape, 0, output_shape, 0);
    const int output_depth = MatchingDim(filter_shape, 3, output_shape, 3);

    TFLITE_DCHECK_EQ(output_depth, input_depth * depth_multiplier);
    if (tflite::micro::GetTensorData<int8_t>(bias)) {
      TFLITE_DCHECK_EQ(bias_shape.FlatSize(), output_depth);
    }

    const int input_size = input_width * input_height * input_depth;
    const int output_size = output_width * output_height * output_depth;
    void *scratch_buf = NULL;
    if (data.buffer_idx > -1) {
      scratch_buf = context->GetScratchBuffer(context, data.buffer_idx);
    }

    if (data.use_esp_nn) {
      esp_nn_set_depthwise_conv_scratch_buf(scratch_buf);
    }

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
    dw_conv_params_t conv_params =  {
                                      .in_offset = input_offset, .out_offset = output_offset,
                                      .ch_mult = depth_multiplier,
                                      .stride = {stride_width, stride_height},
                                      .padding = {pad_width, pad_height}, .dilation = {0, 0},
                                      .activation = {activation_min, activation_max}
                                    };
    quant_data_t quant_data = {
                                .shift = data.op_data.per_channel_output_shift,
                                .mult = data.op_data.per_channel_output_multiplier
    };

    for (int i_batch = 0; i_batch < batch_size; i_batch++) {
#ifdef CONFIG_TFLITEMICRO_ESP_NN_TRACE
      MicroPrintf(
          "[espnn-trace] DepthwiseConv2D out_t=%d esp-nn enter "
          "input=0x%08x filter=0x%08x bias=0x%08x output=0x%08x "
          "scratch=0x%08x mult=0x%08x shift=0x%08x batch=%d",
          data.output_tensor_id,
          static_cast<unsigned int>(reinterpret_cast<uintptr_t>(
              input_data + i_batch * input_size)),
          static_cast<unsigned int>(reinterpret_cast<uintptr_t>(
              tflite::micro::GetTensorData<int8_t>(filter))),
          static_cast<unsigned int>(reinterpret_cast<uintptr_t>(
              tflite::micro::GetOptionalTensorData<int32_t>(bias))),
          static_cast<unsigned int>(reinterpret_cast<uintptr_t>(
              output_data + i_batch * output_size)),
          static_cast<unsigned int>(reinterpret_cast<uintptr_t>(scratch_buf)),
          static_cast<unsigned int>(reinterpret_cast<uintptr_t>(quant_data.mult)),
          static_cast<unsigned int>(reinterpret_cast<uintptr_t>(quant_data.shift)),
          i_batch);
#endif
      esp_nn_depthwise_conv_s8(&input_dims, input_data + i_batch * input_size,
                               &filter_dims, tflite::micro::GetTensorData<int8_t>(filter),
                               tflite::micro::GetOptionalTensorData<int32_t>(bias),
                               &output_dims, output_data + i_batch * output_size,
                               &conv_params, &quant_data);
#ifdef CONFIG_TFLITEMICRO_ESP_NN_TRACE
      MicroPrintf(
          "[espnn-trace] DepthwiseConv2D out_t=%d esp-nn return batch=%d",
          data.output_tensor_id, i_batch);
#endif
    }
  } else {
    reference_integer_ops::DepthwiseConvPerChannel(
        DepthwiseConvParamsQuantized(params, data.op_data),
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
  }
}
#endif

#if ESP_NN
static void EvalReferenceQuantizedPerChannel(
    const TfLiteDepthwiseConvParams& params, const NodeData& data,
    const TfLiteEvalTensor* input, const TfLiteEvalTensor* filter,
    const TfLiteEvalTensor* bias, TfLiteEvalTensor* output,
    int8_t* output_data) {
  reference_integer_ops::DepthwiseConvPerChannel(
      DepthwiseConvParamsQuantized(params, data.op_data),
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
#endif

TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node) {
  TFLITE_DCHECK(node->user_data != nullptr);
  TFLITE_DCHECK(node->builtin_data != nullptr);

  NodeData* data = static_cast<NodeData*>(node->user_data);
  const TfLiteDepthwiseConvParams& params =
      *(static_cast<const TfLiteDepthwiseConvParams*>(node->builtin_data));

  MicroContext* micro_context = GetMicroContext(context);

  TfLiteTensor* output =
      micro_context->AllocateTempOutputTensor(node, kDepthwiseConvOutputTensor);
  TF_LITE_ENSURE(context, output != nullptr);
  TfLiteTensor* input =
      micro_context->AllocateTempInputTensor(node, kDepthwiseConvInputTensor);
  TF_LITE_ENSURE(context, input != nullptr);
  TfLiteTensor* filter =
      micro_context->AllocateTempInputTensor(node, kDepthwiseConvWeightsTensor);
  TF_LITE_ENSURE(context, filter != nullptr);

  // Check dimensionality of input, filter, output
  TF_LITE_ENSURE_EQ(context, input->dims->size, 4);
  TF_LITE_ENSURE_EQ(context, filter->dims->size, 4);
  TF_LITE_ENSURE_EQ(context, output->dims->size, 4);
  TF_LITE_ENSURE(context, params.dilation_height_factor > 0);
  TF_LITE_ENSURE(context, params.dilation_width_factor > 0);

  // Filter in DepthwiseConv is expected to be [1, height, width, channels].
  TF_LITE_ENSURE_EQ(context, filter->dims->data[0], 1);

  // Check input channels matching filter
  const int num_filter_channels = filter->dims->data[3];
  const int num_input_channels = input->dims->data[3];
  TF_LITE_ENSURE(context, num_input_channels != 0);
  TF_LITE_ENSURE_EQ(context, num_filter_channels % num_input_channels, 0);

  const int input_width = input->dims->data[2];
  const int input_height = input->dims->data[1];
  const int filter_width = filter->dims->data[2];
  const int filter_height = filter->dims->data[1];
  const int output_width = output->dims->data[2];
  const int output_height = output->dims->data[1];

  // Dynamically allocate per-channel quantization parameters.
  const int num_channels = filter->dims->data[kDepthwiseConvQuantizedDimension];
  data->op_data.per_channel_output_multiplier =
      static_cast<int32_t*>(context->AllocatePersistentBuffer(
          context, num_channels * sizeof(int32_t)));
  data->op_data.per_channel_output_shift =
      static_cast<int32_t*>(context->AllocatePersistentBuffer(
          context, num_channels * sizeof(int32_t)));

  // All per-channel quantized tensors need valid zero point and scale arrays.
  if (input->type == kTfLiteInt8) {
    TF_LITE_ENSURE_EQ(context, filter->quantization.type,
                      kTfLiteAffineQuantization);

    const auto* affine_quantization =
        static_cast<TfLiteAffineQuantization*>(filter->quantization.params);
    TFLITE_DCHECK(affine_quantization != nullptr);
    TFLITE_DCHECK(affine_quantization->scale != nullptr);
    TFLITE_DCHECK(affine_quantization->zero_point != nullptr);

    TF_LITE_ENSURE(
        context, affine_quantization->scale->size == 1 ||
                     affine_quantization->scale->size ==
                         filter->dims->data[kDepthwiseConvQuantizedDimension]);

    TF_LITE_ENSURE_EQ(context, affine_quantization->scale->size,
                      affine_quantization->zero_point->size);
  }

  TF_LITE_ENSURE_MSG(
      context,
      input->type == filter->type ||
          (input->type == kTfLiteInt8 &&
           (filter->type == kTfLiteInt4 || filter->type == kTfLiteInt8)) ||
          (input->type == kTfLiteInt16 && filter->type == kTfLiteInt8),
      "Hybrid models are not supported on TFLite Micro.");

  if (filter->type == kTfLiteInt4) {
    int filter_size =
        RuntimeShape(filter->dims->size,
                     reinterpret_cast<const int32_t*>(filter->dims->data))
            .FlatSize();
    context->RequestScratchBufferInArena(context, filter_size,
                                         &data->op_data.filter_buffer_index);
  }

  TF_LITE_ENSURE_STATUS(CalculateOpDataDepthwiseConv(
      context, node, params, input_width, input_height, filter_width,
      filter_height, output_width, output_height, input->type, &data->op_data));

#if ESP_NN
  data->buffer_idx = -1;
  data->verify_buffer_idx = -1;
  data->output_tensor_id =
      TensorId(node->outputs, kDepthwiseConvOutputTensor);
  const char* esp_nn_reason = GetEspNnDepthwiseCandidateReason(
      input, filter, output, params, data->op_data);
  data->esp_nn_candidate = esp_nn_reason == nullptr;
  data->use_esp_nn = data->esp_nn_candidate &&
                     IsEspNnDepthwiseOutputSelected(data->output_tensor_id);
  data->runtime_trace_logged = false;

  if (data->use_esp_nn) {
    data_dims_t input_dims = {
        .width = input_width, .height = input_height,
        .channels = num_input_channels, .extra = 1};
    data_dims_t output_dims = {
        .width = output_width, .height = output_height,
        .channels = output->dims->data[3], .extra = 1};
    data_dims_t filter_dims = {
        .width = filter_width, .height = filter_height,
        .channels = filter->dims->data[3], .extra = 0};
    dw_conv_params_t conv_params = {
        .in_offset = 0, .out_offset = 0,
        .ch_mult = params.depth_multiplier,
        .stride = {params.stride_width, params.stride_height},
        .padding = {data->op_data.padding.width, data->op_data.padding.height},
        .dilation = {0, 0}, .activation = {-128, 127}};
    const int scratch_size = esp_nn_get_depthwise_conv_scratch_size(
        &input_dims, &filter_dims, &output_dims, &conv_params);

    /* ESP32-P4 的 DepthwiseConv2D 用内部 QACC，scratch 需求为 0；
     * 只有需求大于 0 时才向 arena 申请，否则保持 buffer_idx = -1。 */
    if (scratch_size > 0) {
      TF_LITE_ENSURE_STATUS(context->RequestScratchBufferInArena(
          context, scratch_size, &data->buffer_idx));
    }
#ifdef CONFIG_TFLITEMICRO_ESP_NN_DEPTHWISE_CONV2D_VERIFY
    TF_LITE_ENSURE_STATUS(context->RequestScratchBufferInArena(
        context, output->bytes, &data->verify_buffer_idx));
#endif
  }

  TraceDepthwisePrepare(node, input, filter, output, params, *data,
                        esp_nn_reason);
#endif

  micro_context->DeallocateTempTfLiteTensor(input);
  micro_context->DeallocateTempTfLiteTensor(filter);
  micro_context->DeallocateTempTfLiteTensor(output);

  return kTfLiteOk;
}

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  TFLITE_DCHECK(node->user_data != nullptr);
  TFLITE_DCHECK(node->builtin_data != nullptr);

  auto& params =
      *(reinterpret_cast<TfLiteDepthwiseConvParams*>(node->builtin_data));
  auto& data = *(static_cast<NodeData*>(node->user_data));

  TfLiteEvalTensor* output =
      tflite::micro::GetEvalOutput(context, node, kDepthwiseConvOutputTensor);
  const TfLiteEvalTensor* input =
      tflite::micro::GetEvalInput(context, node, kDepthwiseConvInputTensor);
  const TfLiteEvalTensor* filter =
      tflite::micro::GetEvalInput(context, node, kDepthwiseConvWeightsTensor);
  const TfLiteEvalTensor* bias =
      (NumInputs(node) == 3)
          ? tflite::micro::GetEvalInput(context, node, kDepthwiseConvBiasTensor)
          : nullptr;

#if ESP_NN
  void* scratch = data.buffer_idx >= 0 ?
                      context->GetScratchBuffer(context, data.buffer_idx) :
                      nullptr;
  const char* runtime_reason =
      data.use_esp_nn
          ? GetEspNnDepthwiseRuntimeReason(
                tflite::micro::GetTensorData<int8_t>(input),
                tflite::micro::GetTensorData<int8_t>(output), scratch)
          : "reference";
#ifdef CONFIG_TFLITEMICRO_ESP_NN_TRACE
  if (!data.runtime_trace_logged) {
    MicroPrintf(
        "[espnn-trace] DepthwiseConv2D out_t=%d input_mod16=%d "
        "filter_mod8=%d output_mod16=%d scratch_mod16=%d backend=%s reason=%s",
        data.output_tensor_id,
        static_cast<int>(reinterpret_cast<uintptr_t>(
                             tflite::micro::GetTensorData<int8_t>(input)) & 15),
        static_cast<int>(reinterpret_cast<uintptr_t>(
                             tflite::micro::GetTensorData<int8_t>(filter)) & 7),
        static_cast<int>(reinterpret_cast<uintptr_t>(
                             tflite::micro::GetTensorData<int8_t>(output)) & 15),
        static_cast<int>(reinterpret_cast<uintptr_t>(scratch) & 15),
        runtime_reason == nullptr ? "esp-nn" : "reference",
        runtime_reason == nullptr ? "ok" : runtime_reason);
    data.runtime_trace_logged = true;
  }
#endif
#endif

  switch (input->type) {  // Already know in/out types are same.
    case kTfLiteFloat32: {
      tflite::reference_ops::DepthwiseConv(
          DepthwiseConvParamsFloat(params, data.op_data),
          tflite::micro::GetTensorShape(input),
          tflite::micro::GetTensorData<float>(input),
          tflite::micro::GetTensorShape(filter),
          tflite::micro::GetTensorData<float>(filter),
          tflite::micro::GetTensorShape(bias),
          tflite::micro::GetOptionalTensorData<float>(bias),
          tflite::micro::GetTensorShape(output),
          tflite::micro::GetTensorData<float>(output));
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
          reference_integer_ops::DepthwiseConvPerChannel(
              DepthwiseConvParamsQuantized(params, data.op_data),
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
#ifdef CONFIG_TFLITEMICRO_ESP_NN_DEPTHWISE_CONV2D_VERIFY
            int8_t* reference_output = static_cast<int8_t*>(
                context->GetScratchBuffer(context, data.verify_buffer_idx));
            EvalReferenceQuantizedPerChannel(params, data, input, filter, bias,
                                             output, reference_output);
            EvalQuantizedPerChannel(context, node, params, data, input, filter,
                                    bias, output);

            const size_t output_bytes =
                tflite::micro::GetTensorShape(output).FlatSize() *
                sizeof(int8_t);
            int mismatch = -1;
            int8_t* esp_nn_output =
                tflite::micro::GetTensorData<int8_t>(output);
            for (size_t i = 0; i < output_bytes; i++) {
              if (reference_output[i] != esp_nn_output[i]) {
                mismatch = static_cast<int>(i);
                break;
              }
            }

            if (mismatch >= 0) {
              MicroPrintf(
                  "[espnn-verify] DepthwiseConv2D out_t=%d mismatch=%d "
                  "ref=%d esp=%d; restoring reference output",
                  data.output_tensor_id, mismatch, reference_output[mismatch],
                  esp_nn_output[mismatch]);
              std::memcpy(esp_nn_output, reference_output, output_bytes);
            } else {
              MicroPrintf(
                  "[espnn-verify] DepthwiseConv2D out_t=%d match bytes=%d",
                  data.output_tensor_id, static_cast<int>(output_bytes));
            }
#else
            EvalQuantizedPerChannel(context, node, params, data, input, filter,
                                    bias, output);
#endif
          } else {
            EvalReferenceQuantizedPerChannel(
                params, data, input, filter, bias, output,
                tflite::micro::GetTensorData<int8_t>(output));
          }
#else
          reference_integer_ops::DepthwiseConvPerChannel(
              DepthwiseConvParamsQuantized(params, data.op_data),
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
          MicroPrintf("Filter type %s (%d) for input type %s not supported.",
                      TfLiteTypeGetName(filter->type), filter->type,
                      TfLiteTypeGetName(input->type));
          return kTfLiteError;
      }
      break;
    }
    case kTfLiteInt16: {
      switch (filter->type) {
        case kTfLiteInt8: {
          reference_integer_ops::DepthwiseConvPerChannel(
              DepthwiseConvParamsQuantized(params, data.op_data),
              data.op_data.per_channel_output_multiplier,
              data.op_data.per_channel_output_shift,
              tflite::micro::GetTensorShape(input),
              tflite::micro::GetTensorData<int16_t>(input),
              tflite::micro::GetTensorShape(filter),
              tflite::micro::GetTensorData<int8_t>(filter),
              tflite::micro::GetTensorShape(bias),
              tflite::micro::GetOptionalTensorData<int64_t>(bias),
              tflite::micro::GetTensorShape(output),
              tflite::micro::GetTensorData<int16_t>(output));
          break;
        }
        default:
          MicroPrintf("Filter type %s (%d) for input type %s not supported.",
                      TfLiteTypeGetName(filter->type), filter->type,
                      TfLiteTypeGetName(input->type));
          return kTfLiteError;
      }
      break;
    }
    default:
      MicroPrintf("Input type %s (%d) not supported.",
                  TfLiteTypeGetName(input->type), input->type);
      return kTfLiteError;
  }
  return kTfLiteOk;
}

}  // namespace

TFLMRegistration Register_DEPTHWISE_CONV_2D() {
  return tflite::micro::RegisterOp(Init, Prepare,
                                   Eval);
}

}  // namespace tflite
