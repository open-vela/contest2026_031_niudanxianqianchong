/****************************************************************************
 * chips/esp32p4/common/espressif/esp_isp.c
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <string.h>

#include "esp_clk_tree.h"
#include "esp_err.h"
#include "esp_private/esp_clk_tree_common.h"
#include "esp_private/periph_ctrl.h"
#include "hal/isp_ll.h"

#include <arch/chip/esp_isp.h>

/* The HAL exposes isp_ll_awb_set_wb_gain() and isp_ll_awb_enable_wb_gain()
 * only for CHIP_SUPPORT_MIN_REV >= 300.  That condition expands to
 * CONFIG_ESP_REV_MIN_FULL, which the ESP32-P4 sdkconfig does not define (it
 * only defines CONFIG_ESP32P4_REV_MIN_FULL), so both helpers compile to empty
 * stubs on this target.  Program the WBG registers directly instead.
 *
 * The WBG gain registers are 12 bits wide with a hardware default of 256,
 * i.e. 8.4 fixed point where 256 is unity.
 */

#define ESP_ISP_WBG_COEF_MAX  0xfff

static void esp_isp_set_wbg(FAR isp_dev_t *hw,
                            FAR const struct esp_isp_config_s *config)
{
  if (config->wbg_gain_r == 0 || config->wbg_gain_g == 0 ||
      config->wbg_gain_b == 0)
    {
      hw->cntl.wbg_en = 0;
      return;
    }

  hw->wbg_coef_r.wbg_r = config->wbg_gain_r & ESP_ISP_WBG_COEF_MAX;
  hw->wbg_coef_g.wbg_g = config->wbg_gain_g & ESP_ISP_WBG_COEF_MAX;
  hw->wbg_coef_b.wbg_b = config->wbg_gain_b & ESP_ISP_WBG_COEF_MAX;

  /* The shadow mode is set to UPDATE_ONLY_NEXT_VSYNC, so the coefficients
   * become active on the next frame.  Request that update explicitly.
   */

  hw->shadow_reg_ctrl.wbg_update = 1;
  hw->cntl.wbg_en = 1;
}

int esp_isp_initialize(FAR struct esp_isp_s *state,
                       FAR const struct esp_isp_config_s *config)
{
  hal_utils_clk_div_t div;
  FAR isp_dev_t *hw = ISP_LL_GET_HW(0);
  bool valid;

  if (state == NULL || config == NULL || config->width == 0 ||
      config->height == 0 || config->input_bpp != 8)
    {
      return -EINVAL;
    }

  if (esp_clk_tree_enable_src((soc_module_clk_t)ISP_CLK_SRC_PLL160, true) !=
      ESP_OK)
    {
      return -EIO;
    }

  memset(&div, 0, sizeof(div));
  div.integer = 2;
  PERIPH_RCC_ATOMIC()
    {
      isp_ll_select_clk_source(hw, ISP_CLK_SRC_PLL160);
      isp_ll_set_clock_div(hw, &div);
      isp_ll_enable_module_clock(hw, true);
      isp_ll_reset_module_clock(hw);
    }

  state->clock_enabled = true;
  isp_ll_init(hw);
  isp_ll_clk_enable(hw, true);
  hw->int_ena.val = 0;
  isp_ll_set_input_data_source(hw, ISP_INPUT_DATA_SOURCE_CSI);
  isp_ll_enable_line_start_packet_exist(hw, config->line_start_packet);
  isp_ll_enable_line_end_packet_exist(hw, config->line_end_packet);
  isp_ll_set_intput_data_v_row_num(hw, config->height);
  isp_ll_set_bayer_mode(hw, (color_raw_element_order_t)config->bayer_order);
  isp_ll_set_byte_swap(hw, config->byte_swap);

  if (config->output == ESP_ISP_OUTPUT_RAW_BYPASS)
    {
      isp_ll_enable(hw, false);
      isp_ll_set_intput_data_h_pixel_num(hw, config->width * 8 / 32);
    }
  else
    {
      valid = isp_ll_set_input_data_color_format(hw, ISP_COLOR_RAW8);
      valid = valid && isp_ll_set_output_data_color_format(hw,
                                                             ISP_COLOR_RGB565);
      if (!valid)
        {
          esp_isp_deinitialize(state);
          return -EINVAL;
        }

      isp_ll_set_intput_data_h_pixel_num(hw, config->width);
      esp_isp_set_wbg(hw, config);
      isp_ll_enable(hw, true);
    }

  isp_ll_shadow_set_mode(hw, ISP_SHADOW_MODE_UPDATE_ONLY_NEXT_VSYNC);
  state->initialized = true;
  return OK;
}

void esp_isp_deinitialize(FAR struct esp_isp_s *state)
{
  FAR isp_dev_t *hw = ISP_LL_GET_HW(0);

  if (state != NULL && state->clock_enabled)
    {
      isp_ll_enable(hw, false);
      isp_ll_clk_enable(hw, false);
      PERIPH_RCC_ATOMIC()
        {
          isp_ll_enable_module_clock(hw, false);
        }
    }

  if (state != NULL)
    {
      state->initialized = false;
      state->clock_enabled = false;
    }
}
