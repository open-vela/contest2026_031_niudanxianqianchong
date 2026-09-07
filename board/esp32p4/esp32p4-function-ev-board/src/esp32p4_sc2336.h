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

#include <stdbool.h>
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

/* The first capture profile is copied from Espressif's Apache-2.0 SC2336
 * component (esp_cam_sensor v1.7.0).  It is deliberately a small RAW8
 * profile so one frame fits in PSRAM while CSI bring-up is verified.
 */

#define SC2336_RAW8_WIDTH              1024
#define SC2336_RAW8_HEIGHT              600
#define SC2336_RAW8_BITS_PER_PIXEL      8
#define SC2336_RAW8_DATA_TYPE           0x2a
#define SC2336_RAW8_LANE_NUM            2
#define SC2336_RAW8_LANE_RATE_MBPS      288
#define SC2336_RAW8_FPS                 30

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

/****************************************************************************
 * Name: esp32p4_sc2336_write_register_pointer
 *
 * Description:
 *   Send only a 16-bit SC2336 register address.  This is intended for SCCB
 *   bus diagnosis and does not write a sensor register value or read data.
 *
 ****************************************************************************/

int esp32p4_sc2336_write_register_pointer(
  FAR struct i2c_master_s *i2c, uint8_t address, uint32_t frequency,
  uint16_t reg);

/****************************************************************************
 * Name: esp32p4_sc2336_read_register_data
 *
 * Description:
 *   Read one byte after a register-pointer write has completed.  This is
 *   intended for the split SCCB diagnostic.
 *
 ****************************************************************************/

int esp32p4_sc2336_read_register_data(
  FAR struct i2c_master_s *i2c, uint8_t address, uint32_t frequency,
  FAR uint8_t *value);

/****************************************************************************
 * Name: esp32p4_sc2336_configure_raw8
 *
 * Description:
 *   Put SC2336 into the 1024x600@30fps, 2-lane RAW8 profile.  The sensor
 *   remains in stream-off state until esp32p4_sc2336_set_stream() is called.
 *
 ****************************************************************************/

int esp32p4_sc2336_configure_raw8(FAR struct i2c_master_s *i2c,
                                  uint8_t address, uint32_t frequency);

/****************************************************************************
 * Name: esp32p4_sc2336_soft_reset
 *
 * Description:
 *   Reset the SC2336 through its standard software-reset register and wait
 *   for the reset interval used by the validated RAW8 profile.
 *
 ****************************************************************************/

int esp32p4_sc2336_soft_reset(FAR struct i2c_master_s *i2c,
                              uint8_t address, uint32_t frequency);

/****************************************************************************
 * Name: esp32p4_sc2336_set_stream
 ****************************************************************************/

int esp32p4_sc2336_set_stream(FAR struct i2c_master_s *i2c,
                               uint8_t address, uint32_t frequency,
                               bool enable);

#endif /* __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_SRC_ESP32P4_SC2336_H */
