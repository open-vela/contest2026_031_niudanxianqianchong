# TFLM ESP-NN adapter

This directory is the version-controlled source of the OpenVela-specific
TFLite Micro adapter layer.  It is not an ESP-NN upstream fork.

`kernels/` contains the four adapter sources:

- `conv.cc`: controlled TFLM Conv2D to ESP-NN dispatch and reference fallback;
- `depthwise_conv.cc`: controlled DepthwiseConv2D dispatch and fallback;
- `mean.cc`: audio-model NHWC global-average Mean adapter;
- `micro_time_esp32s3.cc`: ESP32-S3 CCOUNT profiler time source.

During this transition the original files in
`apps/mlearning/tflite-micro/kernels/esp_nn/` remain unchanged.  Run
`ccf_audioevent/scripts/link_tflm_espnn_adapter.sh --check` to compare them.
The script only creates a link when the original path is absent; it never
replaces an existing path.

The matching TFLite Micro Kconfig, Make, and CMake changes are maintained as
`ccf_audioevent/patches/tflite-micro/0001-add-esp-nn-backend.patch`.
