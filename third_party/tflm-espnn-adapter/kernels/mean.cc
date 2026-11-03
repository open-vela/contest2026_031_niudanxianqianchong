/*
 * ESP-NN specialized Mean dispatch for the openvela TFLite Micro port.
 *
 * This deliberately admits only the audio-event global average pooling forms
 * assigned to hardware verify profiles: int8 [1, H, W, 24/96] reduced over
 * axes {1, 2}. All other
 * Mean nodes retain the TFLite Micro reference implementation.
 */

#include "tensorflow/lite/micro/kernels/reduce.h"

#include <cstdint>
#include <cstring>
#include <limits>

#include "tensorflow/lite/kernels/internal/quantization_util.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/micro/kernels/kernel_util.h"
#include "tensorflow/lite/micro/micro_log.h"

#if ESP_NN
#include <esp_nn.h>
#endif

namespace tflite {
namespace esp_nn {
namespace {

constexpr int kMeanInputTensor = 0;
constexpr int kMeanAxisTensor = 1;
constexpr int kMeanOutputTensor = 0;

bool HasSupportedChannels(int channels) {
  return channels == 24 || channels == 96;
}

struct NodeData {
  OpDataReduce reference_op_data;
  int verify_buffer_idx;
  int output_tensor_id;
  int height;
  int width;
  int channels;
  int32_t mean_multiplier;
  int mean_shift;
  bool esp_nn_candidate;
  bool use_esp_nn;
  bool runtime_trace_logged;
};

void* Init(TfLiteContext* context, const char* buffer, size_t length) {
  TFLITE_DCHECK(context->AllocatePersistentBuffer != nullptr);
  return context->AllocatePersistentBuffer(context, sizeof(NodeData));
}

int TensorId(const TfLiteIntArray* tensors, int position) {
  return tensors != nullptr && position < tensors->size ?
             tensors->data[position] :
             -1;
}

bool HasSpatialMeanAxes(const TfLiteTensor* axis) {
  if (axis == nullptr || axis->type != kTfLiteInt32 || NumElements(axis) != 2 ||
      axis->data.i32 == nullptr) {
    return false;
  }

  const int first = axis->data.i32[0];
  const int second = axis->data.i32[1];
  return (first == 1 && second == 2) || (first == 2 && second == 1);
}

bool HasExpectedOutputShape(const TfLiteTensor* output, bool keep_dims,
                            int channels) {
  if (output == nullptr || NumElements(output) != channels) {
    return false;
  }

  if (keep_dims) {
    return output->dims->size == 4 && output->dims->data[0] == 1 &&
           output->dims->data[1] == 1 && output->dims->data[2] == 1 &&
           output->dims->data[3] == channels;
  }

  return output->dims->size == 2 && output->dims->data[0] == 1 &&
         output->dims->data[1] == channels;
}

bool CalculateMeanMultiplier(const OpDataReduce& reference_op_data,
                             int64_t spatial_elements,
                             int32_t* mean_multiplier, int* mean_shift) {
  if (spatial_elements <= 0 ||
      spatial_elements > std::numeric_limits<int32_t>::max()) {
    return false;
  }

  /*
   * Match TFLM reference_ops::QuantizedMeanOrSum exactly. The resulting
   * multiplier represents input_scale / output_scale / spatial_elements;
   * here that denominator is 25 * 20 = 500 for the current model.
   */
  int reduction_shift =
      63 - __builtin_clzll(static_cast<unsigned long long>(spatial_elements));
  reduction_shift = reduction_shift > 32 ? 32 : reduction_shift;
  const int max_safe_shift = 31 + reference_op_data.shift;
  reduction_shift = reduction_shift > max_safe_shift ? max_safe_shift :
                                                      reduction_shift;
  if (reduction_shift < 0) {
    return false;
  }

  *mean_multiplier = static_cast<int32_t>(
      (static_cast<int64_t>(reference_op_data.multiplier) << reduction_shift) /
      spatial_elements);
  *mean_shift = reference_op_data.shift - reduction_shift;
  return true;
}

const char* GetCandidateReason(const TfLiteTensor* input,
                               const TfLiteTensor* axis,
                               const TfLiteTensor* output,
                               const TfLiteReducerParams& params,
                               const NodeData& data) {
  if (input->type != kTfLiteInt8 || output->type != kTfLiteInt8) {
    return "type";
  }

  if (input->dims->size != 4 || input->dims->data[0] != 1 ||
      input->dims->data[1] <= 0 || input->dims->data[2] <= 0 ||
      !HasSupportedChannels(input->dims->data[3])) {
    return "input_shape";
  }

  if (!HasSpatialMeanAxes(axis)) {
    return "axis";
  }

  if (!HasExpectedOutputShape(output, params.keep_dims,
                              input->dims->data[3])) {
    return "output_shape";
  }

  if (input->params.scale <= 0.0f || output->params.scale <= 0.0f ||
      data.mean_multiplier <= 0) {
    return "quantization";
  }

  return nullptr;
}

void TracePrepare(const TfLiteNode* node, const TfLiteTensor* input,
                  const NodeData& data, const char* reason) {
#ifdef CONFIG_TFLITEMICRO_ESP_NN_TRACE
  const int input_rank = input != nullptr && input->dims != nullptr ?
                             input->dims->size :
                             -1;
  const int batch = input_rank == 4 ? input->dims->data[0] : -1;
  const int height = input_rank == 4 ? input->dims->data[1] : -1;
  const int width = input_rank == 4 ? input->dims->data[2] : -1;
  const int channels = input_rank == 4 ? input->dims->data[3] : -1;
  MicroPrintf(
      "[espnn-trace] Mean in_t=%d out_t=%d in=[%d,%d,%d,%d] "
      "candidate=%d selected=%d reason=%s",
      TensorId(node->inputs, kMeanInputTensor),
      TensorId(node->outputs, kMeanOutputTensor), batch, height, width,
      channels,
      data.esp_nn_candidate ? 1 : 0, data.use_esp_nn ? 1 : 0,
      reason == nullptr ? "ok" : reason);
#endif
}

TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node) {
  NodeData* data = static_cast<NodeData*>(node->user_data);
  TF_LITE_ENSURE(context, data != nullptr);
  TF_LITE_ENSURE_OK(
      context, PrepareMeanOrSumHelper(context, node, &data->reference_op_data));

  MicroContext* micro_context = GetMicroContext(context);
  TfLiteTensor* input =
      micro_context->AllocateTempInputTensor(node, kMeanInputTensor);
  TfLiteTensor* axis =
      micro_context->AllocateTempInputTensor(node, kMeanAxisTensor);
  TfLiteTensor* output =
      micro_context->AllocateTempOutputTensor(node, kMeanOutputTensor);
  TF_LITE_ENSURE(context, input != nullptr && axis != nullptr && output != nullptr);

  data->verify_buffer_idx = -1;
  data->output_tensor_id = TensorId(node->outputs, kMeanOutputTensor);
  data->height = input->dims->size == 4 ? input->dims->data[1] : 0;
  data->width = input->dims->size == 4 ? input->dims->data[2] : 0;
  data->channels = input->dims->size == 4 ? input->dims->data[3] : 0;
  data->mean_multiplier = 0;
  data->mean_shift = 0;
  data->runtime_trace_logged = false;

  const int64_t spatial_elements =
      static_cast<int64_t>(data->height) * data->width;
  const bool has_mean_multiplier = CalculateMeanMultiplier(
      data->reference_op_data, spatial_elements, &data->mean_multiplier,
      &data->mean_shift);
  TfLiteReducerParams* params =
      static_cast<TfLiteReducerParams*>(node->builtin_data);
  const char* reason =
      params != nullptr && has_mean_multiplier
          ? GetCandidateReason(input, axis, output, *params, *data)
          : "quantization";
  data->esp_nn_candidate = reason == nullptr;
#ifdef CONFIG_TFLITEMICRO_ESP_NN_MEAN
  data->use_esp_nn = data->esp_nn_candidate;
#else
  data->use_esp_nn = false;
  if (reason == nullptr) {
    reason = "mean_disabled";
  }
#endif

#ifdef CONFIG_TFLITEMICRO_ESP_NN_MEAN_VERIFY
  if (data->use_esp_nn) {
    TF_LITE_ENSURE_STATUS(context->RequestScratchBufferInArena(
        context, NumElements(output) * sizeof(int8_t),
        &data->verify_buffer_idx));
  }
#endif

  TracePrepare(node, input, *data, reason);
  micro_context->DeallocateTempTfLiteTensor(input);
  micro_context->DeallocateTempTfLiteTensor(axis);
  micro_context->DeallocateTempTfLiteTensor(output);
  return kTfLiteOk;
}

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  NodeData* data = static_cast<NodeData*>(node->user_data);
  TF_LITE_ENSURE(context, data != nullptr);

  const TfLiteEvalTensor* input =
      tflite::micro::GetEvalInput(context, node, kMeanInputTensor);
  TfLiteEvalTensor* output =
      tflite::micro::GetEvalOutput(context, node, kMeanOutputTensor);
  TF_LITE_ENSURE(context, input != nullptr && output != nullptr);

  if (!data->use_esp_nn) {
    return EvalMeanHelperInt8(context, node, &data->reference_op_data);
  }

#ifdef CONFIG_TFLITEMICRO_ESP_NN_TRACE
  if (!data->runtime_trace_logged) {
    MicroPrintf("[espnn-trace] Mean out_t=%d backend=esp-nn",
                data->output_tensor_id);
    data->runtime_trace_logged = true;
  }
#endif

#ifdef CONFIG_TFLITEMICRO_ESP_NN_MEAN_VERIFY
  int8_t* reference_output = static_cast<int8_t*>(
      context->GetScratchBuffer(context, data->verify_buffer_idx));
  TF_LITE_ENSURE(context, reference_output != nullptr);
  TF_LITE_ENSURE_OK(context,
                    EvalMeanHelperInt8(context, node, &data->reference_op_data));
  const size_t output_bytes =
      tflite::micro::GetTensorShape(output).FlatSize() * sizeof(int8_t);
  std::memcpy(reference_output, tflite::micro::GetTensorData<int8_t>(output),
              output_bytes);
#endif

  esp_nn_mean_nhwc_s8(tflite::micro::GetTensorData<int8_t>(input),
                      tflite::micro::GetTensorData<int8_t>(output),
                      data->height, data->width, data->channels,
                      data->reference_op_data.input_zp,
                      data->reference_op_data.output_zp,
                      data->mean_multiplier, data->mean_shift);

#ifdef CONFIG_TFLITEMICRO_ESP_NN_MEAN_VERIFY
  int mismatch = -1;
  int8_t* esp_nn_output = tflite::micro::GetTensorData<int8_t>(output);
  for (size_t i = 0; i < output_bytes; i++) {
    if (reference_output[i] != esp_nn_output[i]) {
      mismatch = static_cast<int>(i);
      break;
    }
  }

  if (mismatch >= 0) {
    MicroPrintf("[espnn-verify] Mean out_t=%d mismatch=%d ref=%d esp=%d; "
                "restoring reference output",
                data->output_tensor_id, mismatch, reference_output[mismatch],
                esp_nn_output[mismatch]);
    std::memcpy(esp_nn_output, reference_output, output_bytes);
  } else {
    MicroPrintf("[espnn-verify] Mean out_t=%d match bytes=%d",
                data->output_tensor_id, static_cast<int>(output_bytes));
  }
#endif

  return kTfLiteOk;
}

}  // namespace

TFLMRegistration Register_MEAN() {
  return tflite::micro::RegisterOp(Init, Prepare, Eval);
}

}  // namespace esp_nn
}  // namespace tflite
