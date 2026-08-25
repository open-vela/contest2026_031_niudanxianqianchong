/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_fb.c
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

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_DSI_FRAMEBUFFER

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/video/fb.h>
#include <nuttx/video/mipi_dsi.h>

#include <arch/board/board.h>
#include <arch/chip/esp_mipi_dsi.h>
#include <arch/chip/esp_mipi_dsi_dpi_panel.h>

#include "ek79007.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP32P4_DSI_FB_WIDTH       1024
#define ESP32P4_DSI_FB_HEIGHT       600
#define ESP32P4_DSI_FB_BPP            16
#define ESP32P4_DSI_FB_STRIDE      (ESP32P4_DSI_FB_WIDTH * 2)
#define ESP32P4_DSI_FB_BUFFER_COUNT     2
#define ESP32P4_DSI_FB_HS_RATE_HZ  1000000000
#define ESP32P4_DSI_FB_LP_RATE_HZ    10000000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct esp32p4_dsi_fb_s
{
  struct fb_vtable_s vtable;
  struct fb_videoinfo_s videoinfo;
  struct fb_planeinfo_s planeinfo;
  struct mipi_dsi_device device;
  struct ek79007_panel_s panel;
  struct ek79007_panel_config_s panel_config;
  struct esp_mipi_dsi_dpi_panel_config_s dpi_config;
  struct esp_mipi_dsi_dpi_panel_s dpi_panel;
  FAR struct mipi_dsi_host *host;
  size_t frame_buffer_bytes;
  uint8_t frame_buffer_count;
  bool attached;
  bool registered;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int esp32p4_dsi_fb_getvideoinfo(FAR struct fb_vtable_s *vtable,
                                       FAR struct fb_videoinfo_s *vinfo);
static int esp32p4_dsi_fb_getplaneinfo(FAR struct fb_vtable_s *vtable,
                                       int planeno,
                                       FAR struct fb_planeinfo_s *pinfo);
static int esp32p4_dsi_fb_pandisplay(FAR struct fb_vtable_s *vtable,
                                     FAR struct fb_planeinfo_s *pinfo);
static void esp32p4_dsi_fb_frame_done(FAR void *arg);

#ifdef CONFIG_FB_UPDATE
static int esp32p4_dsi_fb_updatearea(FAR struct fb_vtable_s *vtable,
                                     FAR const struct fb_area_s *area);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct esp32p4_dsi_fb_s g_esp32p4_dsi_fb;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int esp32p4_dsi_fb_getvideoinfo(FAR struct fb_vtable_s *vtable,
                                       FAR struct fb_videoinfo_s *vinfo)
{
  FAR struct esp32p4_dsi_fb_s *fb =
    (FAR struct esp32p4_dsi_fb_s *)vtable;

  if (fb == NULL || vinfo == NULL)
    {
      return -EINVAL;
    }

  *vinfo = fb->videoinfo;
  return OK;
}

static int esp32p4_dsi_fb_getplaneinfo(FAR struct fb_vtable_s *vtable,
                                       int planeno,
                                       FAR struct fb_planeinfo_s *pinfo)
{
  FAR struct esp32p4_dsi_fb_s *fb =
    (FAR struct esp32p4_dsi_fb_s *)vtable;

  if (fb == NULL || pinfo == NULL || planeno != 0)
    {
      return -EINVAL;
    }

  *pinfo = fb->planeinfo;
  return OK;
}

#ifdef CONFIG_FB_UPDATE
static int esp32p4_dsi_fb_updatearea(FAR struct fb_vtable_s *vtable,
                                     FAR const struct fb_area_s *area)
{
  FAR struct esp32p4_dsi_fb_s *fb =
    (FAR struct esp32p4_dsi_fb_s *)vtable;

  uint8_t first_page;
  uint8_t last_page;
  uint8_t page;
  FAR uint8_t *page_buffer;
  int ret;

  if (fb == NULL || area == NULL || fb->planeinfo.fbmem == NULL ||
      area->x > fb->videoinfo.xres ||
      area->y > fb->planeinfo.yres_virtual ||
      area->w > fb->videoinfo.xres - area->x ||
      area->h > fb->planeinfo.yres_virtual - area->y)
    {
      return -EINVAL;
    }

  if (area->w == 0 || area->h == 0)
    {
      return OK;
    }

  first_page = area->y / fb->videoinfo.yres;
  last_page = (area->y + area->h - 1) / fb->videoinfo.yres;
  if (last_page >= fb->frame_buffer_count)
    {
      return -EINVAL;
    }

  /* A direct-rendering LVGL update belongs to exactly one virtual page in
   * normal operation.  Clean complete pages so cache maintenance remains
   * correct for all source and destination alignment combinations.
   */

  for (page = first_page; page <= last_page; page++)
    {
      page_buffer = (FAR uint8_t *)fb->planeinfo.fbmem +
                    page * fb->frame_buffer_bytes;
      ret = esp_mipi_dsi_dma_buffer_sync_for_device(
        page_buffer, fb->frame_buffer_bytes);
      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}
#endif

/****************************************************************************
 * Name: esp32p4_dsi_fb_pandisplay
 ****************************************************************************/

static int esp32p4_dsi_fb_pandisplay(FAR struct fb_vtable_s *vtable,
                                     FAR struct fb_planeinfo_s *pinfo)
{
  FAR struct esp32p4_dsi_fb_s *fb =
    (FAR struct esp32p4_dsi_fb_s *)vtable;
  FAR uint8_t *page_buffer;
  uint8_t page;
  int ret;

  if (fb == NULL || pinfo == NULL || fb->planeinfo.fbmem == NULL ||
      pinfo->xoffset != 0 ||
      pinfo->yoffset % fb->videoinfo.yres != 0 ||
      pinfo->yoffset >= fb->planeinfo.yres_virtual ||
      pinfo->yoffset + fb->videoinfo.yres > fb->planeinfo.yres_virtual)
    {
      return -EINVAL;
    }

  page = pinfo->yoffset / fb->videoinfo.yres;
  if (page >= fb->frame_buffer_count)
    {
      return -EINVAL;
    }

  page_buffer = (FAR uint8_t *)fb->planeinfo.fbmem +
                page * fb->frame_buffer_bytes;
  ret = esp_mipi_dsi_dma_buffer_sync_for_device(page_buffer,
                                                 fb->frame_buffer_bytes);
  if (ret < 0)
    {
      return ret;
    }

  return esp_mipi_dsi_video_dma_queue_frame_buffer(
    fb->host, page_buffer, fb->frame_buffer_bytes);
}

/****************************************************************************
 * Name: esp32p4_dsi_fb_frame_done
 *
 * Description:
 *   Release one submitted page and notify framebuffer waiters.  This is
 *   invoked by the DSI DMA frame-boundary interrupt after the next scanout
 *   descriptor is armed, so it must remain non-blocking.
 ****************************************************************************/

static void esp32p4_dsi_fb_frame_done(FAR void *arg)
{
  FAR struct esp32p4_dsi_fb_s *fb = arg;

  if (fb == NULL || !fb->registered)
    {
      return;
    }

  if (fb_paninfo_count(&fb->vtable, FB_NO_OVERLAY) > 0)
    {
      (void)fb_remove_paninfo(&fb->vtable, FB_NO_OVERLAY);
    }

  fb_notify_vsync(&fb->vtable);
}

static void esp32p4_dsi_fb_cleanup(FAR struct esp32p4_dsi_fb_s *fb)
{
  if (fb->host != NULL)
    {
      (void)esp_mipi_dsi_video_dma_set_frame_done_callback(
        fb->host, NULL, NULL);
    }

  if (fb->panel.dsi != NULL)
    {
      (void)ek79007_panel_shutdown(&fb->panel);
    }

  if (fb->attached)
    {
      (void)mipi_dsi_detach(&fb->device);
    }

  if (fb->host != NULL)
    {
      (void)board_mipi_dsi_shutdown(fb->host);
    }

  memset(fb, 0, sizeof(*fb));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_mipi_dsi_fb_initialize
 ****************************************************************************/

int board_mipi_dsi_fb_initialize(int display)
{
  FAR struct esp32p4_dsi_fb_s *fb = &g_esp32p4_dsi_fb;
  int ret;

  if (display != 0)
    {
      return -ENODEV;
    }

  if (fb->registered)
    {
      return OK;
    }

  memset(fb, 0, sizeof(*fb));

  ret = board_mipi_dsi_initialize(&fb->host);
  if (ret < 0)
    {
      goto errout;
    }

  fb->device.host = fb->host;
  fb->device.channel = 0;
  strlcpy(fb->device.name, "ek79007-fb", sizeof(fb->device.name));

  fb->panel_config.mode_flags = MIPI_DSI_MODE_LPM;
  fb->panel_config.hs_rate = ESP32P4_DSI_FB_HS_RATE_HZ;
  fb->panel_config.lp_rate = ESP32P4_DSI_FB_LP_RATE_HZ;
  fb->panel_config.lanes = 2;
  fb->panel_config.format = MIPI_DSI_FMT_RGB565;
  fb->panel_config.dpi_panel = &fb->dpi_panel;
  fb->dpi_config = *board_mipi_dsi_dpi_panel_config_get();
  fb->dpi_config.frame_buffer_count = ESP32P4_DSI_FB_BUFFER_COUNT;
  fb->panel_config.dpi_config = &fb->dpi_config;

  ret = ek79007_panel_setup(&fb->panel, &fb->device, &fb->panel_config);
  if (ret < 0)
    {
      goto errout;
    }

  ret = mipi_dsi_attach(&fb->device);
  if (ret < 0)
    {
      goto errout;
    }

  fb->attached = true;
  ret = board_mipi_dsi_panel_reset();
  if (ret < 0)
    {
      goto errout;
    }

  ret = ek79007_panel_initialize(&fb->panel);
  if (ret < 0)
    {
      goto errout;
    }

  ret = esp_mipi_dsi_dpi_panel_get_frame_buffers(
    &fb->dpi_panel, &fb->planeinfo.fbmem, &fb->frame_buffer_bytes,
    &fb->frame_buffer_count);
  if (ret < 0)
    {
      goto errout;
    }

  fb->videoinfo.fmt = FB_FMT_RGB16_565;
  fb->videoinfo.xres = ESP32P4_DSI_FB_WIDTH;
  fb->videoinfo.yres = ESP32P4_DSI_FB_HEIGHT;
  fb->videoinfo.nplanes = 1;

  fb->planeinfo.fblen = fb->frame_buffer_bytes * fb->frame_buffer_count;
  fb->planeinfo.stride = ESP32P4_DSI_FB_STRIDE;
  fb->planeinfo.display = display;
  fb->planeinfo.bpp = ESP32P4_DSI_FB_BPP;
  fb->planeinfo.xres_virtual = ESP32P4_DSI_FB_WIDTH;
  fb->planeinfo.yres_virtual = ESP32P4_DSI_FB_HEIGHT *
                                fb->frame_buffer_count;

  if (fb->frame_buffer_count != ESP32P4_DSI_FB_BUFFER_COUNT ||
      fb->frame_buffer_bytes != (size_t)ESP32P4_DSI_FB_STRIDE *
                                ESP32P4_DSI_FB_HEIGHT)
    {
      ret = -EINVAL;
      goto errout;
    }

  memset(fb->planeinfo.fbmem, 0, fb->planeinfo.fblen);
  ret = esp_mipi_dsi_dma_buffer_sync_for_device(fb->planeinfo.fbmem,
                                                 fb->planeinfo.fblen);
  if (ret < 0)
    {
      goto errout;
    }

  fb->vtable.getvideoinfo = esp32p4_dsi_fb_getvideoinfo;
  fb->vtable.getplaneinfo = esp32p4_dsi_fb_getplaneinfo;
  fb->vtable.pandisplay = esp32p4_dsi_fb_pandisplay;
#ifdef CONFIG_FB_UPDATE
  fb->vtable.updatearea = esp32p4_dsi_fb_updatearea;
#endif

  ret = esp_mipi_dsi_video_dma_set_frame_done_callback(
    fb->host, esp32p4_dsi_fb_frame_done, fb);
  if (ret < 0)
    {
      goto errout;
    }

  ret = fb_register_device(display, 0, &fb->vtable);
  if (ret < 0)
    {
      goto errout;
    }

  fb->registered = true;
  ret = board_mipi_dsi_backlight_set(true);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "WARNING: P4X framebuffer registered but "
             "backlight enable failed: %d\n", ret);
    }

  syslog(LOG_INFO, "INFO: P4X MIPI-DSI framebuffer registered: "
         "/dev/fb%d %ux%u RGB565 buffers=%u base=%p bytes=%zu\n", display,
         fb->videoinfo.xres, fb->videoinfo.yres, fb->planeinfo.fbmem,
         fb->frame_buffer_count, fb->planeinfo.fblen);
  return OK;

errout:
  syslog(LOG_ERR, "ERROR: P4X MIPI-DSI framebuffer init failed: %d\n", ret);
  esp32p4_dsi_fb_cleanup(fb);
  return ret;
}

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_DSI_FRAMEBUFFER */
