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

#include <debug.h>
#include <errno.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>

#include "esp32p4_sc2336.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int sc2336_read_register(FAR struct i2c_master_s *i2c,
                                uint8_t address, uint32_t frequency,
                                uint16_t reg, FAR uint8_t *value)
{
  struct i2c_msg_s msg[2];
  uint8_t regbuf[2];
  int ret;

  DEBUGASSERT(i2c != NULL && value != NULL);

  regbuf[0] = (uint8_t)(reg >> 8);
  regbuf[1] = (uint8_t)reg;

  msg[0].frequency = frequency;
  msg[0].addr      = address;
  msg[0].flags     = 0;
  msg[0].buffer    = regbuf;
  msg[0].length    = sizeof(regbuf);

  msg[1].frequency = frequency;
  msg[1].addr      = address;
  msg[1].flags     = I2C_M_READ;
  msg[1].buffer    = value;
  msg[1].length    = 1;

  ret = I2C_TRANSFER(i2c, msg, 2);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: SC2336 SCCB read failed: address=0x%02x "
             "frequency=%lu reg=0x%04x ret=%d\n",
             address, (unsigned long)frequency, reg, ret);
      return ret;
    }

  return OK;
}

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
