/****************************************************************************
 * chips/esp32p4/common/espressif/esp_mipi_csi.c
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

#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/spinlock.h>

#include <errno.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_clk_tree.h"
#include "esp_err.h"
#include "esp_irq.h"
#include "esp_private/periph_ctrl.h"
#include "hal/dw_gdma_ll.h"
#include "hal/mipi_csi_brg_ll.h"
#include "hal/mipi_csi_hal.h"
#include "hal/mipi_csi_ll.h"
#include "soc/interrupts.h"
#include "soc/reg_base.h"

#include <arch/chip/esp_mipi_csi.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP_MIPI_CSI_MIN_RATE_MBPS             90
#define ESP_MIPI_CSI_MAX_RATE_MBPS            1500
#define ESP_MIPI_CSI_DMA_CHANNEL                 0
#define ESP_MIPI_CSI_DMA_WIDTH_BYTES             8
#define ESP_MIPI_CSI_DMA_BURST_WORDS           512
#define ESP_MIPI_CSI_DMA_FIFO_THRESHOLD        960
#define ESP_MIPI_CSI_DMA_DONE_EVENTS \
  (DW_GDMA_LL_CHANNEL_EVENT_BLOCK_TFR_DONE | \
   DW_GDMA_LL_CHANNEL_EVENT_DMA_TFR_DONE)
#define ESP_MIPI_CSI_DMA_ERROR_EVENTS \
  (DW_GDMA_LL_CHANNEL_EVENT_SRC_DEC_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_DST_DEC_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_SRC_SLV_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_DST_SLV_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_LLI_RD_DEC_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_LLI_WR_DEC_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_LLI_RD_SLV_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_LLI_WR_SLV_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_SHADOWREG_OR_LLI_INVALID_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_ABORTED)
#define ESP_MIPI_CSI_DMA_EVENTS \
  (ESP_MIPI_CSI_DMA_DONE_EVENTS | ESP_MIPI_CSI_DMA_ERROR_EVENTS)

#define ESP_MIPI_CSI_BRG_FRAME_SIZE_GT          (1u << 0)
#define ESP_MIPI_CSI_BRG_FRAME_SIZE_LT          (1u << 1)
#define ESP_MIPI_CSI_BRG_DISCARD                (1u << 2)
#define ESP_MIPI_CSI_BRG_OVERRUN                (1u << 3)
#define ESP_MIPI_CSI_BRG_FIFO_OVERFLOW          (1u << 4)
#define ESP_MIPI_CSI_BRG_ERROR_EVENTS \
  (ESP_MIPI_CSI_BRG_FRAME_SIZE_GT | ESP_MIPI_CSI_BRG_FRAME_SIZE_LT | \
   ESP_MIPI_CSI_BRG_DISCARD | ESP_MIPI_CSI_BRG_OVERRUN | \
   ESP_MIPI_CSI_BRG_FIFO_OVERFLOW)

#define ESP_MIPI_CSI_HOST_PHY_FATAL             (1u << 0)
#define ESP_MIPI_CSI_HOST_PACKET_FATAL          (1u << 1)
#define ESP_MIPI_CSI_HOST_FRAME_FATAL \
  ((1u << 2) | (1u << 3))
#define ESP_MIPI_CSI_HOST_CRC_FATAL \
  ((1u << 4) | (1u << 5))
#define ESP_MIPI_CSI_HOST_DATA_ID                (1u << 6)
#define ESP_MIPI_CSI_HOST_ECC_CORRECTED          (1u << 7)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct esp_mipi_csi_s
{
  mipi_csi_hal_context_t      hal;
  mutex_t                     lock;
  spinlock_t                  irq_lock;
  sem_t                       frame_sem;
  dw_gdma_link_list_item_t    dma_lli;
  FAR dw_gdma_dev_t           *dma_dev;
  FAR void                    *frame_buffer;
  size_t                      expected_frame_bytes;
  struct esp_mipi_csi_stats_s stats;
  int                         dma_cpuint;
  int                         bridge_cpuint;
  bool                        phy_clock_enabled;
  bool                        initialized;
  bool                        running;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* ESP32-P4 has one CSI receiver.  The single static instance also makes the
 * ISR ownership explicit; all externally visible state is protected by lock
 * or irq_lock.
 */

static struct esp_mipi_csi_s g_esp_mipi_csi =
{
  .lock = NXMUTEX_INITIALIZER,
  .irq_lock = SP_UNLOCKED,
  .dma_cpuint = -1,
  .bridge_cpuint = -1,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int esp_mipi_csi_result(esp_err_t result)
{
  if (result == ESP_OK)
    {
      return OK;
    }

  return result < 0 ? result : -EIO;
}

static int esp_mipi_csi_frame_bytes(
  FAR const struct esp_mipi_csi_config_s *config, FAR size_t *bytes)
{
  uint64_t bits;

  if (config->width == 0 || config->height == 0 ||
      config->bits_per_pixel == 0 || bytes == NULL)
    {
      return -EINVAL;
    }

  bits = (uint64_t)config->width * config->height *
         config->bits_per_pixel;
  if ((bits & 7) != 0 || bits / 8 > SIZE_MAX)
    {
      return -EINVAL;
    }

  *bytes = bits / 8;
  return OK;
}

static bool esp_mipi_csi_data_type_valid(uint8_t data_type,
                                         uint8_t bits_per_pixel)
{
  if (data_type == ESP_MIPI_CSI_DT_RAW8)
    {
      return bits_per_pixel == 8;
    }

  if (data_type == ESP_MIPI_CSI_DT_RAW10)
    {
      return bits_per_pixel == 10;
    }

  if (data_type == ESP_MIPI_CSI_DT_RAW12)
    {
      return bits_per_pixel == 12;
    }

  return false;
}

static void esp_mipi_csi_count_host_status(FAR struct esp_mipi_csi_s *priv,
                                            uint32_t status)
{
  if ((status & ESP_MIPI_CSI_HOST_ECC_CORRECTED) != 0)
    {
      priv->stats.csi_ecc_error_count++;
    }

  if ((status & ESP_MIPI_CSI_HOST_CRC_FATAL) != 0)
    {
      priv->stats.csi_crc_error_count++;
    }

  if ((status & ESP_MIPI_CSI_HOST_PHY_FATAL) != 0)
    {
      priv->stats.csi_phy_error_count++;
    }

  if ((status & (ESP_MIPI_CSI_HOST_PACKET_FATAL |
                 ESP_MIPI_CSI_HOST_FRAME_FATAL |
                 ESP_MIPI_CSI_HOST_DATA_ID)) != 0)
    {
      priv->stats.csi_packet_error_count++;
    }

  priv->stats.last_host_status = status;
}

static int esp_mipi_csi_dma_interrupt(int irq, FAR void *context,
                                      FAR void *arg)
{
  FAR struct esp_mipi_csi_s *priv = arg;
  uint32_t dma_status;
  uint32_t host_status;

  (void)irq;
  (void)context;

  if (priv == NULL || !priv->running || priv->dma_dev == NULL)
    {
      return OK;
    }

  dma_status = dw_gdma_ll_channel_get_intr_status(
    priv->dma_dev, ESP_MIPI_CSI_DMA_CHANNEL);
  if (dma_status == 0)
    {
      return OK;
    }

  dw_gdma_ll_channel_clear_intr(priv->dma_dev, ESP_MIPI_CSI_DMA_CHANNEL,
                                dma_status);
  priv->stats.last_dma_status = dma_status;

  if ((dma_status & ESP_MIPI_CSI_DMA_ERROR_EVENTS) != 0)
    {
      priv->stats.dma_error_count++;
      return OK;
    }

  if ((dma_status & ESP_MIPI_CSI_DMA_DONE_EVENTS) != 0)
    {
      host_status = priv->hal.host_dev->int_st_main.val;
      esp_mipi_csi_count_host_status(priv, host_status);

      /* The P4 invalidates terminal descriptors after use.  Revalidate the
       * same caller-owned buffer before the next frame arrives.
       */

      dw_gdma_ll_lli_set_block_markers(&priv->dma_lli, false, true, true);
      esp_cache_msync(&priv->dma_lli, sizeof(priv->dma_lli),
                      ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                      ESP_CACHE_MSYNC_FLAG_UNALIGNED);
      dw_gdma_ll_channel_set_link_list_head_addr(
        priv->dma_dev, ESP_MIPI_CSI_DMA_CHANNEL,
        (uint32_t)(uintptr_t)&priv->dma_lli);
      dw_gdma_ll_channel_enable(priv->dma_dev, ESP_MIPI_CSI_DMA_CHANNEL,
                                 true);
      priv->stats.frame_count++;
      nxsem_post(&priv->frame_sem);
    }

  return OK;
}

static int esp_mipi_csi_bridge_interrupt(int irq, FAR void *context,
                                         FAR void *arg)
{
  FAR struct esp_mipi_csi_s *priv = arg;
  uint32_t status;

  (void)irq;
  (void)context;

  if (priv == NULL || !priv->running || priv->hal.bridge_dev == NULL)
    {
      return OK;
    }

  status = priv->hal.bridge_dev->int_st.val & ESP_MIPI_CSI_BRG_ERROR_EVENTS;
  if (status == 0)
    {
      return OK;
    }

  priv->hal.bridge_dev->int_clr.val = status;
  priv->stats.last_bridge_status = status;

  if ((status & ESP_MIPI_CSI_BRG_OVERRUN) != 0)
    {
      priv->stats.bridge_overrun_count++;
    }

  if ((status & ESP_MIPI_CSI_BRG_FIFO_OVERFLOW) != 0)
    {
      priv->stats.bridge_fifo_overflow_count++;
    }

  if ((status & ESP_MIPI_CSI_BRG_DISCARD) != 0)
    {
      priv->stats.bridge_discard_count++;
    }

  if ((status & (ESP_MIPI_CSI_BRG_FRAME_SIZE_GT |
                 ESP_MIPI_CSI_BRG_FRAME_SIZE_LT)) != 0)
    {
      priv->stats.bridge_frame_size_error_count++;
    }

  return OK;
}

static void esp_mipi_csi_disable_interrupts(FAR struct esp_mipi_csi_s *priv)
{
  if (priv->bridge_cpuint >= 0)
    {
      up_disable_irq(ESP_SOURCE2IRQ(ETS_CSI_BRIDGE_INTR_SOURCE));
      esp_teardown_irq(ETS_CSI_BRIDGE_INTR_SOURCE, priv->bridge_cpuint);
      priv->bridge_cpuint = -1;
    }

  if (priv->dma_cpuint >= 0)
    {
      up_disable_irq(ESP_SOURCE2IRQ(ETS_DW_GDMA_INTR_SOURCE));
      esp_teardown_irq(ETS_DW_GDMA_INTR_SOURCE, priv->dma_cpuint);
      priv->dma_cpuint = -1;
    }
}

static void esp_mipi_csi_release_dma(FAR struct esp_mipi_csi_s *priv)
{
  esp_mipi_csi_disable_interrupts(priv);

  if (priv->dma_dev != NULL)
    {
      dw_gdma_ll_channel_enable(priv->dma_dev, ESP_MIPI_CSI_DMA_CHANNEL,
                                 false);
      dw_gdma_ll_channel_enable_intr_generation(
        priv->dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, UINT32_MAX, false);
      dw_gdma_ll_channel_enable_intr_propagation(
        priv->dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, UINT32_MAX, false);
      dw_gdma_ll_enable_intr_global(priv->dma_dev, false);
      dw_gdma_ll_enable_controller(priv->dma_dev, false);

      PERIPH_RCC_ATOMIC()
        {
          dw_gdma_ll_enable_bus_clock(ESP_MIPI_CSI_BUS0, false);
        }

      priv->dma_dev = NULL;
    }

  memset(&priv->dma_lli, 0, sizeof(priv->dma_lli));
  priv->frame_buffer = NULL;
}

static int esp_mipi_csi_prepare_dma(FAR struct esp_mipi_csi_s *priv,
                                    FAR void *buffer, size_t bytes)
{
  FAR dw_gdma_dev_t *dma_dev;
  FAR dw_gdma_link_list_item_t *lli;
  int ret;

  if (((uintptr_t)buffer & (ESP_MIPI_CSI_DMA_WIDTH_BYTES - 1)) != 0 ||
      (bytes % ESP_MIPI_CSI_DMA_WIDTH_BYTES) != 0)
    {
      return -EINVAL;
    }

  PERIPH_RCC_ATOMIC()
    {
      dw_gdma_ll_enable_bus_clock(ESP_MIPI_CSI_BUS0, true);
      dw_gdma_ll_reset_register(ESP_MIPI_CSI_BUS0);
    }

  dma_dev = DW_GDMA_LL_GET_HW(ESP_MIPI_CSI_BUS0);
  if (dma_dev == NULL)
    {
      return -ENODEV;
    }

  priv->dma_dev = dma_dev;
  priv->frame_buffer = buffer;
  lli = &priv->dma_lli;
  memset(lli, 0, sizeof(*lli));

  dw_gdma_ll_reset(dma_dev);
  dw_gdma_ll_enable_controller(dma_dev, true);
  dw_gdma_ll_enable_intr_global(dma_dev, false);
  dw_gdma_ll_channel_set_trans_flow(
    dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, DW_GDMA_ROLE_PERIPH_CSI,
    DW_GDMA_ROLE_MEM, DW_GDMA_FLOW_CTRL_SRC);
  dw_gdma_ll_channel_set_src_multi_block_type(
    dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, DW_GDMA_BLOCK_TRANSFER_LIST);
  dw_gdma_ll_channel_set_dst_multi_block_type(
    dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, DW_GDMA_BLOCK_TRANSFER_LIST);
  dw_gdma_ll_channel_set_src_handshake_interface(
    dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, DW_GDMA_HANDSHAKE_HW);
  dw_gdma_ll_channel_set_dst_handshake_interface(
    dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, DW_GDMA_HANDSHAKE_HW);
  dw_gdma_ll_channel_set_src_handshake_periph(
    dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, DW_GDMA_ROLE_PERIPH_CSI);
  dw_gdma_ll_channel_set_priority(dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, 1);
  dw_gdma_ll_channel_set_src_outstanding_limit(
    dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, 5);
  dw_gdma_ll_channel_set_dst_outstanding_limit(
    dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, 5);
  dw_gdma_ll_channel_set_src_periph_status_addr(
    dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, MIPI_CSI_BRG_MEM_BASE);
  dw_gdma_ll_channel_clear_intr(dma_dev, ESP_MIPI_CSI_DMA_CHANNEL,
                                UINT32_MAX);
  dw_gdma_ll_channel_enable_intr_generation(
    dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, UINT32_MAX, false);
  dw_gdma_ll_channel_enable_intr_propagation(
    dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, UINT32_MAX, false);
  dw_gdma_ll_channel_enable_intr_generation(
    dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, ESP_MIPI_CSI_DMA_EVENTS, true);
  dw_gdma_ll_channel_enable_intr_propagation(
    dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, ESP_MIPI_CSI_DMA_EVENTS, true);
  dw_gdma_ll_channel_set_link_list_master_port(
    dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, DW_GDMA_LL_MASTER_PORT_MEMORY);
  dw_gdma_ll_channel_set_link_list_head_addr(
    dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, (uint32_t)(uintptr_t)lli);

  dw_gdma_ll_lli_set_src_addr(lli, MIPI_CSI_BRG_MEM_BASE);
  dw_gdma_ll_lli_set_dst_addr(lli, (uint32_t)(uintptr_t)buffer);
  dw_gdma_ll_lli_set_trans_block_size(
    lli, bytes / ESP_MIPI_CSI_DMA_WIDTH_BYTES);
  dw_gdma_ll_lli_set_src_master_port(lli, MIPI_CSI_BRG_MEM_BASE);
  dw_gdma_ll_lli_set_dst_master_port(lli, (intptr_t)buffer);
  dw_gdma_ll_lli_set_src_trans_width(lli, DW_GDMA_TRANS_WIDTH_64);
  dw_gdma_ll_lli_set_dst_trans_width(lli, DW_GDMA_TRANS_WIDTH_64);
  dw_gdma_ll_lli_set_src_burst_mode(lli, DW_GDMA_BURST_MODE_FIXED);
  dw_gdma_ll_lli_set_dst_burst_mode(lli, DW_GDMA_BURST_MODE_INCREMENT);
  dw_gdma_ll_lli_set_src_burst_items(lli, DW_GDMA_BURST_ITEMS_512);
  dw_gdma_ll_lli_set_dst_burst_items(lli, DW_GDMA_BURST_ITEMS_512);
  dw_gdma_ll_lli_set_src_burst_len(lli, 16);
  dw_gdma_ll_lli_set_dst_burst_len(lli, 16);
  dw_gdma_ll_lli_set_block_markers(lli, false, true, true);
  dw_gdma_ll_lli_set_link_list_master_port(
    lli, DW_GDMA_LL_MASTER_PORT_MEMORY);
  dw_gdma_ll_lli_set_next_item_addr(lli, 0);

  ret = esp_mipi_csi_result(esp_cache_msync(
    lli, sizeof(*lli), ESP_CACHE_MSYNC_FLAG_DIR_C2M |
    ESP_CACHE_MSYNC_FLAG_UNALIGNED));
  if (ret < 0)
    {
      goto errout;
    }

  priv->dma_cpuint = esp_setup_irq(ETS_DW_GDMA_INTR_SOURCE,
                                    ESP_IRQ_PRIORITY_DEFAULT,
                                    ESP_IRQ_TRIGGER_LEVEL,
                                    esp_mipi_csi_dma_interrupt, priv);
  if (priv->dma_cpuint < 0)
    {
      ret = priv->dma_cpuint;
      priv->dma_cpuint = -1;
      goto errout;
    }

  priv->bridge_cpuint = esp_setup_irq(ETS_CSI_BRIDGE_INTR_SOURCE,
                                       ESP_IRQ_PRIORITY_DEFAULT,
                                       ESP_IRQ_TRIGGER_LEVEL,
                                       esp_mipi_csi_bridge_interrupt, priv);
  if (priv->bridge_cpuint < 0)
    {
      ret = priv->bridge_cpuint;
      priv->bridge_cpuint = -1;
      goto errout;
    }

  priv->hal.bridge_dev->int_clr.val = ESP_MIPI_CSI_BRG_ERROR_EVENTS;
  priv->hal.bridge_dev->int_ena.val = ESP_MIPI_CSI_BRG_ERROR_EVENTS;
  dw_gdma_ll_enable_intr_global(dma_dev, true);
  up_enable_irq(ESP_SOURCE2IRQ(ETS_DW_GDMA_INTR_SOURCE));
  up_enable_irq(ESP_SOURCE2IRQ(ETS_CSI_BRIDGE_INTR_SOURCE));
  return OK;

errout:
  esp_mipi_csi_release_dma(priv);
  return ret;
}

static void esp_mipi_csi_disable_hardware(FAR struct esp_mipi_csi_s *priv)
{
  if (priv->hal.bridge_dev != NULL)
    {
      priv->hal.bridge_dev->int_ena.val = 0;
      priv->hal.bridge_dev->int_clr.val = UINT32_MAX;
      mipi_csi_brg_ll_enable(priv->hal.bridge_dev, false);
    }

  PERIPH_RCC_ATOMIC()
    {
      mipi_csi_ll_enable_phy_config_clock(ESP_MIPI_CSI_BUS0, false);
      mipi_csi_ll_enable_brg_module_clock(ESP_MIPI_CSI_BUS0, false);
      mipi_csi_ll_enable_host_bus_clock(ESP_MIPI_CSI_BUS0, false);
    }

  if (priv->phy_clock_enabled)
    {
      esp_clk_tree_enable_src(
        (soc_module_clk_t)MIPI_CSI_PHY_CLK_SRC_DEFAULT, false);
      priv->phy_clock_enabled = false;
    }

  memset(&priv->hal, 0, sizeof(priv->hal));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp_mipi_csi_initialize(FAR const struct esp_mipi_csi_config_s *config,
                            FAR struct esp_mipi_csi_s **csi)
{
  FAR struct esp_mipi_csi_s *priv = &g_esp_mipi_csi;
  mipi_csi_hal_config_t hal_config;
  size_t frame_bytes;
  int ret;

  if (config == NULL || csi == NULL ||
      config->lane_num == 0 ||
      config->lane_num > ESP_MIPI_CSI_MAX_DATA_LANES ||
      config->lane_bit_rate_mbps < ESP_MIPI_CSI_MIN_RATE_MBPS ||
      config->lane_bit_rate_mbps > ESP_MIPI_CSI_MAX_RATE_MBPS ||
      !esp_mipi_csi_data_type_valid(config->data_type,
                                    config->bits_per_pixel))
    {
      return -EINVAL;
    }

  ret = esp_mipi_csi_frame_bytes(config, &frame_bytes);
  if (ret < 0 || (frame_bytes % ESP_MIPI_CSI_DMA_WIDTH_BYTES) != 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->initialized)
    {
      ret = -EBUSY;
      goto out;
    }

  ret = esp_mipi_csi_result(esp_clk_tree_enable_src(
    (soc_module_clk_t)MIPI_CSI_PHY_CLK_SRC_DEFAULT, true));
  if (ret < 0)
    {
      goto out;
    }

  priv->phy_clock_enabled = true;
  PERIPH_RCC_ATOMIC()
    {
      mipi_csi_ll_enable_host_bus_clock(ESP_MIPI_CSI_BUS0, true);
      mipi_csi_ll_reset_host_clock(ESP_MIPI_CSI_BUS0);
      mipi_csi_ll_enable_brg_module_clock(ESP_MIPI_CSI_BUS0, true);
      mipi_csi_ll_reset_brg_module_clock(ESP_MIPI_CSI_BUS0);
      mipi_csi_ll_set_phy_clock_source(
        ESP_MIPI_CSI_BUS0, MIPI_CSI_PHY_CLK_SRC_DEFAULT);
      mipi_csi_ll_enable_phy_config_clock(ESP_MIPI_CSI_BUS0, true);
    }

  memset(&hal_config, 0, sizeof(hal_config));
  hal_config.lanes_num = config->lane_num;
  hal_config.frame_width = config->width;
  hal_config.frame_height = config->height;
  hal_config.in_bpp = config->bits_per_pixel;
  hal_config.out_bpp = config->bits_per_pixel;
  hal_config.byte_swap_en = config->byte_swap;
  hal_config.lane_bit_rate_mbps = config->lane_bit_rate_mbps;
  mipi_csi_hal_init(&priv->hal, &hal_config);

  /* Keep the bridge dimensions explicit.  This also protects this NuttX
   * adapter from historical width/height naming inversions in vendor HALs.
   */

  mipi_csi_brg_ll_set_intput_data_h_pixel_num(priv->hal.bridge_dev,
                                               config->width);
  mipi_csi_brg_ll_set_intput_data_v_row_num(priv->hal.bridge_dev,
                                             config->height);
  mipi_csi_brg_ll_set_data_type_min(priv->hal.bridge_dev, config->data_type);
  mipi_csi_brg_ll_set_data_type_max(priv->hal.bridge_dev, config->data_type);
  mipi_csi_brg_ll_set_burst_len(priv->hal.bridge_dev,
                                ESP_MIPI_CSI_DMA_BURST_WORDS);
  mipi_csi_brg_ll_set_flow_ctl_buf_afull_thrd(
    priv->hal.bridge_dev, ESP_MIPI_CSI_DMA_FIFO_THRESHOLD);

  memset(&priv->stats, 0, sizeof(priv->stats));
  nxsem_init(&priv->frame_sem, 0, 0);
  priv->expected_frame_bytes = frame_bytes;
  priv->initialized = true;
  *csi = priv;
  ret = OK;

out:
  if (ret < 0 && priv->phy_clock_enabled)
    {
      esp_mipi_csi_disable_hardware(priv);
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

int esp_mipi_csi_shutdown(FAR struct esp_mipi_csi_s *csi)
{
  FAR struct esp_mipi_csi_s *priv = &g_esp_mipi_csi;
  int ret;

  if (csi != priv)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      ret = -ESHUTDOWN;
      goto out;
    }

  if (priv->running)
    {
      mipi_csi_brg_ll_enable(priv->hal.bridge_dev, false);
      priv->running = false;
    }

  esp_mipi_csi_release_dma(priv);
  esp_mipi_csi_disable_hardware(priv);
  nxsem_destroy(&priv->frame_sem);
  priv->expected_frame_bytes = 0;
  priv->initialized = false;
  ret = OK;

out:
  nxmutex_unlock(&priv->lock);
  return ret;
}

int esp_mipi_csi_start(FAR struct esp_mipi_csi_s *csi,
                       FAR void *frame_buffer, size_t frame_buffer_bytes)
{
  FAR struct esp_mipi_csi_s *priv = &g_esp_mipi_csi;
  int ret;

  if (csi != priv || frame_buffer == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      ret = -ESHUTDOWN;
    }
  else if (priv->running)
    {
      ret = -EBUSY;
    }
  else if (frame_buffer_bytes != priv->expected_frame_bytes)
    {
      ret = -EINVAL;
    }
  else
    {
      while (nxsem_trywait(&priv->frame_sem) == OK)
        {
        }

      ret = esp_mipi_csi_prepare_dma(priv, frame_buffer, frame_buffer_bytes);
      if (ret >= 0)
        {
          priv->running = true;
          mipi_csi_brg_ll_enable(priv->hal.bridge_dev, true);
        }
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

int esp_mipi_csi_wait_frame(FAR struct esp_mipi_csi_s *csi,
                            uint32_t timeout_ms)
{
  FAR struct esp_mipi_csi_s *priv = &g_esp_mipi_csi;

  if (csi != priv || timeout_ms == 0)
    {
      return -EINVAL;
    }

  if (!priv->running)
    {
      return -EPIPE;
    }

  return nxsem_tickwait_uninterruptible(&priv->frame_sem,
                                        MSEC2TICK(timeout_ms));
}

int esp_mipi_csi_stop(FAR struct esp_mipi_csi_s *csi)
{
  FAR struct esp_mipi_csi_s *priv = &g_esp_mipi_csi;
  int ret;

  if (csi != priv)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      ret = -ESHUTDOWN;
    }
  else if (!priv->running)
    {
      ret = -EPIPE;
    }
  else
    {
      mipi_csi_brg_ll_enable(priv->hal.bridge_dev, false);
      priv->running = false;
      esp_mipi_csi_release_dma(priv);
      ret = OK;
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

int esp_mipi_csi_buffer_sync_for_cpu(FAR void *buffer, size_t bytes)
{
  if (buffer == NULL || bytes == 0)
    {
      return -EINVAL;
    }

  return esp_mipi_csi_result(esp_cache_msync(
    buffer, bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C |
    ESP_CACHE_MSYNC_FLAG_UNALIGNED));
}

int esp_mipi_csi_get_stats(FAR struct esp_mipi_csi_s *csi,
                           FAR struct esp_mipi_csi_stats_s *stats)
{
  FAR struct esp_mipi_csi_s *priv = &g_esp_mipi_csi;
  irqstate_t flags;

  if (csi != priv || stats == NULL)
    {
      return -EINVAL;
    }

  flags = spin_lock_irqsave(&priv->irq_lock);
  memcpy(stats, &priv->stats, sizeof(*stats));
  spin_unlock_irqrestore(&priv->irq_lock, flags);
  return OK;
}
