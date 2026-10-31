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

#include "espressif/esp_gpio.h"
#include "espressif/esp_i2c.h"

#include "gt911.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BOARD_GT911_POWER_ON_DELAY_US  (100 * 1000)

/* GT911 address selection is latched from the INT pin level at the rising
 * edge of RESET.  Driving INT low selects 0x5d, the primary address the
 * driver probes first.
 */

#define BOARD_GT911_RESET_ASSERT_US    (10 * 1000)
#define BOARD_GT911_RESET_RELEASE_US   (60 * 1000)

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Interrupt mode is only reachable when both Dupont wires from the LCD
 * adapter's J6 header are present; see the wiring note in board.h.
 */

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_GT911_IRQ

static int gt911_board_reset(FAR const struct gt911_board_s *board)
{
  int ret;

  UNUSED(board);

  /* Hold INT low while reset is released so the controller latches 0x5d. */

  ret = esp_configgpio(BOARD_GT911_INT_GPIO, OUTPUT);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GT911 INT gpio%d output setup failed: %d\n",
             BOARD_GT911_INT_GPIO, ret);
      return ret;
    }

  esp_gpiowrite(BOARD_GT911_INT_GPIO, false);

  ret = esp_configgpio(BOARD_GT911_RESET_GPIO, OUTPUT);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GT911 reset gpio%d output setup failed: %d\n",
             BOARD_GT911_RESET_GPIO, ret);
      return ret;
    }

  esp_gpiowrite(BOARD_GT911_RESET_GPIO, false);
  nxsig_usleep(BOARD_GT911_RESET_ASSERT_US);
  esp_gpiowrite(BOARD_GT911_RESET_GPIO, true);
  nxsig_usleep(BOARD_GT911_RESET_RELEASE_US);

  /* The address is latched on the reset rising edge.  Release INT
   * immediately afterwards: leaving it driven low would fight the GT911,
   * which owns INT as an output once it leaves reset.
   */

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_GT911_IRQ_EDGE
  ret = esp_configgpio(BOARD_GT911_INT_GPIO, INPUT | PULLUP | FALLING);
#else
  ret = esp_configgpio(BOARD_GT911_INT_GPIO, INPUT | PULLUP | ONLOW);
#endif
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GT911 INT gpio%d release failed: %d\n",
             BOARD_GT911_INT_GPIO, ret);
      return ret;
    }

  return OK;
}

static int gt911_board_irq_attach(FAR const struct gt911_board_s *board,
                                  xcpt_t isr, FAR void *arg)
{
  int ret;

  UNUSED(board);

  /* GT911 holds INT low while a frame is ready.  The driver masks the
   * interrupt in its ISR and unmasks it after writing the 0x814e status
   * clear.  gpio_intr_disable() also clears the pending status bit, so a
   * falling edge that arrives inside that window would be lost.  Level-low
   * triggering re-asserts from the pin level instead, which makes the
   * unmask safe even when the controller still holds INT low.
   */

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_GT911_IRQ_EDGE
  ret = esp_configgpio(BOARD_GT911_INT_GPIO, INPUT | PULLUP | FALLING);
#else
  ret = esp_configgpio(BOARD_GT911_INT_GPIO, INPUT | PULLUP | ONLOW);
#endif
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GT911 INT gpio%d input setup failed: %d\n",
             BOARD_GT911_INT_GPIO, ret);
      return ret;
    }

  ret = esp_gpio_irq(BOARD_GT911_INT_GPIO, isr, arg);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GT911 INT gpio%d irq attach failed: %d\n",
             BOARD_GT911_INT_GPIO, ret);
    }

  return ret;
}

static void gt911_board_irq_enable(FAR const struct gt911_board_s *board,
                                   bool enable)
{
  UNUSED(board);

  if (enable)
    {
      esp_gpioirqenable(BOARD_GT911_INT_GPIO);
    }
  else
    {
      esp_gpioirqdisable(BOARD_GT911_INT_GPIO);
    }
}

static const struct gt911_board_s g_gt911_board =
{
  .reset      = gt911_board_reset,
  .irq_attach = gt911_board_irq_attach,
  .irq_enable = gt911_board_irq_enable,
};

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_GT911_IRQ */

/* Without board callbacks the driver falls back to its LPWORK polling path,
 * which is the only option on a stock board where INT_TP/RESET_TP are not
 * wired to the SoC.
 */

static const struct gt911_config_s g_gt911_configs[] =
{
  {
#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_GT911_IRQ
    .board              = &g_gt911_board,
    .poll_interval_ms   = 0,
#else
    .board              = NULL,
    .poll_interval_ms   = BOARD_GT911_POLL_INTERVAL_MS,
#endif
    .frequency          = BOARD_GT911_I2C_FREQUENCY,
    .address            = BOARD_GT911_I2C_ADDRESS,
    .x_resolution       = BOARD_GT911_X_RESOLUTION,
    .y_resolution       = BOARD_GT911_Y_RESOLUTION,
    .max_points         = BOARD_GT911_MAX_POINTS,
    .event_buffer_count = GT911_EVENT_BUFFER_COUNT,
    .swap_xy            = false,
    .invert_x           = true,
    .invert_y           = true,
  },
  {
#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_GT911_IRQ
    .board              = &g_gt911_board,
    .poll_interval_ms   = 0,
#else
    .board              = NULL,
    .poll_interval_ms   = BOARD_GT911_POLL_INTERVAL_MS,
#endif
    .frequency          = BOARD_GT911_I2C_FREQUENCY,
    .address            = BOARD_GT911_I2C_BACKUP_ADDRESS,
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

  /* On a stock board the GT911 reset line is not routed to the SoC, so the
   * controller's local supply and boot sequence must settle before the first
   * I2C product-ID transaction.  In interrupt mode the driver calls
   * board reset() itself, which already covers this delay.
   */

#ifndef CONFIG_ESP32P4_FUNCTION_EV_BOARD_GT911_IRQ
  syslog(LOG_INFO, "GT911 power-on wait: %u ms before product-ID probe\n",
         (unsigned int)(BOARD_GT911_POWER_ON_DELAY_US / 1000));
  ret = nxsig_usleep(BOARD_GT911_POWER_ON_DELAY_US);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GT911 power-on wait failed: %d\n", ret);
      esp_i2cbus_uninitialize(i2c);
      return ret;
    }
#endif

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
#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_GT911_IRQ
          syslog(LOG_INFO,
                 "GT911 registered at /dev/input0: I2C%d address=0x%02x "
                 "irq=gpio%d reset=gpio%d\n",
                 BOARD_GT911_I2C_BUS, config->address,
                 BOARD_GT911_INT_GPIO, BOARD_GT911_RESET_GPIO);
#else
          syslog(LOG_INFO,
                 "GT911 registered at /dev/input0: I2C%d address=0x%02x "
                 "poll=%ums\n",
                 BOARD_GT911_I2C_BUS, config->address,
                 BOARD_GT911_POLL_INTERVAL_MS);
#endif
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
