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

/* White balance gain (WBG) is expressed in 8.4 fixed point: the hardware
 * register defaults to 256, which means unity gain.  Zero disables WBG
 * entirely so that the raw sensor response is passed through unchanged.
 */

#define ESP_ISP_WBG_GAIN_UNITY   256

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

  /* Optional manual white balance.  All three fields must be non-zero to
   * enable WBG; the ISP then multiplies each demosaiced channel by
   * gain / 256 before the RGB->YUV->RGB conversion.
   */

  uint16_t wbg_gain_r;
  uint16_t wbg_gain_g;
  uint16_t wbg_gain_b;
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
