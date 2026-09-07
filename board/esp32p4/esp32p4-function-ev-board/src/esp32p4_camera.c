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

#include <nuttx/arch.h>
#include <nuttx/mutex.h>

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>

#include <arch/board/board.h>
#include <arch/chip/esp_mipi_csi.h>

#include "espressif/esp_i2c.h"

#include "esp32p4_sc2336.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BOARD_MIPI_CSI_PHY_LDO_CHANNEL 3
#define BOARD_MIPI_CSI_PHY_SETTLE_MS  10

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* P2 has one diagnostic camera session.  Keep the I2C reference from
 * profile programming through stream-off so csi_probe cannot race another
 * board user of the camera SCCB bus.
 */

static mutex_t g_sc2336_lock = NXMUTEX_INITIALIZER;
static FAR struct i2c_master_s *g_sc2336_i2c;
static bool g_sc2336_configured;
static bool g_sc2336_streaming;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int board_sc2336_release_locked(void)
{
  FAR struct i2c_master_s *i2c = g_sc2336_i2c;
  int ret = OK;
  int uninit_ret;

  if (i2c == NULL)
    {
      return OK;
    }

  if (g_sc2336_streaming)
    {
      ret = esp32p4_sc2336_set_stream(i2c, BOARD_SC2336_I2C_ADDRESS,
                                       BOARD_SC2336_I2C_FREQUENCY, false);
    }

  uninit_ret = esp_i2cbus_uninitialize(i2c);
  g_sc2336_i2c = NULL;
  g_sc2336_configured = false;
  g_sc2336_streaming = false;

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

static int board_sc2336_csi_get_profile(
  FAR struct esp_mipi_csi_config_s *config)
{
  if (config == NULL)
    {
      return -EINVAL;
    }

  memset(config, 0, sizeof(*config));
  config->lane_num = SC2336_RAW8_LANE_NUM;
  config->data_type = SC2336_RAW8_DATA_TYPE;
  config->bits_per_pixel = SC2336_RAW8_BITS_PER_PIXEL;
  config->width = SC2336_RAW8_WIDTH;
  config->height = SC2336_RAW8_HEIGHT;
  config->lane_bit_rate_mbps = SC2336_RAW8_LANE_RATE_MBPS;
  config->byte_swap = false;
  config->phy_ldo.channel_id = BOARD_MIPI_CSI_PHY_LDO_CHANNEL;
  config->phy_ldo.voltage_mv = ESP_MIPI_CSI_DPHY_VOLTAGE_MV;
  return OK;
}

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

int board_sc2336_sccb_read_write_test(FAR uint16_t *first_product_id,
                                      FAR uint16_t *second_product_id)
{
  int release_ret;
  int ret;

  if (first_product_id == NULL || second_product_id == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_sc2336_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_sc2336_i2c != NULL)
    {
      ret = -EBUSY;
      goto out;
    }

  syslog(LOG_INFO,
         "SC2336 SCCB diagnostic: stage=i2c_initialize bus=%d\n",
         BOARD_SC2336_I2C_BUS);
  g_sc2336_i2c = esp_i2cbus_initialize(BOARD_SC2336_I2C_BUS);
  if (g_sc2336_i2c == NULL)
    {
      ret = -ENODEV;
      syslog(LOG_ERR,
             "ERROR: SC2336 SCCB diagnostic: stage=i2c_initialize "
             "result=%d\n", ret);
      goto out;
    }

  syslog(LOG_INFO,
         "SC2336 SCCB diagnostic: stage=i2c_initialize result=%d\n", OK);
  syslog(LOG_INFO,
         "SC2336 SCCB diagnostic: stage=first_product_id\n");
  ret = esp32p4_sc2336_probe(g_sc2336_i2c, BOARD_SC2336_I2C_ADDRESS,
                             BOARD_SC2336_I2C_FREQUENCY,
                             first_product_id);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: SC2336 SCCB diagnostic: stage=first_product_id "
             "result=%d\n", ret);
      goto release;
    }

  syslog(LOG_INFO,
         "SC2336 SCCB diagnostic: stage=first_product_id result=%d "
         "product_id=0x%04x\n", ret, *first_product_id);
  syslog(LOG_INFO,
         "SC2336 SCCB diagnostic: stage=stream_off register=0x0100 "
         "value=0x00\n");
  ret = esp32p4_sc2336_set_stream(g_sc2336_i2c, BOARD_SC2336_I2C_ADDRESS,
                                   BOARD_SC2336_I2C_FREQUENCY, false);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: SC2336 SCCB diagnostic: stage=stream_off result=%d\n",
             ret);
      goto release;
    }

  syslog(LOG_INFO,
         "SC2336 SCCB diagnostic: stage=stream_off result=%d\n", ret);
  syslog(LOG_INFO,
         "SC2336 SCCB diagnostic: stage=second_product_id\n");
  ret = esp32p4_sc2336_probe(g_sc2336_i2c, BOARD_SC2336_I2C_ADDRESS,
                             BOARD_SC2336_I2C_FREQUENCY,
                             second_product_id);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: SC2336 SCCB diagnostic: stage=second_product_id "
             "result=%d\n", ret);
      goto release;
    }

  syslog(LOG_INFO,
         "SC2336 SCCB diagnostic: result=%d first_product_id=0x%04x "
         "second_product_id=0x%04x\n", ret, *first_product_id,
         *second_product_id);
  goto release;

release:
  release_ret = board_sc2336_release_locked();

  if (ret >= 0 && release_ret < 0)
    {
      ret = release_ret;
    }

out:
  nxmutex_unlock(&g_sc2336_lock);
  return ret;
}

int board_sc2336_sccb_pointer_test(void)
{
  int release_ret;
  int ret;

  ret = nxmutex_lock(&g_sc2336_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_sc2336_i2c != NULL)
    {
      ret = -EBUSY;
      goto out;
    }

  syslog(LOG_INFO,
         "SC2336 pointer diagnostic: stage=i2c_initialize bus=%d\n",
         BOARD_SC2336_I2C_BUS);
  g_sc2336_i2c = esp_i2cbus_initialize(BOARD_SC2336_I2C_BUS);
  if (g_sc2336_i2c == NULL)
    {
      ret = -ENODEV;
      syslog(LOG_ERR,
             "ERROR: SC2336 pointer diagnostic: stage=i2c_initialize "
             "result=%d\n", ret);
      goto out;
    }

  syslog(LOG_INFO,
         "SC2336 pointer diagnostic: stage=i2c_initialize result=%d\n",
         OK);
  syslog(LOG_INFO,
         "SC2336 pointer diagnostic: stage=register_address reg=0x%04x\n",
         SC2336_PRODUCT_ID_HIGH_REG);
  ret = esp32p4_sc2336_write_register_pointer(
    g_sc2336_i2c, BOARD_SC2336_I2C_ADDRESS,
    BOARD_SC2336_I2C_FREQUENCY, SC2336_PRODUCT_ID_HIGH_REG);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: SC2336 pointer diagnostic: stage=register_address "
             "result=%d\n", ret);
      goto release;
    }

  syslog(LOG_INFO,
         "SC2336 pointer diagnostic: stage=register_address result=%d\n",
         ret);

release:
  release_ret = board_sc2336_release_locked();
  if (ret >= 0 && release_ret < 0)
    {
      ret = release_ret;
    }

out:
  nxmutex_unlock(&g_sc2336_lock);
  return ret;
}

int board_sc2336_sccb_split_test(FAR uint8_t *value)
{
  int release_ret;
  int ret;

  if (value == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_sc2336_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_sc2336_i2c != NULL)
    {
      ret = -EBUSY;
      goto out;
    }

  syslog(LOG_INFO,
         "SC2336 split diagnostic: stage=i2c_initialize bus=%d\n",
         BOARD_SC2336_I2C_BUS);
  g_sc2336_i2c = esp_i2cbus_initialize(BOARD_SC2336_I2C_BUS);
  if (g_sc2336_i2c == NULL)
    {
      ret = -ENODEV;
      syslog(LOG_ERR,
             "ERROR: SC2336 split diagnostic: stage=i2c_initialize "
             "result=%d\n", ret);
      goto out;
    }

  syslog(LOG_INFO,
         "SC2336 split diagnostic: stage=i2c_initialize result=%d\n", OK);
  syslog(LOG_INFO,
         "SC2336 split diagnostic: stage=pointer_write reg=0x%04x\n",
         SC2336_PRODUCT_ID_HIGH_REG);
  ret = esp32p4_sc2336_write_register_pointer(
    g_sc2336_i2c, BOARD_SC2336_I2C_ADDRESS,
    BOARD_SC2336_I2C_FREQUENCY, SC2336_PRODUCT_ID_HIGH_REG);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: SC2336 split diagnostic: stage=pointer_write "
             "result=%d\n",
             ret);
      goto release;
    }

  syslog(LOG_INFO,
         "SC2336 split diagnostic: stage=pointer_write result=%d\n", ret);
  syslog(LOG_INFO, "SC2336 split diagnostic: stage=read_data\n");
  ret = esp32p4_sc2336_read_register_data(
    g_sc2336_i2c, BOARD_SC2336_I2C_ADDRESS,
    BOARD_SC2336_I2C_FREQUENCY, value);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: SC2336 split diagnostic: stage=read_data result=%d\n",
             ret);
      goto release;
    }

  syslog(LOG_INFO,
         "SC2336 split diagnostic: stage=read_data result=%d value=0x%02x\n",
         ret, *value);
  if (*value != (uint8_t)(SC2336_PRODUCT_ID >> 8))
    {
      ret = -ENODEV;
      syslog(LOG_ERR,
             "ERROR: SC2336 split diagnostic: stage=validate expected=0x%02x "
             "actual=0x%02x result=%d\n",
             (unsigned int)(SC2336_PRODUCT_ID >> 8), *value, ret);
    }

release:
  release_ret = board_sc2336_release_locked();
  if (ret >= 0 && release_ret < 0)
    {
      ret = release_ret;
    }

out:
  nxmutex_unlock(&g_sc2336_lock);
  return ret;
}

int board_sc2336_sccb_write_test(FAR uint16_t *product_id)
{
  int release_ret;
  int ret;

  if (product_id == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_sc2336_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_sc2336_i2c != NULL)
    {
      ret = -EBUSY;
      goto out;
    }

  syslog(LOG_INFO,
         "SC2336 write diagnostic: stage=i2c_initialize bus=%d\n",
         BOARD_SC2336_I2C_BUS);
  g_sc2336_i2c = esp_i2cbus_initialize(BOARD_SC2336_I2C_BUS);
  if (g_sc2336_i2c == NULL)
    {
      ret = -ENODEV;
      syslog(LOG_ERR,
             "ERROR: SC2336 write diagnostic: stage=i2c_initialize "
             "result=%d\n", ret);
      goto out;
    }

  syslog(LOG_INFO,
         "SC2336 write diagnostic: stage=i2c_initialize result=%d\n", OK);
  syslog(LOG_INFO,
         "SC2336 write diagnostic: stage=soft_reset register=0x0103 "
         "value=0x01\n");
  ret = esp32p4_sc2336_soft_reset(g_sc2336_i2c,
                                  BOARD_SC2336_I2C_ADDRESS,
                                  BOARD_SC2336_I2C_FREQUENCY);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: SC2336 write diagnostic: stage=soft_reset result=%d\n",
             ret);
      goto release;
    }

  syslog(LOG_INFO,
         "SC2336 write diagnostic: stage=soft_reset result=%d\n", ret);
  syslog(LOG_INFO,
         "SC2336 write diagnostic: stage=stream_off register=0x0100 "
         "value=0x00\n");
  ret = esp32p4_sc2336_set_stream(g_sc2336_i2c,
                                   BOARD_SC2336_I2C_ADDRESS,
                                   BOARD_SC2336_I2C_FREQUENCY, false);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: SC2336 write diagnostic: stage=stream_off result=%d\n",
             ret);
      goto release;
    }

  syslog(LOG_INFO,
         "SC2336 write diagnostic: stage=stream_off result=%d\n", ret);
  syslog(LOG_INFO,
         "SC2336 write diagnostic: stage=product_id\n");
  ret = esp32p4_sc2336_probe(g_sc2336_i2c, BOARD_SC2336_I2C_ADDRESS,
                             BOARD_SC2336_I2C_FREQUENCY, product_id);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: SC2336 write diagnostic: stage=product_id result=%d\n",
             ret);
      goto release;
    }

  syslog(LOG_INFO,
         "SC2336 write diagnostic: stage=product_id result=%d "
         "product_id=0x%04x\n", ret, *product_id);
  goto release;

release:
  release_ret = board_sc2336_release_locked();
  if (ret >= 0 && release_ret < 0)
    {
      ret = release_ret;
    }

out:
  nxmutex_unlock(&g_sc2336_lock);
  return ret;
}

int board_sc2336_csi_power_acquire(
  FAR struct esp_mipi_csi_config_s *config,
  FAR struct esp_mipi_csi_s **csi)
{
  int ret;

  ret = board_sc2336_csi_get_profile(config);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_mipi_csi_power_acquire(config, csi);
  if (ret < 0)
    {
      return ret;
    }

  /* Match the ESP-IDF camera baseline: let the MIPI D-PHY LDO settle before
   * configuring the SCCB GPIOs or initializing the I2C controller.
   */

  up_mdelay(BOARD_MIPI_CSI_PHY_SETTLE_MS);
  return OK;
}

int board_sc2336_csi_initialize(FAR struct esp_mipi_csi_s *csi,
  FAR const struct esp_mipi_csi_config_s *config)
{
  return esp_mipi_csi_initialize(csi, config);
}

int board_sc2336_csi_prepare(FAR uint16_t *product_id)
{
  int ret;

  if (product_id == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_sc2336_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_sc2336_i2c != NULL)
    {
      ret = -EBUSY;
      goto out;
    }

  syslog(LOG_INFO, "SC2336 CSI prepare: stage=i2c_initialize bus=%d\n",
         BOARD_SC2336_I2C_BUS);

  g_sc2336_i2c = esp_i2cbus_initialize(BOARD_SC2336_I2C_BUS);
  if (g_sc2336_i2c == NULL)
    {
      ret = -ENODEV;
      syslog(LOG_ERR,
             "ERROR: SC2336 CSI prepare: stage=i2c_initialize result=%d\n",
             ret);
      goto out;
    }

  syslog(LOG_INFO,
         "SC2336 CSI prepare: stage=i2c_initialize result=%d\n", OK);
  syslog(LOG_INFO,
         "SC2336 CSI prepare: stage=product_id address=0x%02x "
         "frequency=%lu\n", BOARD_SC2336_I2C_ADDRESS,
         (unsigned long)BOARD_SC2336_I2C_FREQUENCY);

  ret = esp32p4_sc2336_probe(g_sc2336_i2c, BOARD_SC2336_I2C_ADDRESS,
                             BOARD_SC2336_I2C_FREQUENCY, product_id);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: SC2336 CSI prepare: stage=product_id result=%d\n",
             ret);
      goto errout;
    }

  syslog(LOG_INFO,
         "SC2336 CSI prepare: stage=product_id result=%d product_id=0x%04x\n",
         ret, *product_id);
  syslog(LOG_INFO,
         "SC2336 CSI prepare: stage=profile_configure raw8=%ux%u "
         "lanes=%u dt=0x%02x\n", SC2336_RAW8_WIDTH,
         SC2336_RAW8_HEIGHT, SC2336_RAW8_LANE_NUM,
         SC2336_RAW8_DATA_TYPE);

  ret = esp32p4_sc2336_configure_raw8(g_sc2336_i2c,
                                       BOARD_SC2336_I2C_ADDRESS,
                                       BOARD_SC2336_I2C_FREQUENCY);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: SC2336 CSI prepare: stage=profile_configure result=%d\n",
             ret);
      goto errout;
    }

  g_sc2336_configured = true;

  syslog(LOG_INFO,
         "SC2336 CSI prepare: stage=profile_configure result=%d\n", ret);
  syslog(LOG_INFO,
         "SC2336 CSI profile ready: %u lane(s), dt=0x%02x, "
         "%ux%u RAW%u @%u Mbps/lane\n",
         SC2336_RAW8_LANE_NUM, SC2336_RAW8_DATA_TYPE,
         SC2336_RAW8_WIDTH, SC2336_RAW8_HEIGHT,
         SC2336_RAW8_BITS_PER_PIXEL, SC2336_RAW8_LANE_RATE_MBPS);
  ret = OK;
  goto out;

errout:
  board_sc2336_release_locked();
out:
  nxmutex_unlock(&g_sc2336_lock);
  return ret;
}

int board_sc2336_csi_deinitialize(FAR struct esp_mipi_csi_s *csi)
{
  return esp_mipi_csi_deinitialize(csi);
}

int board_sc2336_csi_power_release(FAR struct esp_mipi_csi_s *csi)
{
  return esp_mipi_csi_power_release(csi);
}

int board_sc2336_csi_set_stream(bool enable)
{
  int ret;

  ret = nxmutex_lock(&g_sc2336_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_sc2336_configured || g_sc2336_i2c == NULL)
    {
      ret = -EPIPE;
    }
  else if (g_sc2336_streaming == enable)
    {
      ret = OK;
    }
  else
    {
      ret = esp32p4_sc2336_set_stream(g_sc2336_i2c,
                                       BOARD_SC2336_I2C_ADDRESS,
                                       BOARD_SC2336_I2C_FREQUENCY, enable);
      if (ret >= 0)
        {
          g_sc2336_streaming = enable;
        }
    }

  nxmutex_unlock(&g_sc2336_lock);
  return ret;
}

int board_sc2336_csi_release(void)
{
  int ret;

  ret = nxmutex_lock(&g_sc2336_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = board_sc2336_release_locked();
  nxmutex_unlock(&g_sc2336_lock);
  return ret;
}
