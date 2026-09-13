/****************************************************************************
 * chips/esp32p4/include/esp_hosted_transport.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_HOSTED_TRANSPORT_H
#define __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_HOSTED_TRANSPORT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <arch/chip/esp_hosted_sdio.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct esp_hosted_transport_s;

struct esp_hosted_transport_config_s
{
  struct esp_hosted_sdio_config_s sdio;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: esp_hosted_transport_initialize
 *
 * Description:
 *   Enumerate an ESP-Hosted SDIO coprocessor after the board has configured
 *   the P4 SDMMC pins and released the coprocessor reset.  The current
 *   control-plane implementation performs CMD0, CMD5, CMD3 (RCA), CMD7
 *   (select card) and a function-0 CMD52 read.  It does not register a
 *   wireless network device.
 *
 ****************************************************************************/

int esp_hosted_transport_initialize(
  FAR const struct esp_hosted_transport_config_s *config,
  FAR struct esp_hosted_transport_s **transport);

/****************************************************************************
 * Name: esp_hosted_transport_get_sdio
 *
 * Description:
 *   Return the P4 SDMMC host associated with an initialized ESP-Hosted
 *   transport.  CMD53 transport and function-interrupt support use this
 *   handle in a later phase.
 *
 ****************************************************************************/

FAR struct esp_hosted_sdio_s *esp_hosted_transport_get_sdio(
  FAR struct esp_hosted_transport_s *transport);

#endif /* __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_HOSTED_TRANSPORT_H */
