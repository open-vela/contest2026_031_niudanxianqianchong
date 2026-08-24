/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_touch.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board registration for the Goodix GT911 capacitive touch controller.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>

#include <nuttx/i2c/i2c_master.h>

#include <arch/board/board.h>

#include "espressif/esp_i2c.h"

#include "gt911.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The P4X reference LCD adapter does not expose GT911 reset or interrupt to
 * the SoC.  Supplying no board callbacks intentionally selects the GT911
 * driver's LPWORK polling path.
 */

static const struct gt911_config_s g_gt911_config =
{
  .board              = NULL,
  .frequency          = BOARD_GT911_I2C_FREQUENCY,
  .address            = BOARD_GT911_I2C_ADDRESS,
  .poll_interval_ms   = BOARD_GT911_POLL_INTERVAL_MS,
  .max_points         = GT911_MAX_POINTS,
  .event_buffer_count = GT911_EVENT_BUFFER_COUNT,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_gt911_initialize
 ****************************************************************************/

int board_gt911_initialize(void)
{
  FAR struct i2c_master_s *i2c;
  int ret;

  i2c = esp_i2cbus_initialize(BOARD_GT911_I2C_BUS);
  if (i2c == NULL)
    {
      snerr("ERROR: Failed to initialize GT911 I2C%d\n",
            BOARD_GT911_I2C_BUS);
      return -ENODEV;
    }

  ret = gt911_register("/dev/input0", i2c, &g_gt911_config);
  if (ret < 0)
    {
      snerr("ERROR: Failed to register GT911 at 0x%02x: %d\n",
            BOARD_GT911_I2C_ADDRESS, ret);
      esp_i2cbus_uninitialize(i2c);
      return ret;
    }

  sninfo("GT911 registered at /dev/input0: I2C%d address=0x%02x poll=%ums\n",
         BOARD_GT911_I2C_BUS, BOARD_GT911_I2C_ADDRESS,
         BOARD_GT911_POLL_INTERVAL_MS);
  return OK;
}
