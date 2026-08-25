/****************************************************************************
 * chips/esp32p4/common/espressif/esp_mipi_dsi_dpi_panel.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <string.h>

#include <arch/chip/esp_mipi_dsi.h>
#include <arch/chip/esp_mipi_dsi_dpi_panel.h>

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_mipi_dsi_dpi_panel_bytes_per_pixel
 ****************************************************************************/

static int esp_mipi_dsi_dpi_panel_bytes_per_pixel(
  enum esp_mipi_dsi_dpi_color_format_e format, FAR size_t *bytes_per_pixel)
{
  if (bytes_per_pixel == NULL)
    {
      return -EINVAL;
    }

  switch (format)
    {
      case ESP_MIPI_DSI_DPI_COLOR_RGB565:
        *bytes_per_pixel = 2;
        return OK;

      case ESP_MIPI_DSI_DPI_COLOR_RGB888:
        *bytes_per_pixel = 3;
        return OK;

      default:
        return -EINVAL;
    }
}

/****************************************************************************
 * Name: esp_mipi_dsi_dpi_panel_validate
 ****************************************************************************/

static int esp_mipi_dsi_dpi_panel_validate(
  FAR const struct esp_mipi_dsi_dpi_panel_config_s *config,
  FAR size_t *frame_buffer_bytes)
{
  size_t bytes_per_pixel;
  int ret;

  if (config == NULL || frame_buffer_bytes == NULL ||
      config->channel > 3 || config->hactive == 0 ||
      config->vactive == 0 || config->pixel_clock_hz == 0 ||
      config->input_format != config->output_format)
    {
      return -EINVAL;
    }

  ret = esp_mipi_dsi_dpi_panel_bytes_per_pixel(config->input_format,
                                                &bytes_per_pixel);
  if (ret < 0)
    {
      return ret;
    }

  if (config->hactive > SIZE_MAX / config->vactive ||
      (size_t)config->hactive * config->vactive >
        SIZE_MAX / bytes_per_pixel)
    {
      return -EOVERFLOW;
    }

  *frame_buffer_bytes = (size_t)config->hactive * config->vactive *
                        bytes_per_pixel;
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_mipi_dsi_dpi_panel_create
 ****************************************************************************/

int esp_mipi_dsi_dpi_panel_create(
  FAR struct esp_mipi_dsi_dpi_panel_s *panel,
  FAR struct mipi_dsi_host *host,
  FAR const struct esp_mipi_dsi_dpi_panel_config_s *config)
{
  size_t frame_buffer_bytes;
  uint8_t frame_buffer_count;
  int ret;

  if (panel == NULL || host == NULL || config == NULL)
    {
      return -EINVAL;
    }

  ret = esp_mipi_dsi_dpi_panel_validate(config, &frame_buffer_bytes);
  if (ret < 0)
    {
      return ret;
    }

  frame_buffer_count = config->frame_buffer_count;
  if (frame_buffer_count == 0)
    {
      frame_buffer_count = 1;
    }

  if (frame_buffer_count > ESP_MIPI_DSI_DPI_PANEL_MAX_FRAME_BUFFERS)
    {
      return -EINVAL;
    }

  if (frame_buffer_bytes > SIZE_MAX / frame_buffer_count)
    {
      return -EOVERFLOW;
    }

  memset(panel, 0, sizeof(*panel));
  panel->host = host;
  panel->config = *config;
  panel->config.frame_buffer_count = frame_buffer_count;
  panel->frame_buffer_bytes = frame_buffer_bytes;
  panel->frame_buffer_count = frame_buffer_count;
  panel->created = true;
  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_dpi_panel_initialize
 ****************************************************************************/

int esp_mipi_dsi_dpi_panel_initialize(
  FAR struct esp_mipi_dsi_dpi_panel_s *panel)
{
  struct esp_mipi_dsi_video_dma_config_s config;
  FAR void *frame_buffer;
  size_t allocation_bytes;
  int ret;

  if (panel == NULL || !panel->created || panel->host == NULL)
    {
      return -EINVAL;
    }

  if (panel->initialized)
    {
      return OK;
    }

  allocation_bytes = panel->frame_buffer_bytes * panel->frame_buffer_count;
  ret = esp_mipi_dsi_dma_buffer_allocate(allocation_bytes,
                                         &frame_buffer);
  if (ret < 0)
    {
      return ret;
    }

  memset(&config, 0, sizeof(config));
  config.channel = panel->config.channel;
  config.hactive = panel->config.hactive;
  config.hsync = panel->config.hsync;
  config.hback_porch = panel->config.hback_porch;
  config.hfront_porch = panel->config.hfront_porch;
  config.vactive = panel->config.vactive;
  config.vsync = panel->config.vsync;
  config.vback_porch = panel->config.vback_porch;
  config.vfront_porch = panel->config.vfront_porch;
  config.pixel_clock_hz = panel->config.pixel_clock_hz;
  config.hsync_active_low = panel->config.hsync_active_low;
  config.vsync_active_low = panel->config.vsync_active_low;
  config.input_format = panel->config.input_format;
  config.output_format = panel->config.output_format;
  config.frame_buffer = frame_buffer;
  config.frame_buffer_bytes = panel->frame_buffer_bytes;

  ret = esp_mipi_dsi_dma_buffer_sync_for_device(
    frame_buffer, allocation_bytes);
  if (ret < 0)
    {
      esp_mipi_dsi_dma_buffer_free(frame_buffer);
      return ret;
    }

  ret = esp_mipi_dsi_video_dma_start(panel->host, &config);
  if (ret < 0)
    {
      esp_mipi_dsi_dma_buffer_free(frame_buffer);
      return ret;
    }

  panel->frame_buffer = frame_buffer;
  panel->initialized = true;
  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_dpi_panel_get_frame_buffer
 ****************************************************************************/

int esp_mipi_dsi_dpi_panel_get_frame_buffer(
  FAR struct esp_mipi_dsi_dpi_panel_s *panel,
  FAR void **frame_buffer, FAR size_t *frame_buffer_bytes)
{
  if (panel == NULL || frame_buffer == NULL || frame_buffer_bytes == NULL ||
      !panel->initialized || panel->frame_buffer == NULL)
    {
      return -EINVAL;
    }

  *frame_buffer = panel->frame_buffer;
  *frame_buffer_bytes = panel->frame_buffer_bytes;
  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_dpi_panel_get_frame_buffers
 ****************************************************************************/

int esp_mipi_dsi_dpi_panel_get_frame_buffers(
  FAR struct esp_mipi_dsi_dpi_panel_s *panel,
  FAR void **frame_buffer, FAR size_t *frame_buffer_bytes,
  FAR uint8_t *frame_buffer_count)
{
  if (panel == NULL || frame_buffer == NULL || frame_buffer_bytes == NULL ||
      frame_buffer_count == NULL || !panel->initialized ||
      panel->frame_buffer == NULL)
    {
      return -EINVAL;
    }

  *frame_buffer = panel->frame_buffer;
  *frame_buffer_bytes = panel->frame_buffer_bytes;
  *frame_buffer_count = panel->frame_buffer_count;
  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_dpi_panel_draw_bitmap
 ****************************************************************************/

int esp_mipi_dsi_dpi_panel_draw_bitmap(
  FAR struct esp_mipi_dsi_dpi_panel_s *panel,
  uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end,
  FAR const void *color_data, size_t color_data_bytes)
{
  if (panel == NULL || !panel->initialized || panel->frame_buffer == NULL ||
      color_data == NULL || x_start != 0 || y_start != 0 ||
      x_end != panel->config.hactive || y_end != panel->config.vactive ||
      color_data_bytes != panel->frame_buffer_bytes)
    {
      return -EINVAL;
    }

  /* Use memmove so callers may render directly into the persistent frame
   * buffer returned by esp_mipi_dsi_dpi_panel_get_frame_buffer().
   */

  memmove(panel->frame_buffer, color_data, panel->frame_buffer_bytes);
  return esp_mipi_dsi_dma_buffer_sync_for_device(
    panel->frame_buffer, panel->frame_buffer_bytes);
}

/****************************************************************************
 * Name: esp_mipi_dsi_dpi_panel_stop
 ****************************************************************************/

int esp_mipi_dsi_dpi_panel_stop(
  FAR struct esp_mipi_dsi_dpi_panel_s *panel)
{
  int ret;

  if (panel == NULL || !panel->created)
    {
      return -EINVAL;
    }

  if (!panel->initialized)
    {
      return OK;
    }

  ret = esp_mipi_dsi_video_stop(panel->host);
  if (ret < 0)
    {
      return ret;
    }

  panel->initialized = false;
  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_dpi_panel_destroy
 ****************************************************************************/

void esp_mipi_dsi_dpi_panel_destroy(
  FAR struct esp_mipi_dsi_dpi_panel_s *panel)
{
  int ret;

  if (panel == NULL)
    {
      return;
    }

  if (panel->initialized)
    {
      ret = esp_mipi_dsi_dpi_panel_stop(panel);
      if (ret < 0)
        {
          /* The DMA channel may still own the frame buffer.  Preserve the
           * object rather than freeing memory that hardware can access.
           */

          return;
        }
    }

  esp_mipi_dsi_dma_buffer_free(panel->frame_buffer);
  memset(panel, 0, sizeof(*panel));
}
