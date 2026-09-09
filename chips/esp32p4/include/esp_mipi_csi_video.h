#ifndef __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_MIPI_CSI_VIDEO_H
#define __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_MIPI_CSI_VIDEO_H

#include <nuttx/video/imgdata.h>
#include <arch/chip/esp_mipi_csi.h>

#include <stdbool.h>

struct esp_mipi_csi_video_s
{
  struct imgdata_s data;
  FAR struct esp_mipi_csi_s *csi;
  FAR uint8_t *buffer;
  uint32_t bytes;
  imgdata_capture_t callback;
  FAR void *callback_arg;
  bool streaming;
};

int esp_mipi_csi_video_initialize(FAR struct esp_mipi_csi_video_s *video,
                                  FAR struct esp_mipi_csi_s *csi);
void esp_mipi_csi_video_set_csi(FAR struct esp_mipi_csi_video_s *video,
                                FAR struct esp_mipi_csi_s *csi);
#endif
