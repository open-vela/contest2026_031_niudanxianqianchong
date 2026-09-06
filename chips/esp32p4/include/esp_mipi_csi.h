/****************************************************************************
 * chips/esp32p4/include/esp_mipi_csi.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_MIPI_CSI_H
#define __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_MIPI_CSI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP_MIPI_CSI_BUS0                 0
#define ESP_MIPI_CSI_MAX_DATA_LANES        2

/* CSI-2 data types used by the raw capture path. */

#define ESP_MIPI_CSI_DT_RAW8               0x2a
#define ESP_MIPI_CSI_DT_RAW10              0x2b
#define ESP_MIPI_CSI_DT_RAW12              0x2c

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct esp_mipi_csi_s;

/* This is deliberately a receiver-only primitive.  A board owns sensor
 * reset, MCLK, SCCB and the sensor's stream profile; this layer only accepts
 * the profile's resulting CSI-2 transport parameters.
 */

struct esp_mipi_csi_config_s
{
  uint8_t  lane_num;
  uint8_t  data_type;
  uint8_t  bits_per_pixel;
  uint16_t width;
  uint16_t height;
  uint32_t lane_bit_rate_mbps;
  bool     byte_swap;
};

struct esp_mipi_csi_stats_s
{
  uint32_t frame_count;
  uint32_t dma_error_count;
  uint32_t bridge_overrun_count;
  uint32_t bridge_fifo_overflow_count;
  uint32_t bridge_discard_count;
  uint32_t bridge_frame_size_error_count;
  uint32_t csi_ecc_error_count;
  uint32_t csi_crc_error_count;
  uint32_t csi_phy_error_count;
  uint32_t csi_packet_error_count;
  uint32_t last_dma_status;
  uint32_t last_bridge_status;
  uint32_t last_host_status;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: esp_mipi_csi_initialize
 *
 * Description:
 *   Configure the ESP32-P4 CSI PHY, Host and Bridge for a raw CSI-2 stream.
 *   The caller must have configured, but not necessarily started, the image
 *   sensor.  Only one CSI receiver exists on ESP32-P4.
 *
 ****************************************************************************/

int esp_mipi_csi_initialize(FAR const struct esp_mipi_csi_config_s *config,
                            FAR struct esp_mipi_csi_s **csi);

/****************************************************************************
 * Name: esp_mipi_csi_shutdown
 *
 * Description:
 *   Stop a running capture if necessary and release the CSI hardware.
 *
 ****************************************************************************/

int esp_mipi_csi_shutdown(FAR struct esp_mipi_csi_s *csi);

/****************************************************************************
 * Name: esp_mipi_csi_start
 *
 * Description:
 *   Start continuous reception into a caller-owned DMA-capable buffer.  The
 *   same buffer is re-armed at each completed frame; callers that need a
 *   queue of frames will be added by the video upper-half in P3.
 *
 ****************************************************************************/

int esp_mipi_csi_start(FAR struct esp_mipi_csi_s *csi,
                       FAR void *frame_buffer, size_t frame_buffer_bytes);

/****************************************************************************
 * Name: esp_mipi_csi_wait_frame
 *
 * Description:
 *   Wait for one completed DMA frame.  timeout_ms must be non-zero.
 *
 ****************************************************************************/

int esp_mipi_csi_wait_frame(FAR struct esp_mipi_csi_s *csi,
                            uint32_t timeout_ms);

/****************************************************************************
 * Name: esp_mipi_csi_stop
 *
 * Description:
 *   Stop continuous reception.  The caller may then inspect or free its
 *   frame buffer.
 *
 ****************************************************************************/

int esp_mipi_csi_stop(FAR struct esp_mipi_csi_s *csi);

/****************************************************************************
 * Name: esp_mipi_csi_buffer_sync_for_cpu
 *
 * Description:
 *   Invalidate a completed frame before CPU CRC, non-zero checks or file
 *   writes.  The buffer must be the one passed to esp_mipi_csi_start().
 *
 ****************************************************************************/

int esp_mipi_csi_buffer_sync_for_cpu(FAR void *buffer, size_t bytes);

/****************************************************************************
 * Name: esp_mipi_csi_get_stats
 *
 * Description:
 *   Return an atomic snapshot of CSI, Bridge and DMA diagnostics.
 *
 ****************************************************************************/

int esp_mipi_csi_get_stats(FAR struct esp_mipi_csi_s *csi,
                           FAR struct esp_mipi_csi_stats_s *stats);

#endif /* __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_MIPI_CSI_H */
