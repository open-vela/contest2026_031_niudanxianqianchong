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
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/signal.h>

#include <arch/board/board.h>

#include "espressif/esp_i2c.h"

#include "gt911.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

#define BOARD_GT911_POWER_ON_DELAY_US  (100 * 1000)

/* The P4X reference LCD adapter does not expose GT911 reset or interrupt to
 * the SoC.  Supplying no board callbacks intentionally selects the GT911
 * driver's LPWORK polling path.
 */

static const struct gt911_config_s g_gt911_configs[] =
{
  {
    .board              = NULL,
    .frequency          = BOARD_GT911_I2C_FREQUENCY,
    .address            = BOARD_GT911_I2C_ADDRESS,
    .poll_interval_ms   = BOARD_GT911_POLL_INTERVAL_MS,
    .x_resolution       = BOARD_GT911_X_RESOLUTION,
    .y_resolution       = BOARD_GT911_Y_RESOLUTION,
    .max_points         = BOARD_GT911_MAX_POINTS,
    .event_buffer_count = GT911_EVENT_BUFFER_COUNT,
    .swap_xy            = false,
    .invert_x           = true,
    .invert_y           = true,
  },
  {
    .board              = NULL,
    .frequency          = BOARD_GT911_I2C_FREQUENCY,
    .address            = BOARD_GT911_I2C_BACKUP_ADDRESS,
    .poll_interval_ms   = BOARD_GT911_POLL_INTERVAL_MS,
    .x_resolution       = BOARD_GT911_X_RESOLUTION,
    .y_resolution       = BOARD_GT911_Y_RESOLUTION,
    .max_points         = BOARD_GT911_MAX_POINTS,
    .event_buffer_count = GT911_EVENT_BUFFER_COUNT,
    .swap_xy            = false,
    .invert_x           = true,
    .invert_y           = true,
  },
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_gt911_initialize
 ****************************************************************************/

int board_gt911_initialize(void)
{
  FAR const struct gt911_config_s *config;
  FAR struct i2c_master_s *i2c;
  unsigned int i;
  int ret;

  i2c = esp_i2cbus_initialize(BOARD_GT911_I2C_BUS);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize GT911 I2C%d\n",
             BOARD_GT911_I2C_BUS);
      return -ENODEV;
    }

  /* The LCD adapter does not expose the GT911 reset line.  Allow its local
   * supply and controller boot sequence to settle before the first I2C
   * product-ID transaction.
   */

  syslog(LOG_INFO, "GT911 power-on wait: %u ms before product-ID probe\n",
         (unsigned int)(BOARD_GT911_POWER_ON_DELAY_US / 1000));
  ret = nxsig_usleep(BOARD_GT911_POWER_ON_DELAY_US);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GT911 power-on wait failed: %d\n", ret);
      esp_i2cbus_uninitialize(i2c);
      return ret;
    }

  for (i = 0;
       i < sizeof(g_gt911_configs) / sizeof(g_gt911_configs[0]);
       i++)
    {
      config = &g_gt911_configs[i];
      syslog(LOG_INFO, "GT911 probing I2C address 0x%02x\n",
             config->address);

      ret = gt911_register("/dev/input0", i2c, config);
      if (ret >= 0)
        {
          syslog(LOG_INFO,
                 "GT911 registered at /dev/input0: I2C%d address=0x%02x "
                 "poll=%ums\n",
                 BOARD_GT911_I2C_BUS, config->address,
                 BOARD_GT911_POLL_INTERVAL_MS);
          return OK;
        }

      syslog(LOG_WARNING,
             "GT911 address probe failed: address=0x%02x ret=%d\n",
             config->address, ret);

      /* Only an I2C probe failure can indicate that the controller selected
       * its alternate address.  Preserve registration/resource errors rather
       * than hiding them behind a second address attempt.
       */

      if (ret != -EIO)
        {
          break;
        }
    }

  syslog(LOG_ERR,
         "ERROR: Failed to register GT911 at addresses 0x%02x/0x%02x: %d\n",
         BOARD_GT911_I2C_ADDRESS, BOARD_GT911_I2C_BACKUP_ADDRESS, ret);
  esp_i2cbus_uninitialize(i2c);
  return ret;
}
