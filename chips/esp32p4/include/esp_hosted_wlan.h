/****************************************************************************
 * chips/esp32p4/include/esp_hosted_wlan.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_HOSTED_WLAN_H
#define __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_HOSTED_WLAN_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

#include <arch/chip/esp_hosted_transport.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: esp_hosted_wlan_initialize
 *
 * Description:
 *   Register the ESP32-C6 station data path as a NuttX IEEE 802.11 netdev.
 *   The resulting interface is named wlan0 by the netdev upper half.  The
 *   C6 station interface must already be initialized, set to STA mode and
 *   started before this function is called.
 *
 ****************************************************************************/

int esp_hosted_wlan_initialize(FAR struct esp_hosted_transport_s *transport);

/****************************************************************************
 * Name: esp_hosted_wlan_set_link
 *
 * Description:
 *   Propagate the C6 station association state to the NuttX netdev.  A
 *   registered interface may be administratively up before it has joined an
 *   access point; in that state WLAN payload must not enter the NuttX data
 *   plane.  The ESP-Hosted STA-connected and STA-disconnected events call
 *   this function when their event support is added.
 *
 ****************************************************************************/

void esp_hosted_wlan_set_link(bool up);

/****************************************************************************
 * Name: esp_hosted_wlan_deinitialize
 *
 * Description:
 *   Unregister the NuttX station netdev and detach it from the ESP-Hosted
 *   receive path before the transport is stopped or reset.
 *
 ****************************************************************************/

int esp_hosted_wlan_deinitialize(void);

#endif /* __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_HOSTED_WLAN_H */
