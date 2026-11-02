/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_graphics_validate.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <arch/chip/esp_graphics.h>
#include <errno.h>
#include <limits.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

unsigned int esp_graphics_bpp(enum esp_graphics_format_e format)
{
  switch (format)
    {
      case ESP_GRAPHICS_RGB565:
      case ESP_GRAPHICS_YUV422:
        return 2;
      case ESP_GRAPHICS_RGB888:
        return 3;
      case ESP_GRAPHICS_ARGB8888:
        return 4;
      default:
        return 0;
    }
}

int esp_graphics_buffer_bounds(
  FAR const struct esp_graphics_buffer_s *buffer,
                              uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height)
{
  unsigned int bpp = esp_graphics_bpp(buffer->format);
  uintptr_t address = (uintptr_t)buffer->data;

  if (bpp == 0 || width == 0 || height == 0 || buffer->size == 0 ||
      address == 0 ||
      (address & (ESP_GRAPHICS_ALIGNMENT - 1)) != 0 ||
      (buffer->size & (ESP_GRAPHICS_ALIGNMENT - 1)) != 0 ||
      buffer->size > UINTPTR_MAX - address ||
      buffer->stride == 0 || buffer->stride % bpp != 0 ||
      buffer->stride % 4 != 0 || buffer->stride / bpp > 16383 ||
      buffer->height == 0 || buffer->height > 16383 ||
      (uint64_t)buffer->stride * buffer->height > buffer->size ||
      (uint64_t)x + width > buffer->stride / bpp ||
      (uint64_t)y + height > buffer->height ||
      (x * bpp) % 4 != 0 || (width * bpp) % 4 != 0)
    {
      return -EINVAL;
    }

  return OK;
}
