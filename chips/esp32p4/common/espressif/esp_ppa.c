/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_ppa.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include "esp_graphics_priv.h"
#include "esp_private/periph_ctrl.h"
#include "hal/ppa_ll.h"
#include "soc/dma2d_channel.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static ppa_srm_color_mode_t esp_ppa_format(enum esp_graphics_format_e format)
{
  switch (format)
    {
      case ESP_GRAPHICS_RGB565:
        return PPA_SRM_COLOR_MODE_RGB565;
      case ESP_GRAPHICS_RGB888:
        return PPA_SRM_COLOR_MODE_RGB888;
      case ESP_GRAPHICS_ARGB8888:
        return PPA_SRM_COLOR_MODE_ARGB8888;
      default:
        return PPA_SRM_COLOR_MODE_YUV422_UYVY;
    }
}

void esp_ppa_reset(void)
{
  PERIPH_RCC_ATOMIC()
    {
      ppa_ll_enable_bus_clock(true);
      ppa_ll_reset_register();
    }

  /* PPA EOF precedes DMA writeback; only DMA EOF returns buffer ownership.
   * Parameter errors use PPA's own interrupt to avoid a stalled DMA timeout.
   */

  (PPA_LL_GET_HW)->int_clr.val = 0x0f;
  (PPA_LL_GET_HW)->int_ena.val = PPA_SR_PARAM_CFG_ERR_INT_ENA |
                             PPA_BLEND_PARAM_CFG_ERR_INT_ENA;
}

uint32_t esp_ppa_errors(void)
{
  uint32_t status = (PPA_LL_GET_HW)->int_st.val;

  (PPA_LL_GET_HW)->int_clr.val = status;
  return status;
}

void esp_ppa_uninitialize(void)
{
  (PPA_LL_GET_HW)->int_ena.val = 0;
  PERIPH_RCC_ATOMIC()
    {
      ppa_ll_enable_bus_clock(false);
    }
}

int esp_ppa_validate(FAR const struct esp_graphics_job_s *job)
{
  uint32_t width = job->width;
  uint32_t height = job->height;
  int ret;

  if (job->operation != ESP_GRAPHICS_FILL &&
      job->operation != ESP_GRAPHICS_SRM &&
      job->operation != ESP_GRAPHICS_BLEND)
    {
      return -ENOTSUP;
    }

  if (job->operation != ESP_GRAPHICS_FILL)
    {
      ret = esp_graphics_check_buffer(&job->src, job->src_x, job->src_y,
                                      width, height);
      if (ret < 0)
        {
          return ret;
        }
    }

  if (job->operation == ESP_GRAPHICS_SRM)
    {
      if (job->scale_x_q4 == 0 || job->scale_y_q4 == 0 ||
          job->scale_x_q4 >= PPA_LL_SRM_SCALING_INT_MAX * 16 ||
          job->scale_y_q4 >= PPA_LL_SRM_SCALING_INT_MAX * 16 ||
          (job->rotation != 0 && job->rotation != 90 &&
           job->rotation != 180 && job->rotation != 270))
        {
          return -EINVAL;
        }

      width = (uint32_t)job->width * job->scale_x_q4 / 16;
      height = (uint32_t)job->height * job->scale_y_q4 / 16;
      if (job->rotation == 90 || job->rotation == 270)
        {
          uint32_t temporary = width;
          width = height;
          height = temporary;
        }

      /* YUV output has additional per-macroblock chroma restrictions.
       * The first port supports YUV422 input to RGB, and RGB conversions.
       */

      if (job->dst.format == ESP_GRAPHICS_YUV422)
        {
          return -ENOTSUP;
        }
    }

  if (job->operation == ESP_GRAPHICS_BLEND)
    {
      if (job->src.format == ESP_GRAPHICS_YUV422 ||
          job->foreground.format == ESP_GRAPHICS_YUV422 ||
          job->dst.format == ESP_GRAPHICS_YUV422)
        {
          return -ENOTSUP;
        }

      ret = esp_graphics_check_buffer(&job->foreground, job->fg_x, job->fg_y,
                                      width, height);
      if (ret < 0)
        {
          return ret;
        }
    }

  if (job->operation == ESP_GRAPHICS_FILL &&
      job->dst.format == ESP_GRAPHICS_YUV422)
    {
      return -ENOTSUP;
    }

  if (job->operation == ESP_GRAPHICS_SRM && (width < 2 || height < 2))
    {
      return -ENOTSUP;
    }

  return esp_graphics_check_buffer(&job->dst, job->dst_x, job->dst_y,
                                   width, height);
}

static void esp_ppa_srm(FAR struct esp_graphics_s *priv)
{
  FAR const struct esp_graphics_job_s *job = &priv->job;
  FAR ppa_dev_t *dev = PPA_LL_GET_HW;
  ppa_srm_color_mode_t src = esp_ppa_format(job->src.format);
  ppa_srm_color_mode_t dst = esp_ppa_format(job->dst.format);
  uint32_t block_h;
  uint32_t block_v;
  uint32_t out_w = (uint32_t)job->width * job->scale_x_q4 / 16;
  uint32_t divisor = esp_graphics_bpp(job->dst.format) >= 3 ? 32 : 64;
  uint32_t left_w = out_w % divisor;
  uint32_t left_h = job->height % 16;
  bool bypass;

  esp_graphics_descriptor(&priv->tx[0].dma, &job->src,
                           job->src_x, job->src_y, job->width, job->height);
  esp_graphics_descriptor(&priv->rx[0].dma, &job->dst,
                           job->dst_x, job->dst_y, 2, 2);
  esp_graphics_tx(0, DMA2D_TRIG_PERIPH_PPA_SRM,
                   SOC_DMA2D_TRIG_PERIPH_PPA_SRM_TX, &priv->tx[0].dma);
  esp_graphics_rx(DMA2D_TRIG_PERIPH_PPA_SRM,
                   SOC_DMA2D_TRIG_PERIPH_PPA_SRM_RX, &priv->rx[0].dma);

  ppa_ll_srm_set_mb_size(dev, PPA_LL_SRM_MB_SIZE_16_16);
  ppa_ll_srm_get_dma_dscr_port_mode_block_size(dev, src,
                                              PPA_LL_SRM_MB_SIZE_16_16,
                                              &block_h, &block_v);
  dma2d_ll_tx_set_dscr_port_block_size(DMA2D_LL_GET_HW(0), 0,
                                       block_h, block_v);
  dma2d_ll_tx_enable_dscr_port(DMA2D_LL_GET_HW(0), 0, true);
  ppa_ll_srm_set_rx_color_mode(dev, src);
  if (job->src.format == ESP_GRAPHICS_YUV422)
    {
      ppa_ll_srm_set_rx_yuv_range(dev, PPA_COLOR_RANGE_LIMIT);
      ppa_ll_srm_set_rx_yuv2rgb_std(dev, PPA_COLOR_CONV_STD_RGB_YUV_BT601);
    }

  ppa_ll_srm_enable_rx_byte_swap(dev, false);
  ppa_ll_srm_enable_rx_rgb_swap(dev, false);
  ppa_ll_srm_configure_rx_alpha(dev, PPA_ALPHA_NO_CHANGE, 0);
  ppa_ll_srm_set_tx_color_mode(dev, dst);
  ppa_ll_srm_set_rotation_angle(dev, job->rotation / 90);
  ppa_ll_srm_set_scaling_x(dev, job->scale_x_q4 / 16, job->scale_x_q4 % 16);
  ppa_ll_srm_set_scaling_y(dev, job->scale_y_q4 / 16, job->scale_y_q4 % 16);
  ppa_ll_srm_enable_mirror_x(dev, job->mirror_x);
  ppa_ll_srm_enable_mirror_y(dev, job->mirror_y);

  /* DIG-734: preserve the pinned HAL's small-tail macroblock workaround. */

  left_w = left_w == 0 ? divisor : left_w;
  left_h = left_h == 0 ? 16 : left_h;
  left_h = left_h * job->scale_y_q4 / 16;
  bypass = (out_w > divisor || job->height > 16) &&
           left_w * left_h * esp_graphics_bpp(job->dst.format) * 8 < 1536;
  ppa_ll_srm_bypass_mb_order(dev, bypass);
}

static void esp_ppa_blend(FAR struct esp_graphics_s *priv)
{
  FAR const struct esp_graphics_job_s *job = &priv->job;
  FAR ppa_dev_t *dev = PPA_LL_GET_HW;
  color_pixel_rgb888_data_t low;
  color_pixel_rgb888_data_t high;

  low.val = 0;
  high.val = 0xffffff;

  esp_graphics_descriptor(&priv->tx[0].dma, &job->src,
                           job->src_x, job->src_y, job->width, job->height);
  esp_graphics_descriptor(&priv->fg->dma, &job->foreground,
                           job->fg_x, job->fg_y, job->width, job->height);
  esp_graphics_tx(0, DMA2D_TRIG_PERIPH_PPA_BLEND,
                   SOC_DMA2D_TRIG_PERIPH_PPA_BLEND_BG_TX, &priv->tx[0].dma);
  esp_graphics_tx(1, DMA2D_TRIG_PERIPH_PPA_BLEND,
                   SOC_DMA2D_TRIG_PERIPH_PPA_BLEND_FG_TX, &priv->fg->dma);
  ppa_ll_blend_set_rx_bg_color_mode(dev, esp_ppa_format(job->src.format));
  ppa_ll_blend_set_rx_fg_color_mode(dev,
                                    esp_ppa_format(job->foreground.format));
  ppa_ll_blend_enable_rx_bg_rgb_swap(dev, false);
  ppa_ll_blend_enable_rx_fg_rgb_swap(dev, false);
  ppa_ll_blend_enable_rx_bg_byte_swap(dev, false);
  ppa_ll_blend_enable_rx_fg_byte_swap(dev, false);
  ppa_ll_blend_configure_rx_bg_alpha(dev, job->bg_pixel_alpha ?
                                     PPA_ALPHA_NO_CHANGE :
                                     PPA_ALPHA_FIX_VALUE,
                                     job->bg_alpha);
  ppa_ll_blend_configure_rx_fg_alpha(dev, job->fg_pixel_alpha ?
                                     PPA_ALPHA_NO_CHANGE :
                                     PPA_ALPHA_FIX_VALUE,
                                     job->fg_alpha);
  ppa_ll_blend_configure_rx_bg_ck_range(dev, &high, &low);
  ppa_ll_blend_configure_rx_fg_ck_range(dev, &high, &low);
  ppa_ll_blend_set_ck_default_rgb(dev, &low);
  ppa_ll_blend_enable_ck_fg_bg_reverse(dev, false);
}

int esp_ppa_prepare(FAR struct esp_graphics_s *priv)
{
  FAR const struct esp_graphics_job_s *job = &priv->job;
  FAR ppa_dev_t *dev = PPA_LL_GET_HW;

  if (job->operation == ESP_GRAPHICS_SRM)
    {
      esp_ppa_srm(priv);
      return OK;
    }

  esp_graphics_descriptor(&priv->rx[0].dma, &job->dst,
                           job->dst_x, job->dst_y, job->width, job->height);
  esp_graphics_rx(DMA2D_TRIG_PERIPH_PPA_BLEND,
                   SOC_DMA2D_TRIG_PERIPH_PPA_BLEND_RX, &priv->rx[0].dma);
  if (job->operation == ESP_GRAPHICS_FILL)
    {
      ppa_ll_blend_configure_filling_block(dev,
                                           esp_ppa_format(job->dst.format),
                                           (void *)&job->color,
                                           job->width, job->height);
    }
  else
    {
      esp_ppa_blend(priv);
    }

  ppa_ll_blend_set_tx_color_mode(dev, esp_ppa_format(job->dst.format));
  return OK;
}

void esp_ppa_start(FAR const struct esp_graphics_job_s *job)
{
  if (job->operation == ESP_GRAPHICS_SRM)
    {
      ppa_ll_srm_start(PPA_LL_GET_HW);
    }
  else
    {
      ppa_ll_blend_start(PPA_LL_GET_HW, job->operation == ESP_GRAPHICS_FILL ?
                          PPA_LL_BLEND_TRANS_MODE_FILL :
                          PPA_LL_BLEND_TRANS_MODE_BLEND);
    }
}
