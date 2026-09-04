/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_camera.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP32-P4X camera sensor board assembly for staged bring-up.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>

#include <arch/board/board.h>

#include "espressif/esp_i2c.h"

#include "esp32p4_sc2336.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_sc2336_probe(FAR uint16_t *product_id)
{
  FAR struct i2c_master_s *i2c;
  int uninit_ret;
  int ret;

  if (product_id == NULL)
    {
      return -EINVAL;
    }

  i2c = esp_i2cbus_initialize(BOARD_SC2336_I2C_BUS);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize SC2336 I2C%d\n",
             BOARD_SC2336_I2C_BUS);
      return -ENODEV;
    }

  syslog(LOG_INFO,
         "SC2336 probe begin: I2C%d address=0x%02x frequency=%lu\n",
         BOARD_SC2336_I2C_BUS, BOARD_SC2336_I2C_ADDRESS,
         (unsigned long)BOARD_SC2336_I2C_FREQUENCY);

  ret = esp32p4_sc2336_probe(i2c, BOARD_SC2336_I2C_ADDRESS,
                             BOARD_SC2336_I2C_FREQUENCY, product_id);

  /* P1 is an explicit one-shot diagnostic, so release exactly the I2C bus
   * reference acquired above.  A later /dev/video0 implementation will keep
   * its reference for the lifetime of the registered sensor instance.
   */

  uninit_ret = esp_i2cbus_uninitialize(i2c);
  if (uninit_ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to release SC2336 I2C%d: %d\n",
             BOARD_SC2336_I2C_BUS, uninit_ret);
      if (ret >= 0)
        {
          ret = uninit_ret;
        }
    }

  return ret;
}
