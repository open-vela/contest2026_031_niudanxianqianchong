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
  struct esp_mipi_dsi_dpi_panel_s dpi_panel;
  FAR struct mipi_dsi_host *host;
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

  if (fb == NULL || area == NULL || fb->planeinfo.fbmem == NULL ||
      area->x > fb->videoinfo.xres || area->y > fb->videoinfo.yres ||
      area->w > fb->videoinfo.xres - area->x ||
      area->h > fb->videoinfo.yres - area->y)
    {
      return -EINVAL;
    }

  /* The P4 DSI Bridge reads the entire persistent PSRAM buffer in a cyclic
   * scanout.  Clean the complete buffer instead of a partial cache range:
   * this keeps the first framebuffer implementation correct for all caller
   * alignments.  A later LVGL phase may optimize this to aligned rectangles.
   */

  return esp_mipi_dsi_dma_buffer_sync_for_device(fb->planeinfo.fbmem,
                                                  fb->planeinfo.fblen);
}
#endif

static void esp32p4_dsi_fb_cleanup(FAR struct esp32p4_dsi_fb_s *fb)
{
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
  size_t frame_buffer_bytes;
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
  fb->panel_config.dpi_config = board_mipi_dsi_dpi_panel_config_get();

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

  ret = esp_mipi_dsi_dpi_panel_get_frame_buffer(&fb->dpi_panel,
                                                 &fb->planeinfo.fbmem,
                                                 &frame_buffer_bytes);
  if (ret < 0)
    {
      goto errout;
    }

  fb->videoinfo.fmt = FB_FMT_RGB16_565;
  fb->videoinfo.xres = ESP32P4_DSI_FB_WIDTH;
  fb->videoinfo.yres = ESP32P4_DSI_FB_HEIGHT;
  fb->videoinfo.nplanes = 1;

  fb->planeinfo.fblen = frame_buffer_bytes;
  fb->planeinfo.stride = ESP32P4_DSI_FB_STRIDE;
  fb->planeinfo.display = display;
  fb->planeinfo.bpp = ESP32P4_DSI_FB_BPP;
  fb->planeinfo.xres_virtual = ESP32P4_DSI_FB_WIDTH;
  fb->planeinfo.yres_virtual = ESP32P4_DSI_FB_HEIGHT;

  if (frame_buffer_bytes != (size_t)ESP32P4_DSI_FB_STRIDE *
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
#ifdef CONFIG_FB_UPDATE
  fb->vtable.updatearea = esp32p4_dsi_fb_updatearea;
#endif

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
         "/dev/fb%d %ux%u RGB565 buffer=%p bytes=%zu\n", display,
         fb->videoinfo.xres, fb->videoinfo.yres, fb->planeinfo.fbmem,
         fb->planeinfo.fblen);
  return OK;

errout:
  syslog(LOG_ERR, "ERROR: P4X MIPI-DSI framebuffer init failed: %d\n", ret);
  esp32p4_dsi_fb_cleanup(fb);
  return ret;
}

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_DSI_FRAMEBUFFER */
