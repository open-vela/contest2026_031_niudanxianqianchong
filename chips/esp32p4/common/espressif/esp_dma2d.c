/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_dma2d.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <errno.h>
#include <sys/param.h>
#include <limits.h>
#include <string.h>
#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/kmalloc.h>
#include <arch/barriers.h>

#include "esp_graphics_priv.h"
#include "esp_graphics_context.h"
#include "esp_irq.h"
#include "esp_cache.h"
#include "esp_intr_alloc.h"
#include "esp_memory_utils.h"
#include "esp_private/periph_ctrl.h"
#include "soc/dma2d_channel.h"
#include "soc/interrupts.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DMA2D_RX_ERRORS (DMA2D_LL_RX_EVENT_MASK & \
                        ~(DMA2D_LL_EVENT_RX_DONE | \
                          DMA2D_LL_EVENT_RX_SUC_EOF))
#define DMA2D_TX_ERRORS (DMA2D_LL_TX_EVENT_MASK & \
                        ~(DMA2D_LL_EVENT_TX_DONE | \
                          DMA2D_LL_EVENT_TX_EOF | \
                          DMA2D_LL_EVENT_TX_TOTAL_EOF))
#define DESC_BYTES ((2 * ESP_GRAPHICS_DESCRIPTORS + 1) * \
                    sizeof(struct esp_graphics_desc_s))

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Single hardware owner, shared with PPA.  Published only after IRQ setup. */

static FAR struct esp_graphics_s *g_graphics;
static mutex_t g_initialize_lock = NXMUTEX_INITIALIZER;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp_graphics_check_buffer(FAR const struct esp_graphics_buffer_s *buffer,
                              uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height)
{
  uintptr_t address = (uintptr_t)buffer->data;
  uintptr_t last;
  int ret = esp_graphics_buffer_bounds(buffer, x, y, width, height);

  if (ret < 0)
    {
      return ret;
    }

  last = address + buffer->size - 1;
  if (!((esp_ptr_dma_capable((void *)address) &&
         esp_ptr_dma_capable((void *)last)) ||
        (esp_ptr_external_ram((void *)address) &&
         esp_ptr_external_ram((void *)last))))
    {
      return -EFAULT;
    }

  return OK;
}

static bool esp_graphics_overlap(FAR const struct esp_graphics_buffer_s *a,
                                 FAR const struct esp_graphics_buffer_s *b)
{
  uintptr_t start_a = (uintptr_t)a->data;
  uintptr_t start_b = (uintptr_t)b->data;

  return start_a < start_b + b->size && start_b < start_a + a->size;
}

void esp_graphics_descriptor(FAR dma2d_descriptor_t *desc,
                             FAR const struct esp_graphics_buffer_s *buffer,
                             uint32_t x, uint32_t y,
                             uint32_t width, uint32_t height)
{
  unsigned int bpp = esp_graphics_bpp(buffer->format);

  memset(desc, 0, sizeof(*desc));
  desc->owner = DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA;
  desc->suc_eof = 1;
  desc->dma2d_en = 1;
  desc->hb_length = width;
  desc->vb_size = height;
  desc->ha_length = buffer->stride / bpp;
  desc->va_size = buffer->height;
  desc->pbyte = bpp + 1;
  desc->x = x;
  desc->y = y;
  desc->buffer = buffer->data;
}

static void esp_graphics_reset(void)
{
  unsigned int channel;

  /* This port exclusively owns DMA2D.  A peripheral reset also quiesces
   * stalled AXI requests before buffers can be returned to the caller.
   */

#ifdef CONFIG_ESP32P4_PPA
  esp_ppa_reset();
#endif
  PERIPH_RCC_ATOMIC()
    {
      dma2d_ll_reset_register(0);
    }

  dma2d_ll_hw_enable(DMA2D_LL_GET_HW(0), true);
  for (channel = 0; channel < 2; channel++)
    {
      dma2d_ll_tx_enable_interrupt(DMA2D_LL_GET_HW(0), channel,
                                   DMA2D_LL_TX_EVENT_MASK, false);
      dma2d_ll_tx_clear_interrupt_status(DMA2D_LL_GET_HW(0), channel,
                                         DMA2D_LL_TX_EVENT_MASK);
    }

  dma2d_ll_rx_enable_interrupt(DMA2D_LL_GET_HW(0), 0,
                               DMA2D_LL_RX_EVENT_MASK, false);
  dma2d_ll_rx_clear_interrupt_status(DMA2D_LL_GET_HW(0), 0,
                                     DMA2D_LL_RX_EVENT_MASK);
}

void esp_graphics_tx(unsigned int channel, dma2d_trigger_peripheral_t periph,
                      int select, FAR dma2d_descriptor_t *desc)
{
  FAR dma2d_dev_t *dev = DMA2D_LL_GET_HW(0);

  dma2d_ll_tx_connect_to_periph(dev, channel, periph, select);
  dma2d_ll_tx_enable_dscr_port(dev, channel,
                               periph == DMA2D_TRIG_PERIPH_PPA_SRM);
  dma2d_ll_tx_enable_owner_check(dev, channel, true);
  dma2d_ll_tx_enable_eof_mode(dev, channel, true);
  dma2d_ll_tx_enable_auto_write_back(dev, channel, false);
  dma2d_ll_tx_enable_page_bound_wrap(dev, channel, true);
  dma2d_ll_tx_enable_descriptor_burst(dev, channel, true);
  dma2d_ll_tx_set_data_burst_length(dev, channel,
                                    DMA2D_DATA_BURST_LENGTH_64);
  dma2d_ll_tx_set_macro_block_size(dev, channel,
                                   DMA2D_MACRO_BLOCK_SIZE_NONE);
  dma2d_ll_tx_configure_color_space_conv(dev, channel, DMA2D_CSC_TX_NONE);
  dma2d_ll_tx_enable_interrupt(dev, channel, DMA2D_TX_ERRORS, true);
  dma2d_ll_tx_set_desc_addr(dev, channel, (uintptr_t)desc);
}

void esp_graphics_rx(dma2d_trigger_peripheral_t periph, int select,
                      FAR dma2d_descriptor_t *desc)
{
  FAR dma2d_dev_t *dev = DMA2D_LL_GET_HW(0);

  dma2d_ll_rx_connect_to_periph(dev, 0, periph, select);
  dma2d_ll_rx_enable_dscr_port(dev, 0, periph == DMA2D_TRIG_PERIPH_PPA_SRM);
  dma2d_ll_rx_set_auto_return_owner(dev, 0,
                                    DMA2D_DESCRIPTOR_BUFFER_OWNER_CPU);
  dma2d_ll_rx_enable_reorder(dev, 0, false);
  dma2d_ll_rx_enable_owner_check(dev, 0, true);
  dma2d_ll_rx_enable_page_bound_wrap(dev, 0, true);
  dma2d_ll_rx_enable_descriptor_burst(dev, 0, true);
  dma2d_ll_rx_set_data_burst_length(dev, 0, DMA2D_DATA_BURST_LENGTH_64);
  dma2d_ll_rx_set_macro_block_size(dev, 0, DMA2D_MACRO_BLOCK_SIZE_NONE);
  dma2d_ll_rx_configure_color_space_conv(dev, 0, DMA2D_CSC_RX_NONE);
  dma2d_ll_rx_enable_interrupt(dev, 0,
                               DMA2D_RX_ERRORS | DMA2D_LL_EVENT_RX_SUC_EOF,
                               true);
  dma2d_ll_rx_set_desc_addr(dev, 0, (uintptr_t)desc);
}

static int esp_dma2d_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct esp_graphics_s *priv = arg;
  FAR dma2d_dev_t *dev = DMA2D_LL_GET_HW(0);
  uint32_t rx = dma2d_ll_rx_get_interrupt_status(dev, 0);
  uint32_t tx0 = dma2d_ll_tx_get_interrupt_status(dev, 0);
  uint32_t tx1 = dma2d_ll_tx_get_interrupt_status(dev, 1);
  irqstate_t flags;
  uint32_t ppa = 0;

#ifdef CONFIG_ESP32P4_PPA
  ppa = esp_ppa_errors();
#endif

  dma2d_ll_rx_clear_interrupt_status(dev, 0, rx);
  dma2d_ll_tx_clear_interrupt_status(dev, 0, tx0);
  dma2d_ll_tx_clear_interrupt_status(dev, 1, tx1);
  flags = spin_lock_irqsave(&priv->lock);
  if (priv->busy && !priv->completed &&
      ((rx & (DMA2D_RX_ERRORS | DMA2D_LL_EVENT_RX_SUC_EOF)) != 0 ||
       ((tx0 | tx1) & DMA2D_TX_ERRORS) != 0 || ppa != 0))
    {
      priv->result = ((rx & DMA2D_RX_ERRORS) != 0 ||
                      ((tx0 | tx1) & DMA2D_TX_ERRORS) != 0 ||
                      ppa != 0) ? -EIO : OK;
      priv->completed = true;
      nxsem_post(&priv->done);
    }

  spin_unlock_irqrestore(&priv->lock, flags);
  return OK;
}

int esp_graphics_initialize(void)
{
  static const int sources[] =
  {
    ETS_DMA2D_IN_CH0_INTR_SOURCE,
    ETS_DMA2D_OUT_CH0_INTR_SOURCE,
    ETS_DMA2D_OUT_CH1_INTR_SOURCE,
#ifdef CONFIG_ESP32P4_PPA
    ETS_PPA_INTR_SOURCE
#endif
  };

  FAR struct esp_graphics_s *priv;
  int installed = 0;
  int ret;

  ret = esp_graphics_check_context();
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&g_initialize_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_graphics != NULL)
    {
      nxmutex_unlock(&g_initialize_lock);
      return OK;
    }

  priv = kmm_zalloc(sizeof(*priv));
  if (priv == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  priv->tx = kmm_memalign(ESP_GRAPHICS_ALIGNMENT, DESC_BYTES);
  if (priv->tx == NULL || !esp_ptr_in_dram(priv->tx) ||
      !esp_ptr_in_dram((uint8_t *)priv->tx + DESC_BYTES - 1))
    {
      ret = -ENOMEM;
      goto free_priv;
    }

  priv->fg = priv->tx + ESP_GRAPHICS_DESCRIPTORS;
  priv->rx = priv->fg + 1;
  spin_lock_init(&priv->lock);
  nxmutex_init(&priv->mutex);
  nxsem_init(&priv->done, 0, 0);
  nxsem_set_protocol(&priv->done, SEM_PRIO_NONE);

  PERIPH_RCC_ATOMIC()
    {
      dma2d_ll_enable_bus_clock(0, true);
    }

  esp_graphics_reset();
  while (installed < nitems(sources))
    {
      ret = esp_setup_irq_with_flags(sources[installed],
                                     ESP_INTR_FLAG_LEVEL1,
                                     esp_dma2d_interrupt, priv);
      if (ret < 0)
        {
          goto free_irqs;
        }

      up_enable_irq(ESP_SOURCE2IRQ(sources[installed]));
      installed++;
    }

  g_graphics = priv;
  ret = OK;
  goto out;

free_irqs:
  while (installed > 0)
    {
      installed--;
      esp_teardown_irq(sources[installed], 0);
    }

  PERIPH_RCC_ATOMIC()
    {
      dma2d_ll_enable_bus_clock(0, false);
    }

#ifdef CONFIG_ESP32P4_PPA
  esp_ppa_uninitialize();
#endif

  nxsem_destroy(&priv->done);
  nxmutex_destroy(&priv->mutex);
free_priv:
  kmm_free(priv->tx);
  kmm_free(priv);
out:
  nxmutex_unlock(&g_initialize_lock);
  return ret;
}

static int esp_graphics_validate(FAR const struct esp_graphics_job_s *job)
{
  int ret;

  if (job->operation != ESP_GRAPHICS_COPY)
    {
#ifdef CONFIG_ESP32P4_PPA
      ret = esp_ppa_validate(job);
#else
      ret = -ENOTSUP;
#endif
    }
  else
    {
      if (job->src.format != ESP_GRAPHICS_RGB565 ||
          job->dst.format != ESP_GRAPHICS_RGB565 ||
          job->height > ESP_GRAPHICS_DESCRIPTORS * ESP_GRAPHICS_BAND_ROWS)
        {
          return -ENOTSUP;
        }

      ret = esp_graphics_check_buffer(&job->src, job->src_x, job->src_y,
                                      job->width, job->height);
      if (ret == OK)
        {
          ret = esp_graphics_check_buffer(&job->dst, job->dst_x, job->dst_y,
                                          job->width, job->height);
        }
    }

  if (ret < 0)
    {
      return ret;
    }

  if ((job->operation != ESP_GRAPHICS_FILL &&
       esp_graphics_overlap(&job->src, &job->dst)) ||
      (job->operation == ESP_GRAPHICS_BLEND &&
       esp_graphics_overlap(&job->foreground, &job->dst)))
    {
      return -EINVAL;
    }

  return OK;
}

static void esp_dma2d_prepare(FAR struct esp_graphics_s *priv)
{
  FAR const struct esp_graphics_job_s *job = &priv->job;
  unsigned int count = (job->height + ESP_GRAPHICS_BAND_ROWS - 1) /
                       ESP_GRAPHICS_BAND_ROWS;
  unsigned int i;
  unsigned int y;
  unsigned int rows;

  for (i = 0; i < count; i++)
    {
      y = i * ESP_GRAPHICS_BAND_ROWS;
      rows = MIN(ESP_GRAPHICS_BAND_ROWS, job->height - y);
      esp_graphics_descriptor(&priv->tx[i].dma, &job->src,
                               job->src_x, job->src_y + y, job->width, rows);
      esp_graphics_descriptor(&priv->rx[i].dma, &job->dst,
                               job->dst_x, job->dst_y + y, job->width, rows);
      if (i + 1 < count)
        {
          priv->tx[i].dma.next = &priv->tx[i + 1].dma;
          priv->rx[i].dma.next = &priv->rx[i + 1].dma;
          priv->tx[i].dma.suc_eof = 0;
        }

      priv->rx[i].dma.suc_eof = 0;
    }

  esp_graphics_tx(0, DMA2D_TRIG_PERIPH_M2M, 4, &priv->tx[0].dma);
  esp_graphics_rx(DMA2D_TRIG_PERIPH_M2M, 4, &priv->rx[0].dma);
}

static int esp_graphics_cache(FAR const struct esp_graphics_buffer_s *buffer,
                               int flags)
{
  return esp_cache_msync(buffer->data, buffer->size, flags) == ESP_OK ?
         OK : -EIO;
}

int esp_graphics_submit(FAR const struct esp_graphics_job_s *job,
                        FAR uint32_t *ticket)
{
  FAR struct esp_graphics_s *priv = g_graphics;
  irqstate_t flags;
  int ret;

  ret = esp_graphics_check_context();
  if (ret < 0)
    {
      return ret;
    }

  if (priv == NULL)
    {
      return -ENODEV;
    }

  if (job == NULL || ticket == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_trylock(&priv->mutex);
  if (ret < 0)
    {
      return -EBUSY;
    }

  if (priv->busy)
    {
      ret = -EBUSY;
      goto out;
    }

  priv->job = *job;
  job = &priv->job;
  ret = esp_graphics_validate(job);
  if (ret < 0)
    {
      goto out;
    }

  ret = esp_graphics_cache(&job->dst, ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                      ESP_CACHE_MSYNC_FLAG_INVALIDATE);
  if (ret == OK && job->operation != ESP_GRAPHICS_FILL)
    {
      ret = esp_graphics_cache(&job->src, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

  if (ret == OK && job->operation == ESP_GRAPHICS_BLEND)
    {
      ret = esp_graphics_cache(&job->foreground,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

  if (ret < 0)
    {
      goto out;
    }

  esp_graphics_reset();
  memset(priv->tx, 0, DESC_BYTES);
  if (job->operation == ESP_GRAPHICS_COPY)
    {
      esp_dma2d_prepare(priv);
    }
#ifdef CONFIG_ESP32P4_PPA
  else
    {
      ret = esp_ppa_prepare(priv);
      if (ret < 0)
        {
          goto out;
        }
    }
#endif

  if (esp_cache_msync(priv->tx, DESC_BYTES,
                      ESP_CACHE_MSYNC_FLAG_DIR_C2M) != ESP_OK)
    {
      ret = -EIO;
      goto out;
    }

  while (nxsem_trywait(&priv->done) == OK);
  flags = spin_lock_irqsave(&priv->lock);
  priv->ticket++;
  if (priv->ticket == 0)
    {
      priv->ticket++;
    }

  *ticket = priv->ticket;
  priv->busy = true;
  priv->completed = false;
  priv->result = -EINPROGRESS;
  UP_DMB();
  dma2d_ll_rx_start(DMA2D_LL_GET_HW(0), 0);
  if (job->operation != ESP_GRAPHICS_FILL)
    {
      dma2d_ll_tx_start(DMA2D_LL_GET_HW(0), 0);
    }

  if (job->operation == ESP_GRAPHICS_BLEND)
    {
      dma2d_ll_tx_start(DMA2D_LL_GET_HW(0), 1);
    }

#ifdef CONFIG_ESP32P4_PPA
  if (job->operation != ESP_GRAPHICS_COPY)
    {
      esp_ppa_start(job);
    }
#endif

  spin_unlock_irqrestore(&priv->lock, flags);
  ret = OK;
out:
  nxmutex_unlock(&priv->mutex);
  return ret;
}

static void esp_graphics_quiesce(void)
{
  FAR dma2d_dev_t *dev = DMA2D_LL_GET_HW(0);
  unsigned int poll;

  dma2d_ll_tx_abort(dev, 0, true);
  dma2d_ll_tx_abort(dev, 1, true);
  dma2d_ll_rx_abort(dev, 0, true);
  for (poll = 0; poll < 10000; poll++)
    {
      if (dma2d_ll_tx_is_reset_avail(dev, 0) &&
          dma2d_ll_tx_is_reset_avail(dev, 1) &&
          dma2d_ll_rx_is_reset_avail(dev, 0))
        {
          esp_graphics_reset();
          return;
        }

      up_udelay(10);
    }

  /* Returning ownership of buffers with outstanding AXI writes is unsafe. */

  PANIC();
  for (; ; )
    {
      up_udelay(1000);
    }
}

static int esp_graphics_finish(uint32_t ticket, unsigned int timeout_ms,
                                bool cancel)
{
  FAR struct esp_graphics_s *priv = g_graphics;
  irqstate_t flags;
  int result;
  int ret;

  ret = esp_graphics_check_context();
  if (ret < 0)
    {
      return ret;
    }

  if (priv == NULL)
    {
      return -ENODEV;
    }

  if (timeout_ms > 60000)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->mutex);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->busy || priv->ticket != ticket)
    {
      nxmutex_unlock(&priv->mutex);
      return -ENOENT;
    }

  ret = cancel ? -ECANCELED :
        nxsem_tickwait_uninterruptible(&priv->done, MSEC2TICK(timeout_ms));
  if (ret == -EAGAIN)
    {
      ret = -ETIMEDOUT;
    }

  flags = spin_lock_irqsave(&priv->lock);
  result = priv->completed && !cancel ? priv->result : ret;
  spin_unlock_irqrestore(&priv->lock, flags);
  esp_graphics_quiesce();
  flags = spin_lock_irqsave(&priv->lock);
  priv->busy = false;
  spin_unlock_irqrestore(&priv->lock, flags);
  ret = esp_graphics_cache(&priv->job.dst, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
  nxmutex_unlock(&priv->mutex);
  return ret < 0 ? ret : result;
}

int esp_graphics_wait(uint32_t ticket, unsigned int timeout_ms)
{
  return esp_graphics_finish(ticket, timeout_ms, false);
}

int esp_graphics_cancel(uint32_t ticket)
{
  return esp_graphics_finish(ticket, 0, true);
}
