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

#include <arch/chip/esp_ldo.h>
#include <arch/chip/esp_isp.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP_MIPI_CSI_BUS0                 0
#define ESP_MIPI_CSI_MAX_DATA_LANES        2
#define ESP_MIPI_CSI_DPHY_VOLTAGE_MV    2500

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
  enum esp_isp_output_e output;
  uint8_t  bayer_order;
  struct esp_ldo_config_s phy_ldo;
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
  uint32_t last_bridge_raw_status;
  uint32_t last_bridge_enable_status;
  uint32_t last_bridge_buffer_status;
  uint32_t last_host_phy_fatal_status;
  uint32_t last_host_packet_fatal_status;
  uint32_t last_host_phy_status;
  uint32_t last_phy_rx_status;
  uint32_t last_phy_stopstate_status;
  uint32_t last_dma_channel_status;
  uint32_t last_dma_transfer_units;
  uint32_t last_dma_fifo_units;
  uint32_t last_dma_source_status;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: esp_mipi_csi_power_acquire
 *
 * Description:
 *   Acquire the ESP32-P4 CSI D-PHY LDO described by config->phy_ldo.  This
 *   is separate from CSI Host initialization so a board can follow the
 *   Espressif camera bring-up order: power the MIPI PHY, configure and start
 *   the sensor, then initialize the CSI receiver.  Only one CSI receiver
 *   exists on ESP32-P4.
 *
 ****************************************************************************/

int esp_mipi_csi_power_acquire(
  FAR const struct esp_mipi_csi_config_s *config,
  FAR struct esp_mipi_csi_s **csi);

/****************************************************************************
 * Name: esp_mipi_csi_initialize
 *
 * Description:
 *   Configure CSI PHY, Host, ISP bypass and Bridge for a raw CSI-2 stream
 *   without explicit line-start/line-end packets.  Each line must be aligned
 *   to 64 bits.  ISP clocks remain enabled until deinitialization.
 *   The caller must acquire power with esp_mipi_csi_power_acquire() first.
 *
 ****************************************************************************/

int esp_mipi_csi_initialize(FAR struct esp_mipi_csi_s *csi,
                            FAR const struct esp_mipi_csi_config_s *config);

/****************************************************************************
 * Name: esp_mipi_csi_deinitialize
 *
 * Description:
 *   Stop a running capture if necessary and release CSI Host, Bridge and DMA
 *   resources.  The D-PHY LDO remains acquired until
 *   esp_mipi_csi_power_release() is called.
 *
 ****************************************************************************/

int esp_mipi_csi_deinitialize(FAR struct esp_mipi_csi_s *csi);

/****************************************************************************
 * Name: esp_mipi_csi_power_release
 *
 * Description:
 *   Release a D-PHY LDO acquired by esp_mipi_csi_power_acquire().  CSI Host
 *   resources must have been deinitialized first.
 *
 ****************************************************************************/

int esp_mipi_csi_power_release(FAR struct esp_mipi_csi_s *csi);

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
 * Name: esp_mipi_csi_wait_frame_diag
 *
 * Description:
 *   Wait for a frame while logging PHY, ISP and DMA observations.  For the
 *   standalone bring-up command only: observe sticky frame events using the
 *   ISP clock already owned by initialization.  Do not change clocks or the
 *   data-path configuration.  Return -EPIPE if reception or the ISP clock
 *   is not active.  Samples are spaced by one scheduler tick; absence of
 *   sampled HS activity does not prove absence of a stream.
 *
 ****************************************************************************/

int esp_mipi_csi_wait_frame_diag(FAR struct esp_mipi_csi_s *csi,
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
 *   writes.  The buffer must be the one passed to esp_mipi_csi_start(), with
 *   both its address and its size aligned to the 64-byte P4 cache line.
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
