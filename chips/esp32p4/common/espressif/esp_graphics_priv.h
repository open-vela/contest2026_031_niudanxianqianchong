/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_graphics_priv.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_GRAPHICS_PRIV_H
#define __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_GRAPHICS_PRIV_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <arch/chip/esp_graphics.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/spinlock.h>
#include "hal/dma2d_ll.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP_GRAPHICS_DESCRIPTORS 16
#define ESP_GRAPHICS_BAND_ROWS   128

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Each descriptor has its own cache line for hardware writeback. */

struct esp_graphics_desc_s
{
  dma2d_descriptor_t dma;
  uint8_t padding[ESP_GRAPHICS_ALIGNMENT - sizeof(dma2d_descriptor_t)];
};

struct esp_graphics_s
{
  mutex_t mutex;
  spinlock_t lock;
  sem_t done;
  uint32_t ticket;
  bool busy;
  bool completed;
  int result;
  struct esp_graphics_job_s job;
  FAR struct esp_graphics_desc_s *tx;
  FAR struct esp_graphics_desc_s *fg;
  FAR struct esp_graphics_desc_s *rx;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

unsigned int esp_graphics_bpp(enum esp_graphics_format_e format);
int esp_graphics_buffer_bounds(
  FAR const struct esp_graphics_buffer_s *buffer,
  uint32_t x, uint32_t y, uint32_t width, uint32_t height);
int esp_graphics_check_buffer(FAR const struct esp_graphics_buffer_s *buffer,
                              uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height);
void esp_graphics_descriptor(FAR dma2d_descriptor_t *desc,
                             FAR const struct esp_graphics_buffer_s *buffer,
                             uint32_t x, uint32_t y,
                             uint32_t width, uint32_t height);
void esp_graphics_tx(unsigned int channel, dma2d_trigger_peripheral_t periph,
                      int select, FAR dma2d_descriptor_t *desc);
void esp_graphics_rx(dma2d_trigger_peripheral_t periph, int select,
                      FAR dma2d_descriptor_t *desc);

#ifdef CONFIG_ESP32P4_PPA
int esp_ppa_validate(FAR const struct esp_graphics_job_s *job);
int esp_ppa_prepare(FAR struct esp_graphics_s *priv);
void esp_ppa_start(FAR const struct esp_graphics_job_s *job);
void esp_ppa_reset(void);
uint32_t esp_ppa_errors(void);
void esp_ppa_uninitialize(void);
#endif

#endif /* __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_GRAPHICS_PRIV_H */
