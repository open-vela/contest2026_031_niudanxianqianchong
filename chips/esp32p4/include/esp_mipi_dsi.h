/****************************************************************************
 * chips/esp32p4/include/esp_mipi_dsi.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_MIPI_DSI_H
#define __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_MIPI_DSI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/video/mipi_dsi.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <arch/chip/esp_ldo.h>

#include <arch/chip/esp_mipi_dsi_dpi_panel.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP_MIPI_DSI_BUS0                0
#define ESP_MIPI_DSI_MAX_DATA_LANES       2
#define ESP_MIPI_DSI_DPHY_VOLTAGE_MV      2500

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct esp_mipi_dsi_host_config_s
{
  uint8_t                 bus;
  uint8_t                 lane_num;
  uint32_t                lane_bit_rate_mbps;
  uint32_t                phy_ref_clock_hz;
  uint32_t                timeout_ms;
  struct esp_ldo_config_s phy_ldo;
};

/* The P4 DSI Host can generate these patterns internally.  This M2a API is
 * deliberately framebuffer-free: it verifies the entire DPI/video path
 * before a board enables DMA-backed framebuffers or LVGL.
 */

enum esp_mipi_dsi_video_pattern_e
{
  ESP_MIPI_DSI_VIDEO_PATTERN_NONE = 0,
  ESP_MIPI_DSI_VIDEO_PATTERN_VERTICAL_BARS,
  ESP_MIPI_DSI_VIDEO_PATTERN_HORIZONTAL_BARS,
  ESP_MIPI_DSI_VIDEO_PATTERN_BER_VERTICAL,
};

struct esp_mipi_dsi_video_pattern_config_s
{
  uint8_t  channel;
  uint16_t hactive;
  uint16_t hsync;
  uint16_t hback_porch;
  uint16_t hfront_porch;
  uint16_t vactive;
  uint16_t vsync;
  uint16_t vback_porch;
  uint16_t vfront_porch;
  uint32_t pixel_clock_hz;
  bool     hsync_active_low;
  bool     vsync_active_low;
  enum esp_mipi_dsi_video_pattern_e pattern;
};

/* DMA scanout accepts a caller-owned frame buffer.  The generic chip adapter
 * configures only the P4 DSI Bridge/GDMA path; allocation, pixel contents
 * and lifetime remain outside this low-level primitive.
 */

struct esp_mipi_dsi_video_dma_config_s
{
  uint8_t        channel;
  uint16_t       hactive;
  uint16_t       hsync;
  uint16_t       hback_porch;
  uint16_t       hfront_porch;
  uint16_t       vactive;
  uint16_t       vsync;
  uint16_t       vback_porch;
  uint16_t       vfront_porch;
  uint32_t       pixel_clock_hz;
  bool           hsync_active_low;
  bool           vsync_active_low;
  enum esp_mipi_dsi_dpi_color_format_e input_format;
  enum esp_mipi_dsi_dpi_color_format_e output_format;
  FAR const void *frame_buffer;
  size_t         frame_buffer_bytes;
};

/* A frame-done callback runs in the DW-GDMA interrupt context after a full
 * frame has completed and the next descriptor has been armed.  It must not
 * block, allocate memory or issue DSI commands.
 */

typedef void (*esp_mipi_dsi_video_dma_frame_done_t)(FAR void *arg);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int esp_mipi_dsi_host_initialize(
  FAR const struct esp_mipi_dsi_host_config_s *config,
  FAR struct mipi_dsi_host **host);
int esp_mipi_dsi_host_shutdown(FAR struct mipi_dsi_host *host);

/****************************************************************************
 * Name: esp_mipi_dsi_video_pattern_start
 *
 * Description:
 *   Configure DPI video mode and start the P4 Host's built-in test-pattern
 *   generator.  No framebuffer, DMA descriptor, or LVGL object is involved.
 *
 ****************************************************************************/

int esp_mipi_dsi_video_pattern_start(
  FAR struct mipi_dsi_host *host,
  FAR const struct esp_mipi_dsi_video_pattern_config_s *config);

/****************************************************************************
 * Name: esp_mipi_dsi_video_pattern_set
 *
 * Description:
 *   Select or disable the P4 Host's built-in pattern while an existing DPI
 *   video pipeline is running.  This preserves the active panel timing and
 *   colour format, allowing framebuffer/GDMA pixel contents to be bypassed
 *   without rebuilding the video pipeline.
 *
 ****************************************************************************/

int esp_mipi_dsi_video_pattern_set(
  FAR struct mipi_dsi_host *host,
  enum esp_mipi_dsi_video_pattern_e pattern);

/****************************************************************************
 * Name: esp_mipi_dsi_video_dma_start
 *
 * Description:
 *   Configure DPI video mode and continuously feed a caller-owned RGB565 or
 *   RGB888 frame buffer to the P4 DSI Bridge through DW-GDMA.  The caller
 *   must keep the buffer valid and unchanged until
 *   esp_mipi_dsi_video_stop() returns.
 *
 ****************************************************************************/

int esp_mipi_dsi_video_dma_start(
  FAR struct mipi_dsi_host *host,
  FAR const struct esp_mipi_dsi_video_dma_config_s *config);

/****************************************************************************
 * Name: esp_mipi_dsi_video_dma_queue_frame_buffer
 *
 * Description:
 *   Queue a same-sized, DMA-aligned framebuffer as the source for the next
 *   completed DW-GDMA frame.  The current frame remains untouched.  A newer
 *   request replaces an older request that has not reached a frame boundary.
 *   The caller must clean CPU-written pixels before queuing the buffer and
 *   retain it until a later frame-done callback.
 *
 ****************************************************************************/

int esp_mipi_dsi_video_dma_queue_frame_buffer(
  FAR struct mipi_dsi_host *host, FAR const void *frame_buffer,
  size_t frame_buffer_bytes);

/****************************************************************************
 * Name: esp_mipi_dsi_video_dma_set_frame_done_callback
 *
 * Description:
 *   Register or clear the non-blocking frame-done callback for a running DPI
 *   DMA scanout.  This permits a board framebuffer driver to release one
 *   queued FBIOPAN_DISPLAY request and notify VSync at each frame boundary.
 *
 ****************************************************************************/

int esp_mipi_dsi_video_dma_set_frame_done_callback(
  FAR struct mipi_dsi_host *host,
  esp_mipi_dsi_video_dma_frame_done_t callback, FAR void *arg);

/****************************************************************************
 * Name: esp_mipi_dsi_video_dma_dump_status
 *
 * Description:
 *   Print a read-only DW-GDMA/DSI Bridge scanout snapshot.  The result
 *   includes transfer progress, DMA completion and error bits, plus the
 *   Bridge underrun state.  It is intended for board bring-up only.
 *
 ****************************************************************************/

int esp_mipi_dsi_video_dma_dump_status(FAR struct mipi_dsi_host *host,
                                       FAR const char *stage);

/****************************************************************************
 * Name: esp_mipi_dsi_video_phy_sample_status
 *
 * Description:
 *   Sample the read-only D-PHY lane status repeatedly while DPI video is
 *   running.  The function reports how often the clock lane and both data
 *   lanes leave LP11 stop state, plus lock loss and stop-state transitions.
 *   Leaving stop state is evidence of lane activity, but is not by itself a
 *   decoded proof of HS video packets.  This diagnostic does not change the
 *   lane mode.
 *
 ****************************************************************************/

int esp_mipi_dsi_video_phy_sample_status(
  FAR struct mipi_dsi_host *host, FAR const char *stage,
  uint32_t sample_count, uint32_t interval_us);

/****************************************************************************
 * Name: esp_mipi_dsi_dma_buffer_allocate
 *
 * Description:
 *   Allocate a P4 DMA-capable PSRAM buffer with the alignment required by
 *   the DSI Bridge/GDMA scanout path.  This is a chip-layer ownership
 *   boundary: board code supplies the pixels, but does not include ESP HAL
 *   heap or cache headers.  The caller must release the buffer only after
 *   esp_mipi_dsi_video_stop() returns.
 *
 ****************************************************************************/

int esp_mipi_dsi_dma_buffer_allocate(size_t bytes, FAR void **buffer);

/****************************************************************************
 * Name: esp_mipi_dsi_dma_buffer_sync_for_device
 *
 * Description:
 *   Clean CPU-written data to memory before the DSI Bridge/GDMA reads a
 *   buffer returned by esp_mipi_dsi_dma_buffer_allocate().
 *
 ****************************************************************************/

int esp_mipi_dsi_dma_buffer_sync_for_device(FAR void *buffer, size_t bytes);

/****************************************************************************
 * Name: esp_mipi_dsi_dma_buffer_free
 *
 * Description:
 *   Release a buffer returned by esp_mipi_dsi_dma_buffer_allocate().
 *
 ****************************************************************************/

void esp_mipi_dsi_dma_buffer_free(FAR void *buffer);

/****************************************************************************
 * Name: esp_mipi_dsi_video_stop
 *
 * Description:
 *   Stop DPI video mode and return the DSI Host to command mode.
 *
 ****************************************************************************/

int esp_mipi_dsi_video_stop(FAR struct mipi_dsi_host *host);

#endif /* __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_MIPI_DSI_H */
