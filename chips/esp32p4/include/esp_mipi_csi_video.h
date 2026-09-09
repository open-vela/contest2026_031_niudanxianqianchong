/****************************************************************************
 * chips/esp32p4/include/esp_mipi_csi_video.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_MIPI_CSI_VIDEO_H
#define __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_MIPI_CSI_VIDEO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/spinlock.h>
#include <nuttx/video/imgdata.h>

#include <stdbool.h>

#include <arch/chip/esp_mipi_csi.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP_MIPI_CSI_VIDEO_DMA_BUFFERS 3

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct esp_mipi_csi_video_s
{
  struct imgdata_s data;
  FAR struct esp_mipi_csi_s *csi;
  FAR uint8_t *v4l2_buffer;
  FAR uint8_t *dma_buffers[ESP_MIPI_CSI_VIDEO_DMA_BUFFERS];
  imgdata_capture_t callback;
  FAR void *callback_arg;
  uint32_t bytes;
  spinlock_t lock;
  bool streaming;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int esp_mipi_csi_video_initialize(FAR struct esp_mipi_csi_video_s *video,
                                  FAR struct esp_mipi_csi_s *csi);
void esp_mipi_csi_video_set_csi(FAR struct esp_mipi_csi_video_s *video,
                                FAR struct esp_mipi_csi_s *csi);

#endif /* __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_MIPI_CSI_VIDEO_H */
