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
#include <nuttx/wqueue.h>

#include <errno.h>
#include <string.h>
#include <syslog.h>

#include "esp_cache.h"
#include "esp_clk_tree.h"
#include "esp_err.h"
#include "esp_irq.h"
#include "esp_private/esp_clk_tree_common.h"
#include "esp_private/periph_ctrl.h"
#include "hal/dw_gdma_ll.h"
#include "hal/isp_ll.h"
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
#define ESP_MIPI_CSI_CACHE_LINE_BYTES            64
#define ESP_MIPI_CSI_DMA_BURST_WORDS           512
#define ESP_MIPI_CSI_DMA_FIFO_THRESHOLD        960
#define ESP_MIPI_CSI_VIDEO_BUFFER_COUNT           3
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
  FAR struct esp_ldo_channel_s *phy_ldo;
  mutex_t                     lock;
  spinlock_t                  irq_lock;
  sem_t                       frame_sem;
  dw_gdma_link_list_item_t    dma_lli;
  FAR dw_gdma_dev_t           *dma_dev;
  FAR void                    *frame_buffer;
  FAR void                    *ready_buffers[ESP_MIPI_CSI_VIDEO_BUFFER_COUNT];
  FAR void                    *done_buffers[ESP_MIPI_CSI_VIDEO_BUFFER_COUNT];
  struct work_s               frame_work;
  esp_mipi_csi_frame_callback_t frame_callback;
  FAR void                    *frame_callback_arg;
  size_t                      expected_frame_bytes;
  uint8_t                     ready_head;
  uint8_t                     ready_tail;
  uint8_t                     ready_count;
  uint8_t                     done_head;
  uint8_t                     done_tail;
  uint8_t                     done_count;
  struct esp_mipi_csi_stats_s stats;
  int                         dma_cpuint;
  int                         bridge_cpuint;
  bool                        phy_clock_enabled;
  bool                        isp_clock_enabled;
  struct esp_isp_s            isp;
  bool                        powered;
  bool                        initialized;
  bool                        running;
  bool                        video_mode;
  bool                        frame_work_pending;
  bool                        dma_paused;
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

static bool esp_mipi_csi_ready_pop_locked(FAR struct esp_mipi_csi_s *priv,
                                          FAR void **buffer)
{
  if (priv->ready_count == 0)
    {
      return false;
    }

  *buffer = priv->ready_buffers[priv->ready_head];
  priv->ready_head = (priv->ready_head + 1) % ESP_MIPI_CSI_VIDEO_BUFFER_COUNT;
  priv->ready_count--;
  return true;
}

static bool esp_mipi_csi_done_push_locked(FAR struct esp_mipi_csi_s *priv,
                                          FAR void *buffer)
{
  if (priv->done_count == ESP_MIPI_CSI_VIDEO_BUFFER_COUNT)
    {
      return false;
    }

  priv->done_buffers[priv->done_tail] = buffer;
  priv->done_tail = (priv->done_tail + 1) % ESP_MIPI_CSI_VIDEO_BUFFER_COUNT;
  priv->done_count++;
  return true;
}

static bool esp_mipi_csi_done_pop_locked(FAR struct esp_mipi_csi_s *priv,
                                         FAR void **buffer)
{
  if (priv->done_count == 0)
    {
      return false;
    }

  *buffer = priv->done_buffers[priv->done_head];
  priv->done_head = (priv->done_head + 1) % ESP_MIPI_CSI_VIDEO_BUFFER_COUNT;
  priv->done_count--;
  return true;
}

static void esp_mipi_csi_rearm_dma(FAR struct esp_mipi_csi_s *priv,
                                   FAR void *buffer)
{
  dw_gdma_ll_lli_set_dst_addr(&priv->dma_lli, (uint32_t)(uintptr_t)buffer);
  dw_gdma_ll_lli_set_dst_master_port(&priv->dma_lli, (intptr_t)buffer);
  dw_gdma_ll_lli_set_block_markers(&priv->dma_lli, false, true, true);
  esp_cache_msync(&priv->dma_lli, sizeof(priv->dma_lli),
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                  ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  dw_gdma_ll_channel_set_link_list_head_addr(
    priv->dma_dev, ESP_MIPI_CSI_DMA_CHANNEL,
    (uint32_t)(uintptr_t)&priv->dma_lli);
  dw_gdma_ll_channel_enable(priv->dma_dev, ESP_MIPI_CSI_DMA_CHANNEL, true);
}

static void esp_mipi_csi_frame_worker(FAR void *arg)
{
  FAR struct esp_mipi_csi_s *priv = arg;
  esp_mipi_csi_frame_callback_t callback;
  FAR void *callback_arg;
  FAR void *buffer;
  irqstate_t flags;
  int ret;

  for (;;)
    {
      flags = spin_lock_irqsave(&priv->irq_lock);
      if (!esp_mipi_csi_done_pop_locked(priv, &buffer))
        {
          priv->frame_work_pending = false;
          spin_unlock_irqrestore(&priv->irq_lock, flags);
          return;
        }

      callback = priv->frame_callback;
      callback_arg = priv->frame_callback_arg;
      spin_unlock_irqrestore(&priv->irq_lock, flags);

      if (callback != NULL)
        {
          ret = esp_mipi_csi_buffer_sync_for_cpu(buffer,
                                                  priv->expected_frame_bytes);
          if (ret >= 0)
            {
              callback(buffer, priv->expected_frame_bytes, callback_arg);
            }
          else
            {
              esp_mipi_csi_queue_buffer(priv, buffer,
                                        priv->expected_frame_bytes);
            }
        }
    }
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
         (config->output == ESP_ISP_OUTPUT_RGB565 ? 16 :
          config->bits_per_pixel);
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

static void esp_mipi_csi_count_bridge_status(FAR struct esp_mipi_csi_s *priv,
                                              uint32_t status)
{
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
}

static uint32_t esp_mipi_csi_get_bridge_errors(
  FAR struct esp_mipi_csi_s *priv)
{
  uint32_t status;

  if (priv == NULL || priv->hal.bridge_dev == NULL)
    {
      return 0;
    }

  /* int_st is masked by int_ena.  The temporary single-IRQ path keeps the
   * bridge interrupt disabled, so sample the sticky raw status instead.
   */

  status = priv->hal.bridge_dev->int_raw.val & ESP_MIPI_CSI_BRG_ERROR_EVENTS;
  if (status != 0)
    {
      priv->hal.bridge_dev->int_clr.val = status;
    }

  return status;
}

static void esp_mipi_csi_sample_bridge_errors(
  FAR struct esp_mipi_csi_s *priv)
{
  uint32_t status;
  irqstate_t flags;

  flags = spin_lock_irqsave(&priv->irq_lock);
  status = esp_mipi_csi_get_bridge_errors(priv);
  if (status != 0)
    {
      esp_mipi_csi_count_bridge_status(priv, status);
    }

  spin_unlock_irqrestore(&priv->irq_lock, flags);
}

/* Capture the receiver state at a diagnostic boundary.  The Host has no
 * int_raw register: its event status registers are read-clear, so do not use
 * this helper from a polling loop.  phy_rx and phy_stopstate are read-only
 * live lane state.  The Bridge raw status is deliberately not cleared here;
 * esp_mipi_csi_sample_bridge_errors() retains ownership of clearing its
 * sticky error events.
 */

static void esp_mipi_csi_sample_receive_state(
  FAR struct esp_mipi_csi_s *priv)
{
  uint32_t host_status = 0;
  uint32_t host_phy_fatal = 0;
  uint32_t host_packet_fatal = 0;
  uint32_t host_phy = 0;
  uint32_t phy_rx = 0;
  uint32_t phy_stopstate = 0;
  uint32_t bridge_raw = 0;
  uint32_t bridge_enable = 0;
  uint32_t bridge_buffer = 0;
  uint32_t dma_status = 0;
  uint32_t dma_transfer_units = 0;
  uint32_t dma_fifo_units = 0;
  uint32_t dma_source_status = 0;
  irqstate_t flags;

  if (priv == NULL)
    {
      return;
    }

  if (priv->hal.host_dev != NULL)
    {
      host_status = priv->hal.host_dev->int_st_main.val;
      host_phy_fatal = priv->hal.host_dev->int_st_phy_fatal.val;
      host_packet_fatal = priv->hal.host_dev->int_st_pkt_fatal.val;
      host_phy = priv->hal.host_dev->int_st_phy.val;
      phy_rx = priv->hal.host_dev->phy_rx.val;
      phy_stopstate = priv->hal.host_dev->phy_stopstate.val;
    }

  if (priv->hal.bridge_dev != NULL)
    {
      bridge_raw = priv->hal.bridge_dev->int_raw.val;
      bridge_enable = priv->hal.bridge_dev->csi_en.val;
      bridge_buffer = priv->hal.bridge_dev->buf_flow_ctl.val;
    }

  if (priv->dma_dev != NULL)
    {
      dma_status = dw_gdma_ll_channel_get_intr_status(
        priv->dma_dev, ESP_MIPI_CSI_DMA_CHANNEL);
      dma_transfer_units = dw_gdma_ll_channel_get_trans_amount(
        priv->dma_dev, ESP_MIPI_CSI_DMA_CHANNEL);
      dma_fifo_units = dw_gdma_ll_channel_get_fifo_remain(
        priv->dma_dev, ESP_MIPI_CSI_DMA_CHANNEL);
      dma_source_status = dw_gdma_ll_channel_get_src_periph_status(
        priv->dma_dev, ESP_MIPI_CSI_DMA_CHANNEL);
    }

  flags = spin_lock_irqsave(&priv->irq_lock);
  esp_mipi_csi_count_host_status(priv, host_status);
  priv->stats.last_host_phy_fatal_status = host_phy_fatal;
  priv->stats.last_host_packet_fatal_status = host_packet_fatal;
  priv->stats.last_host_phy_status = host_phy;
  priv->stats.last_phy_rx_status = phy_rx;
  priv->stats.last_phy_stopstate_status = phy_stopstate;
  priv->stats.last_bridge_raw_status = bridge_raw;
  priv->stats.last_bridge_enable_status = bridge_enable;
  priv->stats.last_bridge_buffer_status = bridge_buffer;
  priv->stats.last_dma_channel_status = dma_status;
  priv->stats.last_dma_transfer_units = dma_transfer_units;
  priv->stats.last_dma_fifo_units = dma_fifo_units;
  priv->stats.last_dma_source_status = dma_source_status;
  spin_unlock_irqrestore(&priv->irq_lock, flags);
}

static int esp_mipi_csi_dma_interrupt(int irq, FAR void *context,
                                      FAR void *arg)
{
  FAR struct esp_mipi_csi_s *priv = arg;
  uint32_t dma_status;
  uint32_t host_status;
  uint32_t bridge_status;
  irqstate_t flags;
  FAR void *next_buffer = NULL;
  bool schedule_work = false;
  bool rearm_video_dma = false;
  bool video_mode = false;

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
  host_status = 0;
  bridge_status = 0;
  if ((dma_status & ESP_MIPI_CSI_DMA_DONE_EVENTS) != 0)
    {
      flags = spin_lock_irqsave(&priv->irq_lock);
      video_mode = priv->video_mode;
      if (video_mode)
        {
          if (esp_mipi_csi_done_push_locked(priv, priv->frame_buffer) &&
              esp_mipi_csi_ready_pop_locked(priv, &next_buffer))
            {
              priv->frame_buffer = next_buffer;
              rearm_video_dma = true;
            }
          else
            {
              /* Preserve completed buffers.  Reception resumes when the
               * worker returns one through esp_mipi_csi_queue_buffer().
               */

              priv->frame_buffer = NULL;
              priv->dma_paused = true;
            }

          if (!priv->frame_work_pending)
            {
              priv->frame_work_pending = true;
              schedule_work = true;
            }
        }

      spin_unlock_irqrestore(&priv->irq_lock, flags);

      if (video_mode && !rearm_video_dma)
        {
          mipi_csi_brg_ll_enable(priv->hal.bridge_dev, false);
          dw_gdma_ll_channel_enable(priv->dma_dev, ESP_MIPI_CSI_DMA_CHANNEL,
                                    false);
        }

      host_status = priv->hal.host_dev->int_st_main.val;
    }

  flags = spin_lock_irqsave(&priv->irq_lock);
  priv->stats.last_dma_status = dma_status;

  if ((dma_status & ESP_MIPI_CSI_DMA_ERROR_EVENTS) != 0)
    {
      priv->stats.dma_error_count++;
      spin_unlock_irqrestore(&priv->irq_lock, flags);
      return OK;
    }

  if ((dma_status & ESP_MIPI_CSI_DMA_DONE_EVENTS) != 0)
    {
      esp_mipi_csi_count_host_status(priv, host_status);
      bridge_status = esp_mipi_csi_get_bridge_errors(priv);
      if (bridge_status != 0)
        {
          esp_mipi_csi_count_bridge_status(priv, bridge_status);
        }

      priv->stats.frame_count++;
    }

  spin_unlock_irqrestore(&priv->irq_lock, flags);

  if ((dma_status & ESP_MIPI_CSI_DMA_DONE_EVENTS) != 0 && video_mode &&
      rearm_video_dma)
    {
      esp_mipi_csi_rearm_dma(priv, next_buffer);
    }
  else if ((dma_status & ESP_MIPI_CSI_DMA_DONE_EVENTS) != 0 && !video_mode)
    {
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
      nxsem_post(&priv->frame_sem);
    }

  if (schedule_work)
    {
      work_queue(HPWORK, &priv->frame_work, esp_mipi_csi_frame_worker,
                 priv, 0);
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
  irqstate_t flags;

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
  priv->video_mode = false;
  priv->dma_paused = false;

  flags = spin_lock_irqsave(&priv->irq_lock);
  memset(priv->ready_buffers, 0, sizeof(priv->ready_buffers));
  memset(priv->done_buffers, 0, sizeof(priv->done_buffers));
  priv->ready_head = 0;
  priv->ready_tail = 0;
  priv->ready_count = 0;
  priv->done_head = 0;
  priv->done_tail = 0;
  priv->done_count = 0;
  priv->frame_callback = NULL;
  priv->frame_callback_arg = NULL;
  spin_unlock_irqrestore(&priv->irq_lock, flags);
}

static int esp_mipi_csi_prepare_dma(FAR struct esp_mipi_csi_s *priv,
                                    FAR void *buffer, size_t bytes)
{
  FAR dw_gdma_dev_t *dma_dev;
  FAR dw_gdma_link_list_item_t *lli;
  FAR const char *stage;
  int ret;

  stage = "validate";
  syslog(LOG_INFO,
         "INFO: MIPI-CSI DMA prepare: stage=begin bytes=%zu alignment=%lu\n",
         bytes, (unsigned long)((uintptr_t)buffer &
                                 (ESP_MIPI_CSI_DMA_WIDTH_BYTES - 1)));
  if (((uintptr_t)buffer & (ESP_MIPI_CSI_DMA_WIDTH_BYTES - 1)) != 0 ||
      (bytes % ESP_MIPI_CSI_DMA_WIDTH_BYTES) != 0)
    {
      return -EINVAL;
    }

  stage = "dma_clock_reset";
  syslog(LOG_INFO, "INFO: MIPI-CSI DMA prepare: stage=%s\n", stage);
  PERIPH_RCC_ATOMIC()
    {
      dw_gdma_ll_enable_bus_clock(ESP_MIPI_CSI_BUS0, true);
      dw_gdma_ll_reset_register(ESP_MIPI_CSI_BUS0);
    }

  syslog(LOG_INFO,
         "INFO: MIPI-CSI DMA prepare: stage=%s result=%d\n", stage, OK);

  dma_dev = DW_GDMA_LL_GET_HW(ESP_MIPI_CSI_BUS0);
  if (dma_dev == NULL)
    {
      return -ENODEV;
    }

  priv->dma_dev = dma_dev;
  priv->frame_buffer = buffer;
  lli = &priv->dma_lli;
  memset(lli, 0, sizeof(*lli));

  stage = "dma_channel_configure";
  syslog(LOG_INFO, "INFO: MIPI-CSI DMA prepare: stage=%s\n", stage);
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
  syslog(LOG_INFO,
         "INFO: MIPI-CSI DMA prepare: stage=%s result=%d\n", stage, OK);

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

  stage = "dma_lli_cache_sync";
  syslog(LOG_INFO, "INFO: MIPI-CSI DMA prepare: stage=%s\n", stage);
  ret = esp_mipi_csi_result(esp_cache_msync(
    lli, sizeof(*lli), ESP_CACHE_MSYNC_FLAG_DIR_C2M |
    ESP_CACHE_MSYNC_FLAG_UNALIGNED));
  if (ret < 0)
    {
      goto errout;
    }

  syslog(LOG_INFO,
         "INFO: MIPI-CSI DMA prepare: stage=%s result=%d\n", stage, OK);

  stage = "dma_irq_setup";
  syslog(LOG_INFO, "INFO: MIPI-CSI DMA prepare: stage=%s\n", stage);
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

  syslog(LOG_INFO,
         "INFO: MIPI-CSI DMA prepare: stage=%s result=%d cpuint=%d\n",
         stage, OK, priv->dma_cpuint);

  /* Current ESP32-P4 dynamic IRQ allocation does not return from its second
   * allocation, regardless of whether CSI Bridge or GDMA is registered
   * first.  Keep the Bridge source disabled until that common allocator
   * problem is fixed.  Its sticky raw error status is sampled at DMA
   * completion, frame timeout and statistics readout.
   */

  stage = "bridge_error_polling";
  syslog(LOG_INFO, "INFO: MIPI-CSI DMA prepare: stage=%s\n", stage);
  priv->hal.bridge_dev->int_clr.val = ESP_MIPI_CSI_BRG_ERROR_EVENTS;
  priv->hal.bridge_dev->int_ena.val = 0;
  syslog(LOG_INFO,
         "INFO: MIPI-CSI DMA prepare: stage=%s result=%d\n", stage, OK);

  stage = "irq_enable";
  syslog(LOG_INFO, "INFO: MIPI-CSI DMA prepare: stage=%s\n", stage);
  dw_gdma_ll_enable_intr_global(dma_dev, true);
  up_enable_irq(ESP_SOURCE2IRQ(ETS_DW_GDMA_INTR_SOURCE));
  syslog(LOG_INFO,
         "INFO: MIPI-CSI DMA prepare: stage=complete result=%d\n", OK);
  return OK;

errout:
  syslog(LOG_ERR,
         "ERROR: MIPI-CSI DMA prepare: stage=%s result=%d\n", stage, ret);
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

  /* Access ISP registers before removing either of its clocks. */

  if (priv->isp_clock_enabled)
    {
      esp_isp_deinitialize(&priv->isp);
      esp_clk_tree_enable_src((soc_module_clk_t)ISP_CLK_SRC_PLL160, false);
      priv->isp_clock_enabled = false;
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

int esp_mipi_csi_power_acquire(
  FAR const struct esp_mipi_csi_config_s *config,
  FAR struct esp_mipi_csi_s **csi)
{
  FAR struct esp_mipi_csi_s *priv = &g_esp_mipi_csi;
  bool ldo_acquired = false;
  int ret;

  if (config == NULL || csi == NULL ||
      config->phy_ldo.voltage_mv != ESP_MIPI_CSI_DPHY_VOLTAGE_MV)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->powered)
    {
      ret = -EBUSY;
      goto out;
    }

  ret = esp_ldo_acquire(&config->phy_ldo, &priv->phy_ldo);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: MIPI-CSI D-PHY LDO acquire failed channel=%d "
             "voltage_mv=%d ret=%d\n",
             config->phy_ldo.channel_id, config->phy_ldo.voltage_mv, ret);
      goto out;
    }

  ldo_acquired = true;

  syslog(LOG_INFO,
         "INFO: MIPI-CSI D-PHY LDO ready channel=%d voltage_mv=%d\n",
         config->phy_ldo.channel_id, config->phy_ldo.voltage_mv);

  priv->powered = true;
  *csi = priv;
  ret = OK;

out:
  if (ret < 0 && ldo_acquired)
    {
      esp_ldo_release(priv->phy_ldo);
      priv->phy_ldo = NULL;
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

int esp_mipi_csi_initialize(FAR struct esp_mipi_csi_s *csi,
                            FAR const struct esp_mipi_csi_config_s *config)
{
  FAR struct esp_mipi_csi_s *priv = &g_esp_mipi_csi;
  mipi_csi_hal_config_t hal_config;
  FAR isp_dev_t *isp = ISP_LL_GET_HW(0);
  FAR const char *stage;
  size_t frame_bytes;
  uint32_t line_bits;
  int ret;

  if (csi != priv || config == NULL || config->lane_num == 0 ||
      config->lane_num > ESP_MIPI_CSI_MAX_DATA_LANES ||
      config->lane_bit_rate_mbps < ESP_MIPI_CSI_MIN_RATE_MBPS ||
      config->lane_bit_rate_mbps > ESP_MIPI_CSI_MAX_RATE_MBPS ||
      !esp_mipi_csi_data_type_valid(config->data_type,
                                    config->bits_per_pixel))
    {
      return -EINVAL;
    }

  ret = esp_mipi_csi_frame_bytes(config, &frame_bytes);
  line_bits = (uint32_t)config->width * config->bits_per_pixel;
  if (ret < 0 || (line_bits % 64) != 0 ||
      line_bits / 32 > 0x1000 || config->height > 0xfff)
    {
      return -EINVAL;
    }

  stage = "lock";
  syslog(LOG_INFO,
         "INFO: MIPI-CSI initialize: stage=begin lanes=%u dt=0x%02x "
         "raw_bpp=%u width=%u height=%u lane_rate_mbps=%lu\n",
         config->lane_num, config->data_type, config->bits_per_pixel,
         config->width, config->height,
         (unsigned long)config->lane_bit_rate_mbps);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: MIPI-CSI initialize: stage=%s result=%d\n", stage, ret);
      return ret;
    }

  syslog(LOG_INFO, "INFO: MIPI-CSI initialize: stage=lock result=%d\n",
         OK);

  stage = "power_check";
  if (!priv->powered || priv->phy_ldo == NULL)
    {
      ret = -EPIPE;
      goto out;
    }

  if (priv->initialized)
    {
      ret = -EBUSY;
      goto out;
    }

  stage = "phy_clock_enable";
  syslog(LOG_INFO,
         "INFO: MIPI-CSI initialize: stage=%s source=%d\n", stage,
         (int)MIPI_CSI_PHY_CLK_SRC_DEFAULT);
  ret = esp_mipi_csi_result(esp_clk_tree_enable_src(
    (soc_module_clk_t)MIPI_CSI_PHY_CLK_SRC_DEFAULT, true));
  if (ret < 0)
    {
      goto out;
    }

  priv->phy_clock_enabled = true;
  stage = "host_bridge_clock_reset";
  syslog(LOG_INFO, "INFO: MIPI-CSI initialize: stage=%s\n", stage);
  PERIPH_RCC_ATOMIC()
    {
      mipi_csi_ll_enable_host_bus_clock(ESP_MIPI_CSI_BUS0, true);
      mipi_csi_ll_reset_host_clock(ESP_MIPI_CSI_BUS0);
      mipi_csi_ll_enable_brg_module_clock(ESP_MIPI_CSI_BUS0, true);
      mipi_csi_ll_reset_brg_module_clock(ESP_MIPI_CSI_BUS0);
      mipi_csi_brg_ll_enable_clock(MIPI_CSI_BRG_LL_GET_HW(0), true);
      mipi_csi_ll_set_phy_clock_source(
        ESP_MIPI_CSI_BUS0, MIPI_CSI_PHY_CLK_SRC_DEFAULT);
      mipi_csi_ll_enable_phy_config_clock(ESP_MIPI_CSI_BUS0, true);
    }

  syslog(LOG_INFO,
         "INFO: MIPI-CSI initialize: stage=%s result=%d\n", stage, OK);

  memset(&hal_config, 0, sizeof(hal_config));
  hal_config.lanes_num = config->lane_num;
  hal_config.frame_width = config->width;
  hal_config.frame_height = config->height;
  hal_config.in_bpp = config->bits_per_pixel;
  hal_config.out_bpp = config->bits_per_pixel;
  hal_config.byte_swap_en = config->byte_swap;
  hal_config.lane_bit_rate_mbps = config->lane_bit_rate_mbps;
  stage = "hal_initialize";
  syslog(LOG_INFO,
         "INFO: MIPI-CSI initialize: stage=%s lanes=%u width=%lu height=%lu "
         "bpp=%lu lane_rate_mbps=%lu\n",
         stage, (unsigned int)hal_config.lanes_num,
         (unsigned long)hal_config.frame_width,
         (unsigned long)hal_config.frame_height,
         (unsigned long)hal_config.in_bpp,
         (unsigned long)hal_config.lane_bit_rate_mbps);
  mipi_csi_hal_init(&priv->hal, &hal_config);
  syslog(LOG_INFO,
         "INFO: MIPI-CSI initialize: stage=%s result=%d\n", stage, OK);

  stage = "isp_configure";
  {
    struct esp_isp_config_s isp_config =
    {
      .width = config->width,
      .height = config->height,
      .input_bpp = config->bits_per_pixel,
      .bayer_order = config->bayer_order,
      .byte_swap = config->byte_swap,
      .output = config->output
    };

    ret = esp_isp_initialize(&priv->isp, &isp_config);
  }
  if (ret < 0)
    {
      goto out;
    }

  priv->isp_clock_enabled = true;
  syslog(LOG_INFO,
         "INFO: MIPI-CSI initialize: stage=%s result=%d "
         "isp_en=%u mipi_data_en=%u input_words32=%lu cntl=0x%08lx\n",
         stage, OK, (unsigned int)isp->cntl.isp_en,
         (unsigned int)isp->cntl.mipi_data_en,
         (unsigned long)(config->output == ESP_ISP_OUTPUT_RGB565 ?
                         config->width : line_bits / 32),
         (unsigned long)isp->cntl.val);

  /* The Bridge width counts 64-bit words, not sensor pixels.  The ISP
   * bypass width above counts 32-bit words.  Neither is the pixel width.
   */

  stage = "bridge_configure";
  syslog(LOG_INFO,
         "INFO: MIPI-CSI initialize: stage=%s width=%u height=%u "
         "dt=0x%02x burst_words=%u fifo_threshold=%u words64_per_line=%lu\n",
         stage, config->width, config->height, config->data_type,
         ESP_MIPI_CSI_DMA_BURST_WORDS, ESP_MIPI_CSI_DMA_FIFO_THRESHOLD,
         (unsigned long)(frame_bytes / config->height / 8));
  mipi_csi_brg_ll_set_intput_data_h_pixel_num(priv->hal.bridge_dev,
    frame_bytes / config->height / 8);
  mipi_csi_brg_ll_set_intput_data_v_row_num(priv->hal.bridge_dev,
                                             config->height);
  mipi_csi_brg_ll_enable_has_hsync(priv->hal.bridge_dev, false);
  mipi_csi_brg_ll_enable_color_conversion(priv->hal.bridge_dev, true);
  mipi_csi_brg_ll_set_color_mode_bypass(priv->hal.bridge_dev,
    config->output != ESP_ISP_OUTPUT_RGB565);
  mipi_csi_brg_ll_set_data_type_min(priv->hal.bridge_dev, config->data_type);
  mipi_csi_brg_ll_set_data_type_max(priv->hal.bridge_dev, config->data_type);
  mipi_csi_brg_ll_set_burst_len(priv->hal.bridge_dev,
                                ESP_MIPI_CSI_DMA_BURST_WORDS);
  mipi_csi_brg_ll_set_flow_ctl_buf_afull_thrd(
    priv->hal.bridge_dev, ESP_MIPI_CSI_DMA_FIFO_THRESHOLD);
  syslog(LOG_INFO,
         "INFO: MIPI-CSI initialize: stage=%s result=%d\n", stage, OK);

  memset(&priv->stats, 0, sizeof(priv->stats));
  nxsem_init(&priv->frame_sem, 0, 0);
  priv->expected_frame_bytes = frame_bytes;
  priv->initialized = true;
  ret = OK;
  syslog(LOG_INFO,
         "INFO: MIPI-CSI initialize: stage=complete frame_bytes=%zu\n",
         frame_bytes);

out:
  if (ret < 0 && priv->phy_clock_enabled)
    {
      esp_mipi_csi_disable_hardware(priv);
    }

  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: MIPI-CSI initialize: stage=%s result=%d\n", stage, ret);
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

int esp_mipi_csi_deinitialize(FAR struct esp_mipi_csi_s *csi)
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

  if (!priv->powered)
    {
      ret = -ESHUTDOWN;
      goto out;
    }

  if (priv->initialized && priv->running)
    {
      mipi_csi_brg_ll_enable(priv->hal.bridge_dev, false);
      priv->running = false;
    }

  if (priv->initialized)
    {
      esp_mipi_csi_release_dma(priv);
      esp_mipi_csi_disable_hardware(priv);
      nxsem_destroy(&priv->frame_sem);
      priv->expected_frame_bytes = 0;
      priv->initialized = false;
    }

  ret = OK;

out:
  nxmutex_unlock(&priv->lock);
  return ret;
}

int esp_mipi_csi_power_release(FAR struct esp_mipi_csi_s *csi)
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

  if (!priv->powered || priv->phy_ldo == NULL)
    {
      ret = -ESHUTDOWN;
    }
  else if (priv->initialized)
    {
      ret = -EBUSY;
    }
  else
    {
      ret = esp_ldo_release(priv->phy_ldo);
      if (ret >= 0)
        {
          priv->phy_ldo = NULL;
          priv->powered = false;
        }
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

int esp_mipi_csi_start(FAR struct esp_mipi_csi_s *csi,
                       FAR void *frame_buffer, size_t frame_buffer_bytes)
{
  FAR struct esp_mipi_csi_s *priv = &g_esp_mipi_csi;
  FAR const char *stage;
  uint32_t chen;
  int ret;

  if (csi != priv || frame_buffer == NULL)
    {
      return -EINVAL;
    }

  stage = "lock";
  syslog(LOG_INFO,
         "INFO: MIPI-CSI start: stage=begin frame_bytes=%zu\n",
         frame_buffer_bytes);
  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: MIPI-CSI start: stage=%s result=%d\n", stage, ret);
      return ret;
    }

  syslog(LOG_INFO, "INFO: MIPI-CSI start: stage=lock result=%d\n", OK);

  stage = "state_check";
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

      /* A caller normally clears or initializes its capture buffer before
       * starting a test.  Flush those dirty cache lines before DMA owns the
       * buffer, otherwise a later cache eviction can overwrite received
       * data.
       */

      stage = "frame_buffer_cache_sync";
      syslog(LOG_INFO, "INFO: MIPI-CSI start: stage=%s\n", stage);
      ret = esp_mipi_csi_result(esp_cache_msync(
        frame_buffer, frame_buffer_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M |
        ESP_CACHE_MSYNC_FLAG_UNALIGNED));
      if (ret >= 0)
        {
          syslog(LOG_INFO,
                 "INFO: MIPI-CSI start: stage=%s result=%d\n", stage, OK);
          stage = "dma_prepare";
          syslog(LOG_INFO, "INFO: MIPI-CSI start: stage=%s\n", stage);
          ret = esp_mipi_csi_prepare_dma(priv, frame_buffer,
                                         frame_buffer_bytes);
        }

      if (ret >= 0)
        {
          /* Arm the first transfer after the LLI and IRQ are ready.  The
           * completion ISR only rearms subsequent frames.  Mark the session
           * running first so an immediate DMA error can reach the ISR.
           */

          stage = "dma_channel_enable";
          ISP.int_clr.val = ISP_LL_EVENT_HEADER_IDI_FRAME |
                            ISP_LL_EVENT_TAIL_IDI_FRAME;
          syslog(LOG_INFO, "INFO: MIPI-CSI start: stage=%s channel=%u\n",
                 stage, ESP_MIPI_CSI_DMA_CHANNEL);
          priv->running = true;
          dw_gdma_ll_channel_enable(priv->dma_dev,
                                       ESP_MIPI_CSI_DMA_CHANNEL, true);
          chen = priv->dma_dev->chen0.val;
          syslog(LOG_INFO,
                 "INFO: MIPI-CSI start: stage=%s channel=%u "
                 "chen=0x%08lx enabled=%u\n",
                 stage, ESP_MIPI_CSI_DMA_CHANNEL, (unsigned long)chen,
                 (unsigned int)((chen >> ESP_MIPI_CSI_DMA_CHANNEL) & 1u));

          stage = "bridge_enable";
          syslog(LOG_INFO, "INFO: MIPI-CSI start: stage=%s\n", stage);
          mipi_csi_brg_ll_enable(priv->hal.bridge_dev, true);
          syslog(LOG_INFO,
                 "INFO: MIPI-CSI start: stage=complete result=%d\n", OK);
        }
    }

  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: MIPI-CSI start: stage=%s result=%d\n", stage, ret);
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

int esp_mipi_csi_wait_frame(FAR struct esp_mipi_csi_s *csi,
                            uint32_t timeout_ms)
{
  FAR struct esp_mipi_csi_s *priv = &g_esp_mipi_csi;
  int ret;

  if (csi != priv || timeout_ms == 0)
    {
      return -EINVAL;
    }

  if (!priv->running)
    {
      return -EPIPE;
    }

  ret = nxsem_tickwait_uninterruptible(&priv->frame_sem,
                                       MSEC2TICK(timeout_ms));
  if (ret < 0)
    {
      esp_mipi_csi_sample_receive_state(priv);
      esp_mipi_csi_sample_bridge_errors(priv);
    }

  return ret;
}

int esp_mipi_csi_start_video(FAR struct esp_mipi_csi_s *csi,
                             FAR void *buffer, size_t bytes,
                             esp_mipi_csi_frame_callback_t callback,
                             FAR void *arg)
{
  FAR struct esp_mipi_csi_s *priv = &g_esp_mipi_csi;
  irqstate_t flags;
  int ret;

  if (csi != priv || callback == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  flags = spin_lock_irqsave(&priv->irq_lock);
  if (priv->running)
    {
      ret = -EBUSY;
    }
  else if (priv->ready_count < ESP_MIPI_CSI_VIDEO_BUFFER_COUNT - 1)
    {
      ret = -ENOBUFS;
    }
  else
    {
      priv->video_mode = true;
      priv->dma_paused = false;
      priv->frame_callback = callback;
      priv->frame_callback_arg = arg;
      ret = OK;
    }

  spin_unlock_irqrestore(&priv->irq_lock, flags);
  nxmutex_unlock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_mipi_csi_start(csi, buffer, bytes);
  if (ret < 0)
    {
      flags = spin_lock_irqsave(&priv->irq_lock);
      priv->video_mode = false;
      priv->frame_callback = NULL;
      priv->frame_callback_arg = NULL;
      spin_unlock_irqrestore(&priv->irq_lock, flags);
    }

  return ret;
}

int esp_mipi_csi_queue_buffer(FAR struct esp_mipi_csi_s *csi,
                              FAR void *buffer, size_t bytes)
{
  FAR struct esp_mipi_csi_s *priv = &g_esp_mipi_csi;
  irqstate_t flags;
  bool resume_dma = false;
  FAR void *next_buffer = NULL;
  int ret;

  if (csi != priv || buffer == NULL || bytes != priv->expected_frame_bytes ||
      ((uintptr_t)buffer & (ESP_MIPI_CSI_CACHE_LINE_BYTES - 1)) != 0)
    {
      return -EINVAL;
    }

  if (esp_mipi_csi_result(esp_cache_msync(buffer, bytes,
      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED)) < 0)
    {
      return -EIO;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  flags = spin_lock_irqsave(&priv->irq_lock);
  if (!priv->initialized || (priv->running && !priv->video_mode))
    {
      ret = -ESHUTDOWN;
    }
  else if (priv->ready_count == ESP_MIPI_CSI_VIDEO_BUFFER_COUNT)
    {
      ret = -EBUSY;
    }
  else
    {
      priv->ready_buffers[priv->ready_tail] = buffer;
      priv->ready_tail = (priv->ready_tail + 1) %
                         ESP_MIPI_CSI_VIDEO_BUFFER_COUNT;
      priv->ready_count++;
      if (priv->running && priv->dma_paused &&
          esp_mipi_csi_ready_pop_locked(priv, &next_buffer))
        {
          priv->frame_buffer = next_buffer;
          priv->dma_paused = false;
          resume_dma = true;
        }

      ret = OK;
    }

  spin_unlock_irqrestore(&priv->irq_lock, flags);
  if (resume_dma)
    {
      esp_mipi_csi_rearm_dma(priv, next_buffer);
      mipi_csi_brg_ll_enable(priv->hal.bridge_dev, true);
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

int esp_mipi_csi_wait_video_idle(FAR struct esp_mipi_csi_s *csi)
{
  FAR struct esp_mipi_csi_s *priv = &g_esp_mipi_csi;
  irqstate_t flags;
  int ret;

  if (csi != priv)
    {
      return -EINVAL;
    }

  ret = work_cancel_sync(HPWORK, &priv->frame_work);
  flags = spin_lock_irqsave(&priv->irq_lock);
  priv->frame_work_pending = false;
  priv->done_head = 0;
  priv->done_tail = 0;
  priv->done_count = 0;
  spin_unlock_irqrestore(&priv->irq_lock, flags);
  return ret < 0 && ret != -ENOENT ? ret : OK;
}

int esp_mipi_csi_wait_frame_diag(FAR struct esp_mipi_csi_s *csi,
                                 uint32_t timeout_ms)
{
  FAR struct esp_mipi_csi_s *priv = &g_esp_mipi_csi;
  FAR isp_dev_t *isp = ISP_LL_GET_HW(0);
  FAR dw_gdma_dev_t *dma;
  const uint32_t events = ISP_LL_EVENT_HEADER_IDI_FRAME |
                          ISP_LL_EVENT_TAIL_IDI_FRAME;
  clock_t start;
  clock_t ticks;
  uint32_t samples = 0;
  uint32_t hs_samples = 0;
  uint32_t non_stop_samples = 0;
  uint32_t isp_events = 0;
  uint32_t fifo_max = 0;
  uint32_t dst_advance_max = 0;
  uint32_t fifo;
  uint32_t dst;
  uint32_t buffer;
  int ret;

  if (csi != priv || timeout_ms == 0)
    {
      return -EINVAL;
    }

  syslog(LOG_INFO, "diag: enter\n");
  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->running || priv->dma_dev == NULL ||
      !priv->isp_clock_enabled)
    {
      ret = -EPIPE;
      goto out;
    }

  dma = priv->dma_dev;
  buffer = (uint32_t)(uintptr_t)priv->frame_buffer;

  /* Initialization owns the clock for the whole capture session.  Test its
   * gate in HP_SYS_CLKRST before reading the ISP register bank.
   */

  if (!HP_SYS_CLKRST.peri_clk_ctrl25.reg_isp_clk_en)
    {
      ret = -EPIPE;
      goto out;
    }

  syslog(LOG_INFO,
         "diag: before isp_cntl=0x%08lx isp_en=%u mipi_data_en=%u "
         "work_clk=%u reg_clk=%u raw=0x%08lx\n",
         (unsigned long)isp->cntl.val, (unsigned int)isp->cntl.isp_en,
         (unsigned int)isp->cntl.mipi_data_en,
         (unsigned int)HP_SYS_CLKRST.peri_clk_ctrl25.reg_isp_clk_en,
         (unsigned int)isp->clk_en.clk_en, (unsigned long)isp->int_raw.val);

  /* Retain events since capture start, including any frame that completed
   * before the caller entered this wait function.
   */

  syslog(LOG_INFO,
         "diag: window timeout_ms=%lu sample_tick_us=%lu "
         "isp_clock_hz=80000000 cntl=0x%08lx\n",
         (unsigned long)timeout_ms, (unsigned long)CONFIG_USEC_PER_TICK,
         (unsigned long)isp->cntl.val);

  /* Only read live PHY/FIFO/address state and sticky ISP events in this
   * loop.  Do not read-clear Host events or acknowledge DMA interrupts.
   * The tick deadline bounds the whole window, including sampling time.
   */

  ticks = MSEC2TICK(timeout_ms);
  start = clock_systime_ticks();
  for (; ; )
    {
      samples++;
      hs_samples += priv->hal.host_dev->phy_rx.phy_rxclkactivehs;
      non_stop_samples +=
        (priv->hal.host_dev->phy_stopstate.val & 0x00010003) != 0x00010003;
      isp_events |= isp->int_raw.val & events;
      fifo = priv->hal.bridge_dev->buf_flow_ctl.csi_buf_depth;
      if (fifo > fifo_max)
        {
          fifo_max = fifo;
        }

      dst = dma->ch[ESP_MIPI_CSI_DMA_CHANNEL].dar0.val;
      if (dst > buffer && dst - buffer <= priv->expected_frame_bytes &&
          dst - buffer > dst_advance_max)
        {
          dst_advance_max = dst - buffer;
        }

      ret = nxsem_trywait(&priv->frame_sem);
      if (ret == OK)
        {
          break;
        }

      if (clock_systime_ticks() - start >= ticks)
        {
          ret = -ETIMEDOUT;
          break;
        }

      ret = nxsem_tickwait_uninterruptible(&priv->frame_sem, 1);
      if (ret != -ETIMEDOUT)
        {
          isp_events |= isp->int_raw.val & events;
          break;
        }
    }

  syslog(LOG_INFO,
         "diag: result=%d samples=%lu hs_samples=%lu non_stop_samples=%lu "
         "isp_events=0x%08lx header_seen=%u tail_seen=%u "
         "bridge_fifo_max=%lu dma_dst_advance_max=%lu\n",
         ret, (unsigned long)samples, (unsigned long)hs_samples,
         (unsigned long)non_stop_samples, (unsigned long)isp_events,
         (unsigned int)((isp_events & ISP_LL_EVENT_HEADER_IDI_FRAME) != 0),
         (unsigned int)((isp_events & ISP_LL_EVENT_TAIL_IDI_FRAME) != 0),
         (unsigned long)fifo_max, (unsigned long)dst_advance_max);
  syslog(LOG_INFO,
         "diag: bridge host_ctrl=0x%08lx frame_cfg=0x%08lx "
         "data_type=0x%08lx dma_req=0x%08lx\n",
         (unsigned long)priv->hal.bridge_dev->host_ctrl.val,
         (unsigned long)priv->hal.bridge_dev->frame_cfg.val,
         (unsigned long)priv->hal.bridge_dev->data_type_cfg.val,
         (unsigned long)priv->hal.bridge_dev->dma_req_cfg.val);
  syslog(LOG_INFO,
         "diag: dma chen=0x%08lx llp=0x%08lx sar=0x%08lx dar=0x%08lx "
         "buffer=0x%08lx cfg0=0x%08lx cfg1=0x%08lx "
         "ctl0=0x%08lx ctl1=0x%08lx\n",
         (unsigned long)dma->chen0.val,
         (unsigned long)dma->ch[ESP_MIPI_CSI_DMA_CHANNEL].llp0.val,
         (unsigned long)dma->ch[ESP_MIPI_CSI_DMA_CHANNEL].sar0.val,
         (unsigned long)dma->ch[ESP_MIPI_CSI_DMA_CHANNEL].dar0.val,
         (unsigned long)buffer,
         (unsigned long)dma->ch[ESP_MIPI_CSI_DMA_CHANNEL].cfg0.val,
         (unsigned long)dma->ch[ESP_MIPI_CSI_DMA_CHANNEL].cfg1.val,
         (unsigned long)dma->ch[ESP_MIPI_CSI_DMA_CHANNEL].ctl0.val,
         (unsigned long)dma->ch[ESP_MIPI_CSI_DMA_CHANNEL].ctl1.val);

  esp_mipi_csi_sample_receive_state(priv);
  esp_mipi_csi_sample_bridge_errors(priv);

out:
  nxmutex_unlock(&priv->lock);
  return ret;
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
  if (buffer == NULL || bytes == 0 ||
      ((uintptr_t)buffer & (ESP_MIPI_CSI_CACHE_LINE_BYTES - 1)) != 0 ||
      (bytes & (ESP_MIPI_CSI_CACHE_LINE_BYTES - 1)) != 0)
    {
      return -EINVAL;
    }

  /* M2C invalidation does not allow the UNALIGNED flag.  The P4 data-cache
   * line is 64 bytes, so the buffer and range have been checked accordingly.
   */

  return esp_mipi_csi_result(esp_cache_msync(
    buffer, bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C));
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

  esp_mipi_csi_sample_bridge_errors(priv);
  flags = spin_lock_irqsave(&priv->irq_lock);
  memcpy(stats, &priv->stats, sizeof(*stats));
  spin_unlock_irqrestore(&priv->irq_lock, flags);
  return OK;
}
