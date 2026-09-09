/****************************************************************************
 * chips/esp32p4/common/espressif/esp_isp.c
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <string.h>

#include "esp_clk_tree.h"
#include "esp_err.h"
#include "esp_private/periph_ctrl.h"
#include "hal/isp_ll.h"

#include <arch/chip/esp_isp.h>

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
