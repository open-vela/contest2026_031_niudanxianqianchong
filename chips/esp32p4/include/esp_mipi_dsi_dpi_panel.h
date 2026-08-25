/****************************************************************************
 * chips/esp32p4/include/esp_mipi_dsi_dpi_panel.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_MIPI_DSI_DPI_PANEL_H
#define __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_MIPI_DSI_DPI_PANEL_H

#include <nuttx/compiler.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP_MIPI_DSI_DPI_PANEL_MAX_FRAME_BUFFERS 2

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct mipi_dsi_host;

/* Keep the public format enum independent from ESP HAL headers.  The P4
 * adapter converts it to the corresponding HAL color coding internally.
 */

enum esp_mipi_dsi_dpi_color_format_e
{
  ESP_MIPI_DSI_DPI_COLOR_RGB565 = 0,
  ESP_MIPI_DSI_DPI_COLOR_RGB888,
};

struct esp_mipi_dsi_dpi_panel_config_s
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
  enum esp_mipi_dsi_dpi_color_format_e input_format;
  enum esp_mipi_dsi_dpi_color_format_e output_format;
  uint8_t  frame_buffer_count;
};

/* A caller owns this object.  It models the subset of ESP-IDF's DPI panel
 * needed by the P4X bring-up path: one or more contiguous persistent frame
 * buffers, continuous scanout and full-frame draw_bitmap submission.  A
 * zero frame_buffer_count selects one buffer for compatibility with existing
 * callers.  It intentionally does not expose a /dev/fb device itself.
 */

struct esp_mipi_dsi_dpi_panel_s
{
  FAR struct mipi_dsi_host *host;
  struct esp_mipi_dsi_dpi_panel_config_s config;
  FAR void *frame_buffer;
  size_t frame_buffer_bytes;
  uint8_t frame_buffer_count;
  bool created;
  bool initialized;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int esp_mipi_dsi_dpi_panel_create(
  FAR struct esp_mipi_dsi_dpi_panel_s *panel,
  FAR struct mipi_dsi_host *host,
  FAR const struct esp_mipi_dsi_dpi_panel_config_s *config);

int esp_mipi_dsi_dpi_panel_initialize(
  FAR struct esp_mipi_dsi_dpi_panel_s *panel);

int esp_mipi_dsi_dpi_panel_get_frame_buffer(
  FAR struct esp_mipi_dsi_dpi_panel_s *panel,
  FAR void **frame_buffer, FAR size_t *frame_buffer_bytes);

/****************************************************************************
 * Name: esp_mipi_dsi_dpi_panel_get_frame_buffers
 *
 * Description:
 *   Return the base address, one-frame size and count of the panel-owned
 *   contiguous framebuffer allocation.  The first frame is the initial
 *   scanout source.  Additional frames are for a board framebuffer driver's
 *   page-flip policy and must not be released by the caller.
 *
 ****************************************************************************/

int esp_mipi_dsi_dpi_panel_get_frame_buffers(
  FAR struct esp_mipi_dsi_dpi_panel_s *panel,
  FAR void **frame_buffer, FAR size_t *frame_buffer_bytes,
  FAR uint8_t *frame_buffer_count);

int esp_mipi_dsi_dpi_panel_draw_bitmap(
  FAR struct esp_mipi_dsi_dpi_panel_s *panel,
  uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end,
  FAR const void *color_data, size_t color_data_bytes);

int esp_mipi_dsi_dpi_panel_stop(
  FAR struct esp_mipi_dsi_dpi_panel_s *panel);

void esp_mipi_dsi_dpi_panel_destroy(
  FAR struct esp_mipi_dsi_dpi_panel_s *panel);

#endif /* __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_MIPI_DSI_DPI_PANEL_H */
