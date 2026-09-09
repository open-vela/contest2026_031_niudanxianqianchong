/****************************************************************************
 * chips/esp32p4/include/esp_isp.h
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_ISP_H
#define __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_ISP_H

#include <nuttx/compiler.h>

#include <stdbool.h>
#include <stdint.h>

enum esp_isp_output_e
{
  ESP_ISP_OUTPUT_RAW_BYPASS = 0,
  ESP_ISP_OUTPUT_RGB565
};

/* Keep these values aligned with ESP-IDF color_raw_element_order_t, while
 * avoiding an ESP-IDF HAL header dependency in board-facing interfaces.
 */

enum esp_isp_bayer_order_e
{
  ESP_ISP_BAYER_ORDER_BGGR = 0,
  ESP_ISP_BAYER_ORDER_GBRG,
  ESP_ISP_BAYER_ORDER_GRBG,
  ESP_ISP_BAYER_ORDER_RGGB
};

struct esp_isp_config_s
{
  uint16_t width;
  uint16_t height;
  uint8_t input_bpp;
  enum esp_isp_bayer_order_e bayer_order;
  bool line_start_packet;
  bool line_end_packet;
  bool byte_swap;
  enum esp_isp_output_e output;
};

struct esp_isp_s
{
  bool clock_enabled;
  bool initialized;
};

int esp_isp_initialize(FAR struct esp_isp_s *isp,
                       FAR const struct esp_isp_config_s *config);
void esp_isp_deinitialize(FAR struct esp_isp_s *isp);

#endif
