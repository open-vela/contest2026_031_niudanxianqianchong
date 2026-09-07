/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_sc2336.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board-private SC2336 SCCB product-ID probe.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/arch.h>
#include <debug.h>
#include <errno.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>

#include "esp32p4_sc2336.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SC2336_REG_END                 0xffff
#define SC2336_REG_DELAY               0xfffe
#define SC2336_REG_SOFT_RESET          0x0103
#define SC2336_REG_STREAM              0x0100
#define SC2336_SCCB_RETRY_COUNT         3
#define SC2336_SCCB_RETRY_DELAY_MS      5

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sc2336_regval_s
{
  uint16_t reg;
  uint8_t value;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void sc2336_recover_i2c(FAR struct i2c_master_s *i2c);

static int sc2336_read_register(FAR struct i2c_master_s *i2c,
                                uint8_t address, uint32_t frequency,
                                uint16_t reg, FAR uint8_t *value)
{
  struct i2c_msg_s msg[2];
  uint8_t regbuf[2];
  unsigned int attempt;
  int ret;

  DEBUGASSERT(i2c != NULL && value != NULL);

  regbuf[0] = (uint8_t)(reg >> 8);
  regbuf[1] = (uint8_t)reg;

  msg[0].frequency = frequency;
  msg[0].addr      = address;
  /* Use the standard NuttX two-message register read sequence.  The P4
   * controller's repeated-START path is diagnosed separately; SC2336
   * probing must not depend on that unverified path.
   */

  msg[0].flags     = 0;
  msg[0].buffer    = regbuf;
  msg[0].length    = sizeof(regbuf);

  msg[1].frequency = frequency;
  msg[1].addr      = address;
  msg[1].flags     = I2C_M_READ;
  msg[1].buffer    = value;
  msg[1].length    = 1;

  for (attempt = 0; attempt < SC2336_SCCB_RETRY_COUNT; attempt++)
    {
      ret = I2C_TRANSFER(i2c, msg, 2);
      if (ret >= 0)
        {
          return OK;
        }

      if (attempt + 1 < SC2336_SCCB_RETRY_COUNT)
        {
          /* A NACK may leave the controller state machine or physical bus
           * unusable for the next transaction.  Use the standard NuttX I2C
           * recovery operation before retrying.
           */

          sc2336_recover_i2c(i2c);
          syslog(LOG_WARNING,
                 "WARNING: SC2336 SCCB read retry: address=0x%02x "
                 "reg=0x%04x attempt=%u ret=%d\n",
                 address, reg, attempt + 1, ret);
          up_mdelay(SC2336_SCCB_RETRY_DELAY_MS);
        }
    }

  syslog(LOG_ERR,
         "ERROR: SC2336 SCCB read failed: address=0x%02x "
         "frequency=%lu reg=0x%04x attempts=%u ret=%d\n",
         address, (unsigned long)frequency, reg, attempt, ret);
  return ret;
}

static void sc2336_recover_i2c(FAR struct i2c_master_s *i2c)
{
#ifdef CONFIG_I2C_RESET
  int ret;

  ret = I2C_RESET(i2c);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "WARNING: SC2336 SCCB I2C bus reset failed: %d\n",
             ret);
    }
  else
    {
      syslog(LOG_WARNING,
             "WARNING: SC2336 SCCB I2C bus reset completed before retry\n");
    }
#else
  (void)i2c;
#endif
}

static int sc2336_write_register(FAR struct i2c_master_s *i2c,
                                 uint8_t address, uint32_t frequency,
                                 uint16_t reg, uint8_t value)
{
  struct i2c_msg_s msg;
  uint8_t buffer[3];
  unsigned int attempt;
  int ret;

  DEBUGASSERT(i2c != NULL);

  buffer[0] = (uint8_t)(reg >> 8);
  buffer[1] = (uint8_t)reg;
  buffer[2] = value;

  msg.frequency = frequency;
  msg.addr      = address;
  msg.flags     = 0;
  msg.buffer    = buffer;
  msg.length    = sizeof(buffer);

  for (attempt = 0; attempt < SC2336_SCCB_RETRY_COUNT; attempt++)
    {
      ret = I2C_TRANSFER(i2c, &msg, 1);
      if (ret >= 0)
        {
          return OK;
        }

      if (attempt + 1 < SC2336_SCCB_RETRY_COUNT)
        {
          /* A NACK may leave the controller state machine or physical bus
           * unusable for the next transaction.  Use the standard NuttX I2C
           * recovery operation before retrying.
           */

          sc2336_recover_i2c(i2c);
          syslog(LOG_WARNING,
                 "WARNING: SC2336 SCCB write retry: address=0x%02x "
                 "reg=0x%04x attempt=%u ret=%d\n",
                 address, reg, attempt + 1, ret);
          up_mdelay(SC2336_SCCB_RETRY_DELAY_MS);
        }
    }

  syslog(LOG_ERR,
         "ERROR: SC2336 SCCB write failed: address=0x%02x "
         "frequency=%lu reg=0x%04x value=0x%02x attempts=%u ret=%d\n",
         address, (unsigned long)frequency, reg, value, attempt, ret);
  return ret;
}

static int sc2336_write_array(FAR struct i2c_master_s *i2c,
                              uint8_t address, uint32_t frequency,
                              FAR const struct sc2336_regval_s *regs)
{
  FAR const struct sc2336_regval_s *entry;
  int ret;

  if (i2c == NULL || regs == NULL)
    {
      return -EINVAL;
    }

  for (entry = regs; entry->reg != SC2336_REG_END; entry++)
    {
      if (entry->reg == SC2336_REG_DELAY)
        {
          up_mdelay(entry->value);
          continue;
        }

      ret = sc2336_write_register(i2c, address, frequency, entry->reg,
                                   entry->value);
      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}

/* SC2336 24MHz-input, 2-lane, 288Mbps/lane RAW8 1024x600@30fps profile.
 *
 * Source: Espressif esp_cam_sensor v1.7.0, Apache-2.0,
 * sensors/sc2336/private_include/sc2336_settings.h
 * (init_reglist_MIPI_2lane_1024x600_raw8_30fps).  The explicit 5ms delay
 * after software reset makes the sequence safe on NuttX's faster SCCB path.
 */

static const struct sc2336_regval_s g_sc2336_raw8_1024x600[] =
{
  {SC2336_REG_SOFT_RESET, 0x01}, {SC2336_REG_DELAY, 5},
  {SC2336_REG_STREAM, 0x00}, {0x36e9, 0x80}, {0x37f9, 0x80},
  {0x301f, 0xc7}, {0x3031, 0x08}, {0x3037, 0x00}, {0x3106, 0x05},
  {0x3200, 0x01}, {0x3201, 0xb4}, {0x3202, 0x00}, {0x3203, 0xf0},
  {0x3204, 0x05}, {0x3205, 0xd3}, {0x3206, 0x03}, {0x3207, 0x4f},
  {0x3208, 0x04}, {0x3209, 0x00}, {0x320a, 0x02}, {0x320b, 0x58},
  {0x320c, 0x09}, {0x320d, 0x60}, {0x320e, 0x03}, {0x320f, 0xe8},
  {0x3210, 0x00}, {0x3211, 0x10}, {0x3212, 0x00}, {0x3213, 0x04},
  {0x3248, 0x04}, {0x3249, 0x0b}, {0x3253, 0x08}, {0x3301, 0x09},
  {0x3302, 0xff}, {0x3303, 0x10}, {0x3306, 0x60}, {0x3307, 0x02},
  {0x330a, 0x01}, {0x330b, 0x10}, {0x330c, 0x16}, {0x330d, 0xff},
  {0x3318, 0x02}, {0x3321, 0x0a}, {0x3327, 0x0e}, {0x332b, 0x12},
  {0x3333, 0x10}, {0x3334, 0x40}, {0x335e, 0x06}, {0x335f, 0x0a},
  {0x3364, 0x1f}, {0x337c, 0x02}, {0x337d, 0x0e}, {0x3390, 0x09},
  {0x3391, 0x0f}, {0x3392, 0x1f}, {0x3393, 0x20}, {0x3394, 0x20},
  {0x3395, 0xff}, {0x33a2, 0x04}, {0x33b1, 0x80}, {0x33b2, 0x68},
  {0x33b3, 0x42}, {0x33f9, 0x78}, {0x33fb, 0xd8}, {0x33fc, 0x0f},
  {0x33fd, 0x1f}, {0x349f, 0x03}, {0x34a6, 0x0f}, {0x34a7, 0x1f},
  {0x34a8, 0x42}, {0x34a9, 0x06}, {0x34aa, 0x01}, {0x34ab, 0x28},
  {0x34ac, 0x01}, {0x34ad, 0x90}, {0x3630, 0xf4}, {0x3633, 0x22},
  {0x3639, 0xf4}, {0x363c, 0x47}, {0x3670, 0x09}, {0x3674, 0xf4},
  {0x3675, 0xfb}, {0x3676, 0xed}, {0x367c, 0x09}, {0x367d, 0x0f},
  {0x3690, 0x22}, {0x3691, 0x22}, {0x3692, 0x22}, {0x3698, 0x89},
  {0x3699, 0x96}, {0x369a, 0xd0}, {0x369b, 0xd0}, {0x369c, 0x09},
  {0x369d, 0x0f}, {0x36a2, 0x09}, {0x36a3, 0x0f}, {0x36a4, 0x1f},
  {0x36d0, 0x01}, {0x36ea, 0x08}, {0x36eb, 0x0a}, {0x36ec, 0x1a},
  {0x36ed, 0x18}, {0x3722, 0xe1}, {0x3724, 0x41}, {0x3725, 0xc1},
  {0x3728, 0x20}, {0x37fa, 0x08}, {0x37fb, 0x32}, {0x37fc, 0x11},
  {0x37fd, 0x37}, {0x3900, 0x0d}, {0x3905, 0x98}, {0x391b, 0x81},
  {0x391c, 0x10}, {0x3933, 0x81}, {0x3934, 0xc5}, {0x3940, 0x68},
  {0x3941, 0x00}, {0x3942, 0x01}, {0x3943, 0xc6}, {0x3952, 0x02},
  {0x3953, 0x0f}, {0x3e01, 0x37}, {0x3e02, 0xe0}, {0x3e08, 0x1f},
  {0x3e1b, 0x14}, {0x4509, 0x38}, {0x4819, 0x05}, {0x481b, 0x03},
  {0x481d, 0x0a}, {0x481f, 0x02}, {0x4821, 0x08}, {0x4823, 0x03},
  {0x4825, 0x02}, {0x4827, 0x03}, {0x4829, 0x04}, {0x5799, 0x06},
  {0x5ae0, 0xfe}, {0x5ae1, 0x40}, {0x5ae2, 0x30}, {0x5ae3, 0x28},
  {0x5ae4, 0x20}, {0x5ae5, 0x30}, {0x5ae6, 0x28}, {0x5ae7, 0x20},
  {0x5ae8, 0x3c}, {0x5ae9, 0x30}, {0x5aea, 0x28}, {0x5aeb, 0x3c},
  {0x5aec, 0x30}, {0x5aed, 0x28}, {0x5aee, 0xfe}, {0x5aef, 0x40},
  {0x5af4, 0x30}, {0x5af5, 0x28}, {0x5af6, 0x20}, {0x5af7, 0x30},
  {0x5af8, 0x28}, {0x5af9, 0x20}, {0x5afa, 0x3c}, {0x5afb, 0x30},
  {0x5afc, 0x28}, {0x5afd, 0x3c}, {0x5afe, 0x30}, {0x5aff, 0x28},
  {0x36e9, 0x53}, {0x37f9, 0x53}, {SC2336_REG_END, 0x00},
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp32p4_sc2336_probe(FAR struct i2c_master_s *i2c,
                         uint8_t address, uint32_t frequency,
                         FAR uint16_t *product_id)
{
  uint16_t id;
  uint8_t high;
  uint8_t low;
  int ret;

  if (i2c == NULL || product_id == NULL)
    {
      return -EINVAL;
    }

  ret = sc2336_read_register(i2c, address, frequency,
                             SC2336_PRODUCT_ID_HIGH_REG, &high);
  if (ret < 0)
    {
      return ret;
    }

  ret = sc2336_read_register(i2c, address, frequency,
                             SC2336_PRODUCT_ID_LOW_REG, &low);
  if (ret < 0)
    {
      return ret;
    }

  id = ((uint16_t)high << 8) | low;
  *product_id = id;

  if (id != SC2336_PRODUCT_ID)
    {
      syslog(LOG_ERR,
             "ERROR: SC2336 product ID mismatch: expected=0x%04x "
             "actual=0x%04x\n",
             SC2336_PRODUCT_ID, id);
      return -ENODEV;
    }

  syslog(LOG_INFO,
         "SC2336 detected: address=0x%02x product_id=0x%04x\n",
         address, id);
  return OK;
}

int esp32p4_sc2336_write_register_pointer(
  FAR struct i2c_master_s *i2c, uint8_t address, uint32_t frequency,
  uint16_t reg)
{
  struct i2c_msg_s msg;
  uint8_t regbuf[2];

  if (i2c == NULL)
    {
      return -EINVAL;
    }

  regbuf[0] = (uint8_t)(reg >> 8);
  regbuf[1] = (uint8_t)reg;

  msg.frequency = frequency;
  msg.addr      = address;
  msg.flags     = 0;
  msg.buffer    = regbuf;
  msg.length    = sizeof(regbuf);

  return I2C_TRANSFER(i2c, &msg, 1);
}

int esp32p4_sc2336_read_register_data(
  FAR struct i2c_master_s *i2c, uint8_t address, uint32_t frequency,
  FAR uint8_t *value)
{
  struct i2c_msg_s msg;

  if (i2c == NULL || value == NULL)
    {
      return -EINVAL;
    }

  msg.frequency = frequency;
  msg.addr      = address;
  msg.flags     = I2C_M_READ;
  msg.buffer    = value;
  msg.length    = 1;

  return I2C_TRANSFER(i2c, &msg, 1);
}

int esp32p4_sc2336_configure_raw8(FAR struct i2c_master_s *i2c,
                                  uint8_t address, uint32_t frequency)
{
  int ret;

  ret = sc2336_write_array(i2c, address, frequency,
                           g_sc2336_raw8_1024x600);
  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_INFO,
         "SC2336 profile configured: RAW8 %dx%d %dfps, "
         "%d-lane %dMbps\n",
         SC2336_RAW8_WIDTH, SC2336_RAW8_HEIGHT, SC2336_RAW8_FPS,
         SC2336_RAW8_LANE_NUM, SC2336_RAW8_LANE_RATE_MBPS);
  return OK;
}

int esp32p4_sc2336_soft_reset(FAR struct i2c_master_s *i2c,
                              uint8_t address, uint32_t frequency)
{
  int ret;

  if (i2c == NULL)
    {
      return -EINVAL;
    }

  ret = sc2336_write_register(i2c, address, frequency,
                              SC2336_REG_SOFT_RESET, 0x01);
  if (ret >= 0)
    {
      up_mdelay(SC2336_SCCB_RETRY_DELAY_MS);
    }

  return ret;
}

int esp32p4_sc2336_set_stream(FAR struct i2c_master_s *i2c,
                               uint8_t address, uint32_t frequency,
                               bool enable)
{
  int ret;

  if (i2c == NULL)
    {
      return -EINVAL;
    }

  ret = sc2336_write_register(i2c, address, frequency,
                              SC2336_REG_STREAM, enable ? 0x01 : 0x00);
  if (ret >= 0)
    {
      syslog(LOG_INFO, "SC2336 stream %s\n", enable ? "on" : "off");
    }

  return ret;
}
