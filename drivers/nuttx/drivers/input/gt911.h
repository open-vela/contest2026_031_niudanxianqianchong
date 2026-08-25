/****************************************************************************
 * drivers/input/gt911.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Goodix GT911 capacitive touch controller interface.
 ****************************************************************************/

#ifndef __DRIVERS_INPUT_GT911_H
#define __DRIVERS_INPUT_GT911_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/compiler.h>
#include <nuttx/irq.h>

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define GT911_I2C_ADDRESS             0x5d
#define GT911_I2C_FREQUENCY           400000
#define GT911_MAX_POINTS               5
#define GT911_EVENT_BUFFER_COUNT       8
#define GT911_POLL_INTERVAL_MS         20

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct i2c_master_s;
struct gt911_board_s;

/* The board owns the GPIO electrical sequence.  In particular, GT911 I2C
 * address selection is sampled during reset, so reset() must drive both the
 * reset and INT pins according to the board wiring before releasing reset.
 * irq_attach() and irq_enable() are optional as a pair.  When they are not
 * supplied, poll_interval_ms must be non-zero.
 */

struct gt911_board_s
{
  int (*reset)(FAR const struct gt911_board_s *board);
  int (*irq_attach)(FAR const struct gt911_board_s *board, xcpt_t isr,
                    FAR void *arg);
  void (*irq_enable)(FAR const struct gt911_board_s *board, bool enable);
};

/* The configuration object must remain valid for the registered driver's
 * lifetime.  address, frequency, max_points and event_buffer_count accept
 * zero to select the GT911 defaults above.  x_resolution and y_resolution
 * select the exported coordinate range when non-zero.  The axis flags are
 * then applied to that range.  poll_interval_ms selects polling mode when
 * non-zero; zero selects interrupt mode.
 */

struct gt911_config_s
{
  FAR const struct gt911_board_s *board;
  uint32_t frequency;
  uint16_t address;
  uint16_t poll_interval_ms;
  uint16_t x_resolution;
  uint16_t y_resolution;
  uint8_t max_points;
  uint8_t event_buffer_count;
  bool swap_xy;
  bool invert_x;
  bool invert_y;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: gt911_register
 *
 * Description:
 *   Register a GT911 touchscreen lower half at devpath.  The caller must
 *   provide a configured I2C bus and a static configuration object.
 *
 * Input Parameters:
 *   devpath - Touch input device path, for example "/dev/input0"
 *   i2c     - Initialized I2C master
 *   config  - Generic and board-specific configuration
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 ****************************************************************************/

int gt911_register(FAR const char *devpath,
                   FAR struct i2c_master_s *i2c,
                   FAR const struct gt911_config_s *config);

#endif /* __DRIVERS_INPUT_GT911_H */
