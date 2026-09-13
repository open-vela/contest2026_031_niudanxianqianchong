/****************************************************************************
 * chips/esp32p4/include/esp_hosted_sdio.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_HOSTED_SDIO_H
#define __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_HOSTED_SDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP_HOSTED_SDIO_SLOT0             0
#define ESP_HOSTED_SDIO_SLOT1             1

#define ESP_HOSTED_SDIO_WIDTH1            1
#define ESP_HOSTED_SDIO_WIDTH4            4

#define ESP_HOSTED_SDIO_PROBING_CLOCK_KHZ 400
#define ESP_HOSTED_SDIO_MAX_CLOCK_KHZ     40000

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct esp_hosted_sdio_s;

struct esp_hosted_sdio_config_s
{
  uint8_t  slot;
  uint8_t  bus_width;
  uint32_t clock_khz;
};

struct esp_hosted_sdio_status_s
{
  uint32_t revision;
  uint32_t hardware_config;
  uint32_t clock_khz;
  uint8_t  slot;
  uint8_t  bus_width;
  bool     initialized;
};

/* The board provides its GPIO wiring while this chip-layer interface owns
 * the ESP32-P4 SDMMC slot 1 GPIO-matrix signal selection.  C6 reset and
 * wakeup remain board-owned and are deliberately absent from this type.
 */

struct esp_hosted_sdio_pins_s
{
  int clk_gpio;
  int cmd_gpio;
  int d0_gpio;
  int d1_gpio;
  int d2_gpio;
  int d3_gpio;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: esp_hosted_sdio_route_pins
 *
 * Description:
 *   Configure board-provided GPIOs for ESP32-P4 SDMMC slot 1 probing.  The
 *   function routes CLK, CMD and DAT0 to the SDMMC matrix, leaves DAT1 and
 *   DAT2 pulled up, and holds DAT3 high as GPIO so the attached C6 enters
 *   SDIO mode before the later four-bit bus-width negotiation.
 *
 ****************************************************************************/

int esp_hosted_sdio_route_pins(
  FAR const struct esp_hosted_sdio_pins_s *pins);

/****************************************************************************
 * Name: esp_hosted_sdio_initialize
 *
 * Description:
 *   Acquire and reset the single ESP32-P4 SDMMC controller for a
 *   board-routed ESP-Hosted SDIO device.  The call configures the clock and
 *   bus width, but it does not configure GPIOs, reset the coprocessor, or
 *   perform CMD52/CMD53 transfers.
 *
 ****************************************************************************/

int esp_hosted_sdio_initialize(
  FAR const struct esp_hosted_sdio_config_s *config,
  FAR struct esp_hosted_sdio_s **host);

/****************************************************************************
 * Name: esp_hosted_sdio_set_clock
 *
 * Description:
 *   Change the selected slot clock.  clock_khz must be in the range from
 *   ESP_HOSTED_SDIO_PROBING_CLOCK_KHZ through
 *   ESP_HOSTED_SDIO_MAX_CLOCK_KHZ.
 *
 ****************************************************************************/

int esp_hosted_sdio_set_clock(FAR struct esp_hosted_sdio_s *host,
                               uint32_t clock_khz);

/****************************************************************************
 * Name: esp_hosted_sdio_command
 *
 * Description:
 *   Submit one synchronous command without a data phase.  This primitive is
 *   intended for SDIO discovery commands such as CMD0 and CMD5.  If response
 *   is false, response_value may be NULL.  send_init emits the initial 80
 *   clocks and must be used only for the first command after a coprocessor
 *   reset.
 *
 ****************************************************************************/

int esp_hosted_sdio_command(FAR struct esp_hosted_sdio_s *host,
                             uint8_t command, uint32_t argument,
                             bool response, bool check_crc, bool send_init,
                             FAR uint32_t *response_value);

/****************************************************************************
 * Name: esp_hosted_sdio_get_status
 *
 * Description:
 *   Return the controller identification and active transport parameters for
 *   diagnostics.  The information describes only the P4 SDMMC controller;
 *   it does not imply that a C6 is attached or that ESP-Hosted has completed
 *   its protocol handshake.
 *
 ****************************************************************************/

int esp_hosted_sdio_get_status(FAR struct esp_hosted_sdio_s *host,
                                FAR struct esp_hosted_sdio_status_s *status);

/****************************************************************************
 * Name: esp_hosted_sdio_deinitialize
 *
 * Description:
 *   Stop the selected slot clock and release the P4 SDMMC controller.
 *
 ****************************************************************************/

int esp_hosted_sdio_deinitialize(FAR struct esp_hosted_sdio_s *host);

#endif /* __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_HOSTED_SDIO_H */
