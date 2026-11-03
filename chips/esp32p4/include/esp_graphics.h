/****************************************************************************
 * arch/risc-v/src/esp32p4/include/esp_graphics.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_GRAPHICS_H
#define __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_GRAPHICS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

#define ESP_GRAPHICS_ALIGNMENT 64

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum esp_graphics_format_e
{
  ESP_GRAPHICS_RGB565,
  ESP_GRAPHICS_RGB888,    /* B, G, R bytes */
  ESP_GRAPHICS_ARGB8888,  /* B, G, R, A bytes */
  ESP_GRAPHICS_YUV422     /* U, Y0, V, Y1; BT.601 limited range */
};

enum esp_graphics_operation_e
{
  ESP_GRAPHICS_COPY,
  ESP_GRAPHICS_FILL,
  ESP_GRAPHICS_SRM,
  ESP_GRAPHICS_BLEND
};

struct esp_graphics_buffer_s
{
  FAR void *data;
  size_t size;            /* Entire allocation, multiple of alignment */
  uint32_t stride;        /* Bytes per row, including padding */
  uint16_t height;
  enum esp_graphics_format_e format;
};

struct esp_graphics_job_s
{
  enum esp_graphics_operation_e operation;
  struct esp_graphics_buffer_s src;
  struct esp_graphics_buffer_s foreground;
  struct esp_graphics_buffer_s dst;
  uint16_t width;         /* Input ROI width/height, in pixels */
  uint16_t height;
  uint16_t src_x;
  uint16_t src_y;
  uint16_t fg_x;
  uint16_t fg_y;
  uint16_t dst_x;
  uint16_t dst_y;
  uint16_t scale_x_q4;    /* SRM: 16 = 1x, 8 = 0.5x */
  uint16_t scale_y_q4;
  uint16_t rotation;      /* Counterclockwise: 0, 90, 180, 270 */
  bool mirror_x;
  bool mirror_y;
  uint32_t color;         /* FILL: 0xAARRGGBB */
  bool bg_pixel_alpha;    /* BLEND: retain pixel alpha */
  bool fg_pixel_alpha;
  uint8_t bg_alpha;       /* BLEND: fixed alpha, straight color */
  uint8_t fg_alpha;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* FLAT-mode kernel interface.  One outstanding job across DMA2D and PPA.
 * Caller owns all complete buffer allocations until wait/cancel returns;
 * no CPU or other DMA access is allowed in that interval.  Inputs may alias
 * each other, but destination must not overlap either input.  Completion is
 * interrupt driven.  A successful submit always needs wait or cancel.
 * SMP coexistence requires callers pinned to CPU0 for every API (-EXDEV
 * otherwise).  Keep buffer producers/consumers on CPU0, and do not change
 * their affinity or share buffers with CPU1 while a job is outstanding.
 * Interrupt context is rejected with -EPERM.
 */

int esp_graphics_initialize(void);
int esp_graphics_submit(FAR const struct esp_graphics_job_s *job,
                        FAR uint32_t *ticket);
int esp_graphics_wait(uint32_t ticket, unsigned int timeout_ms);
int esp_graphics_cancel(uint32_t ticket);

#ifdef __cplusplus
}
#endif

#endif /* __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_GRAPHICS_H */
