/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_hosted.c
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

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED

#include <errno.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/signal.h>

#include <arch/board/board.h>
#include <arch/chip/esp_hosted_sdio.h>
#include <arch/chip/esp_hosted_transport.h>

#include "espressif/esp_gpio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP_HOSTED_C6_RESET_ASSERT_MS       10

/* Allow the coprocessor firmware to boot before the first SDIO probe.
 * This delay is a startup margin, not an indication of SDIO readiness.
 */

#define ESP_HOSTED_C6_BOOT_DELAY_MS       1000

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The board owns reset and pin routing.  The chip transport owns SDIO
 * enumeration and retains the P4 SDMMC host after a successful probe.
 */

static FAR struct esp_hosted_transport_s *g_esp_hosted_transport;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int board_esp_hosted_configure_pin(int pin, gpio_pinattr_t attr)
{
  if (esp_configgpio(pin, attr) < 0)
    {
      return -EIO;
    }

  return OK;
}

static int board_esp_hosted_route_sdio(void)
{
  struct esp_hosted_sdio_pins_s pins =
  {
    .clk_gpio = BOARD_ESP_HOSTED_SDIO_CLK_GPIO,
    .cmd_gpio = BOARD_ESP_HOSTED_SDIO_CMD_GPIO,
    .d0_gpio = BOARD_ESP_HOSTED_SDIO_D0_GPIO,
    .d1_gpio = BOARD_ESP_HOSTED_SDIO_D1_GPIO,
    .d2_gpio = BOARD_ESP_HOSTED_SDIO_D2_GPIO,
    .d3_gpio = BOARD_ESP_HOSTED_SDIO_D3_GPIO,
  };

  return esp_hosted_sdio_route_pins(&pins);
}

static int board_esp_hosted_reset_c6(void)
{
  int ret;

  ret = board_esp_hosted_configure_pin(BOARD_ESP_HOSTED_C6_RESET_GPIO,
                                       OUTPUT);
  if (ret < 0)
    {
      return ret;
    }

  esp_gpiowrite(BOARD_ESP_HOSTED_C6_RESET_GPIO, false);
  up_mdelay(ESP_HOSTED_C6_RESET_ASSERT_MS);
  esp_gpiowrite(BOARD_ESP_HOSTED_C6_RESET_GPIO, true);
  syslog(LOG_INFO,
         "INFO: ESP-Hosted C6 reset released: gpio=%u boot_wait_ms=%u\n",
         (unsigned int)BOARD_ESP_HOSTED_C6_RESET_GPIO,
         (unsigned int)ESP_HOSTED_C6_BOOT_DELAY_MS);

  /* Bring-up runs in task context.  Let other tasks run during boot. */

  ret = nxsig_usleep(ESP_HOSTED_C6_BOOT_DELAY_MS * 1000);
  if (ret < 0)
    {
      esp_gpiowrite(BOARD_ESP_HOSTED_C6_RESET_GPIO, false);
      return ret;
    }

  syslog(LOG_INFO, "INFO: ESP-Hosted C6 boot wait complete; probing SDIO\n");
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_esp_hosted_initialize(void)
{
  struct esp_hosted_transport_config_s config;
  FAR const char *stage;
  int ret;

  if (g_esp_hosted_transport != NULL)
    {
      return OK;
    }

  stage = "route_sdio";
  ret = board_esp_hosted_route_sdio();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: ESP-Hosted C6 probe: stage=%s result=%d\n",
             stage, ret);
      return ret;
    }

  stage = "reset_c6";
  ret = board_esp_hosted_reset_c6();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: ESP-Hosted C6 probe: stage=%s result=%d\n",
             stage, ret);
      return ret;
    }

  config.sdio.slot = BOARD_ESP_HOSTED_SDIO_SLOT;
  config.sdio.bus_width = ESP_HOSTED_SDIO_WIDTH1;
  config.sdio.clock_khz = BOARD_ESP_HOSTED_PROBING_CLOCK_KHZ;
  stage = "transport_initialize";
  ret = esp_hosted_transport_initialize(&config, &g_esp_hosted_transport);
  if (ret < 0)
    {
      esp_gpiowrite(BOARD_ESP_HOSTED_C6_RESET_GPIO, false);
      goto fail;
    }

  return OK;

fail:
  syslog(LOG_ERR, "ERROR: ESP-Hosted C6 initialize: stage=%s result=%d\n",
         stage, ret);
  return ret;
}

FAR struct esp_hosted_sdio_s *board_esp_hosted_sdio_get(void)
{
  return esp_hosted_transport_get_sdio(g_esp_hosted_transport);
}

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED */
