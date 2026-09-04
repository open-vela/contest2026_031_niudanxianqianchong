/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_sc2336.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board-private SC2336 SCCB helpers used during camera bring-up.
 ****************************************************************************/

#ifndef __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_SRC_ESP32P4_SC2336_H
#define __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_SRC_ESP32P4_SC2336_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/compiler.h>

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Sensor identity values from Espressif's Apache-2.0 SC2336 component:
 * github.com/espressif/esp-video-components/tree/master/esp_cam_sensor/
 * sensors/sc2336
 */

#define SC2336_PRODUCT_ID             0xcb3a
#define SC2336_PRODUCT_ID_HIGH_REG    0x3107
#define SC2336_PRODUCT_ID_LOW_REG     0x3108

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct i2c_master_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int esp32p4_sc2336_probe(FAR struct i2c_master_s *i2c,
                         uint8_t address, uint32_t frequency,
                         FAR uint16_t *product_id);

#endif /* __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_SRC_ESP32P4_SC2336_H */
