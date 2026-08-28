/****************************************************************************
 * drivers/input/gt911.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Goodix GT911 capacitive touch controller driver.
 *
 * The driver uses the NuttX touchscreen upper half and supports either an
 * interrupt-driven worker or a polling worker.  I2C transactions always run
 * in LPWORK/thread context; the optional GPIO ISR only queues work.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <string.h>
#include <syslog.h>

#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
#  include <stdio.h>
#endif

#include <debug.h>

#include <nuttx/clock.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>

#include "gt911.h"

#if defined(CONFIG_INPUT_GT911) && defined(CONFIG_I2C)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define GT911_REG_PRODUCT_ID          0x8140
#define GT911_REG_COORD_STATUS         0x814e
/* 0x814e is the coordinate status byte.  The first contact record starts
 * immediately after it at 0x814f: track ID, X, Y, size, and reserved byte.
 */

#define GT911_REG_POINT1               0x814f

#define GT911_STATUS_READY             (1 << 7)
#define GT911_STATUS_TOUCH_MASK        0x0f
#define GT911_TRACK_ID_MASK            0x0f
#define GT911_TRACK_ID_COUNT           16

#define GT911_POINT_BYTES              8
#define GT911_MAX_DATA_BYTES           (GT911_MAX_POINTS * GT911_POINT_BYTES)

#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
#  define GT911_DIAGNOSTIC_INTERVAL_SCANS  50
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct gt911_contact_s
{
  int16_t x;
  int16_t y;
  int16_t h;
  int16_t w;
  bool active;
};

struct gt911_dev_s
{
  struct touch_lowerhalf_s lower;
  FAR struct i2c_master_s *i2c;
  FAR const struct gt911_config_s *config;
  FAR const struct gt911_board_s *board;
  struct work_s work;
  struct wdog_s poll_timer;
  mutex_t lock;
  uint32_t frequency;
  uint16_t address;
  uint16_t poll_interval_ms;
  uint8_t max_points;
  bool polling;
  struct gt911_contact_s contacts[GT911_TRACK_ID_COUNT];
#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
  uint32_t diagnostic_scans;
  uint32_t diagnostic_queued;
  uint32_t diagnostic_queue_errors;
  uint32_t diagnostic_ready;
  uint32_t diagnostic_frames;
  uint32_t diagnostic_i2c_errors;
  uint32_t diagnostic_last_report_scan;
  int diagnostic_last_error;
  uint8_t diagnostic_last_status;
  uint8_t diagnostic_last_points;
  uint8_t diagnostic_last_frame_status;
  uint8_t diagnostic_last_frame_len;
  uint8_t diagnostic_last_frame[GT911_MAX_DATA_BYTES];
  uint32_t diagnostic_last_ack_report_scan;
  bool diagnostic_last_frame_valid;
#endif
  struct touch_sample_s sample[0];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void gt911_worker(FAR void *arg);
static void gt911_poll_timeout(wdparm_t arg);

#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
static void gt911_diagnostic_report(FAR struct gt911_dev_s *priv);
static void gt911_diagnostic_frame(FAR struct gt911_dev_s *priv,
                                   uint8_t status,
                                   FAR const uint8_t *data,
                                   uint8_t data_len,
                                   int clear_ret,
                                   int readback_ret,
                                   uint8_t status_after_clear);
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: gt911_i2c_read
 ****************************************************************************/

static int gt911_i2c_read(FAR struct gt911_dev_s *priv, uint16_t reg,
                          FAR uint8_t *buffer, size_t buflen)
{
  struct i2c_msg_s msg[2];
  uint8_t regbuf[2];

  if (buffer == NULL || buflen == 0)
    {
      return -EINVAL;
    }

  regbuf[0] = reg >> 8;
  regbuf[1] = reg & 0xff;

  msg[0].frequency = priv->frequency;
  msg[0].addr = priv->address;
  msg[0].flags = 0;
  msg[0].buffer = regbuf;
  msg[0].length = sizeof(regbuf);

  msg[1].frequency = priv->frequency;
  msg[1].addr = priv->address;
  msg[1].flags = I2C_M_READ;
  msg[1].buffer = buffer;
  msg[1].length = buflen;

  return I2C_TRANSFER(priv->i2c, msg, 2);
}

/****************************************************************************
 * Name: gt911_i2c_write
 ****************************************************************************/

static int gt911_i2c_write(FAR struct gt911_dev_s *priv, uint16_t reg,
                           FAR const uint8_t *buffer, size_t buflen)
{
  struct i2c_msg_s msg;
  uint8_t txbuf[3];

  if (buffer == NULL || buflen != 1)
    {
      return -EINVAL;
    }

  txbuf[0] = reg >> 8;
  txbuf[1] = reg & 0xff;
  txbuf[2] = buffer[0];

  msg.frequency = priv->frequency;
  msg.addr = priv->address;
  msg.flags = 0;
  msg.buffer = txbuf;
  msg.length = sizeof(txbuf);
  return I2C_TRANSFER(priv->i2c, &msg, 1);
}

/****************************************************************************
 * Name: gt911_clear_status
 ****************************************************************************/

static int gt911_clear_status(FAR struct gt911_dev_s *priv)
{
  const uint8_t status = 0;

  return gt911_i2c_write(priv, GT911_REG_COORD_STATUS, &status,
                         sizeof(status));
}

#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
/****************************************************************************
 * Name: gt911_diagnostic_report
 *
 * Description:
 *   Emit a rate-limited polling summary directly to the serial console.
 *   This deliberately uses printf rather than syslog so board bring-up does
 *   not depend on a RAM log device being configured.
 ****************************************************************************/

static void gt911_diagnostic_report(FAR struct gt911_dev_s *priv)
{
  if (priv->diagnostic_scans - priv->diagnostic_last_report_scan <
      GT911_DIAGNOSTIC_INTERVAL_SCANS)
    {
      return;
    }

  printf("gt911: scans=%lu queued=%lu queue_err=%lu ready=%lu "
         "frames=%lu i2c_err=%lu status=0x%02x points=%u last_err=%d\n",
         (unsigned long)priv->diagnostic_scans,
         (unsigned long)priv->diagnostic_queued,
         (unsigned long)priv->diagnostic_queue_errors,
         (unsigned long)priv->diagnostic_ready,
         (unsigned long)priv->diagnostic_frames,
         (unsigned long)priv->diagnostic_i2c_errors,
         priv->diagnostic_last_status,
         priv->diagnostic_last_points,
         priv->diagnostic_last_error);
  priv->diagnostic_last_report_scan = priv->diagnostic_scans;
}

/****************************************************************************
 * Name: gt911_diagnostic_frame
 *
 * Description:
 *   Report changed controller frames and rate-limit acknowledgement
 *   failures.
 *   A successful acknowledgement must clear bit 7 in register 0x814e.
 ****************************************************************************/

static void gt911_diagnostic_frame(FAR struct gt911_dev_s *priv,
                                   uint8_t status,
                                   FAR const uint8_t *data,
                                   uint8_t data_len,
                                   int clear_ret,
                                   int readback_ret,
                                   uint8_t status_after_clear)
{
  bool ack_failed;
  bool changed;
  bool report_ack;
  uint8_t i;

  changed = !priv->diagnostic_last_frame_valid ||
            priv->diagnostic_last_frame_status != status ||
            priv->diagnostic_last_frame_len != data_len ||
            (data_len > 0 &&
             memcmp(priv->diagnostic_last_frame, data, data_len) != 0);
  ack_failed = clear_ret < 0 || readback_ret < 0 ||
               (status_after_clear & GT911_STATUS_READY) != 0;
  report_ack = ack_failed &&
               priv->diagnostic_scans -
               priv->diagnostic_last_ack_report_scan >=
               GT911_DIAGNOSTIC_INTERVAL_SCANS;

  if (changed || report_ack)
    {
      printf("gt911: frame before=0x%02x raw=", status);
      for (i = 0; i < data_len; i++)
        {
          printf("%s%02x", i == 0 ? "" : " ", data[i]);
        }

      printf(" clear_ret=%d readback_ret=%d after=0x%02x\n",
             clear_ret, readback_ret, status_after_clear);
    }

  if (report_ack)
    {
      priv->diagnostic_last_ack_report_scan = priv->diagnostic_scans;
    }

  priv->diagnostic_last_frame_status = status;
  priv->diagnostic_last_frame_len = data_len;
  if (data_len > 0)
    {
      memcpy(priv->diagnostic_last_frame, data, data_len);
    }

  priv->diagnostic_last_frame_valid = true;
}
#endif

/****************************************************************************
 * Name: gt911_emit_releases
 ****************************************************************************/

static void gt911_emit_releases(FAR struct gt911_dev_s *priv,
                                FAR const bool *seen)
{
  FAR struct touch_sample_s *sample = priv->sample;
  uint8_t point_count = 0;
  uint8_t id;

  memset(sample, 0, SIZEOF_TOUCH_SAMPLE_S(priv->max_points));
  for (id = 0; id < GT911_TRACK_ID_COUNT; id++)
    {
      FAR struct gt911_contact_s *contact = &priv->contacts[id];
      FAR struct touch_point_s *point;

      if (!contact->active || seen[id])
        {
          continue;
        }

      point = &sample->point[point_count++];
      point->id = id;
      point->flags = TOUCH_UP | TOUCH_ID_VALID | TOUCH_POS_VALID |
                     TOUCH_SIZE_VALID;
      point->x = contact->x;
      point->y = contact->y;
      point->h = contact->h;
      point->w = contact->w;
      point->timestamp = touch_get_time();
      contact->active = false;
    }

  if (point_count > 0)
    {
      sample->npoints = point_count;
      touch_event(priv->lower.priv, sample);
    }
}

/****************************************************************************
 * Name: gt911_map_coordinates
 ****************************************************************************/

static void gt911_map_coordinates(FAR const struct gt911_config_s *config,
                                  uint16_t raw_x, uint16_t raw_y,
                                  FAR struct touch_point_s *point)
{
  uint16_t x = raw_x;
  uint16_t y = raw_y;

  if (config->swap_xy)
    {
      x = raw_y;
      y = raw_x;
    }

  if (config->x_resolution > 0)
    {
      if (x >= config->x_resolution)
        {
          x = config->x_resolution - 1;
        }

      if (config->invert_x)
        {
          x = config->x_resolution - 1 - x;
        }
    }

  if (config->y_resolution > 0)
    {
      if (y >= config->y_resolution)
        {
          y = config->y_resolution - 1;
        }

      if (config->invert_y)
        {
          y = config->y_resolution - 1 - y;
        }
    }

  point->x = x;
  point->y = y;
}

/****************************************************************************
 * Name: gt911_decode_points
 ****************************************************************************/

static int gt911_decode_points(FAR struct gt911_dev_s *priv,
                               FAR const uint8_t *data, uint8_t touch_count)
{
  FAR struct touch_sample_s *sample = priv->sample;
  bool seen[GT911_TRACK_ID_COUNT];
  uint8_t point_count = 0;
  uint8_t i;

  memset(sample, 0, SIZEOF_TOUCH_SAMPLE_S(priv->max_points));
  memset(seen, 0, sizeof(seen));

  for (i = 0; i < touch_count; i++)
    {
      FAR const uint8_t *raw = &data[i * GT911_POINT_BYTES];
      FAR struct gt911_contact_s *contact;
      FAR struct touch_point_s *point;
      uint16_t size;
      uint16_t x;
      uint16_t y;
      uint8_t id;

      id = raw[0] & GT911_TRACK_ID_MASK;
      if (seen[id])
        {
          return -EPROTO;
        }

      seen[id] = true;
      contact = &priv->contacts[id];
      point = &sample->point[point_count++];
      size = raw[5] | ((uint16_t)raw[6] << 8);

      point->id = id;
      point->flags = (contact->active ? TOUCH_MOVE : TOUCH_DOWN) |
                     TOUCH_ID_VALID | TOUCH_POS_VALID | TOUCH_SIZE_VALID;
      x = raw[1] | ((uint16_t)raw[2] << 8);
      y = raw[3] | ((uint16_t)raw[4] << 8);
      gt911_map_coordinates(priv->config, x, y, point);
      point->h = size;
      point->w = size;
      point->timestamp = touch_get_time();

      contact->x = point->x;
      contact->y = point->y;
      contact->h = point->h;
      contact->w = point->w;
      contact->active = true;
    }

  if (point_count > 0)
    {
      sample->npoints = point_count;
      touch_event(priv->lower.priv, sample);
    }

  gt911_emit_releases(priv, seen);
  return OK;
}

/****************************************************************************
 * Name: gt911_process
 ****************************************************************************/

static int gt911_process(FAR struct gt911_dev_s *priv)
{
  uint8_t data[GT911_MAX_DATA_BYTES];
  uint8_t status;
  uint8_t touch_count;
  uint8_t data_len = 0;
  int clear_ret;
  int ret;
#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
  uint8_t status_after_clear = 0xff;
  int readback_ret = -ENODATA;
#endif

#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
  priv->diagnostic_scans++;
#endif

  ret = gt911_i2c_read(priv, GT911_REG_COORD_STATUS, &status,
                       sizeof(status));
  if (ret < 0)
    {
#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
      priv->diagnostic_i2c_errors++;
      priv->diagnostic_last_error = ret;
#endif
      return ret;
    }

#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
  priv->diagnostic_last_status = status;
#endif

  if ((status & GT911_STATUS_READY) == 0)
    {
      return OK;
    }

  touch_count = status & GT911_STATUS_TOUCH_MASK;
#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
  priv->diagnostic_ready++;
  priv->diagnostic_last_points = touch_count;
#endif

  if (touch_count > priv->max_points)
    {
      ret = -EPROTO;
#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
      priv->diagnostic_last_error = ret;
#endif
      goto clear_status;
    }

  data_len = touch_count * GT911_POINT_BYTES;

  if (touch_count > 0)
    {
      ret = gt911_i2c_read(priv, GT911_REG_POINT1, data,
                           data_len);
      if (ret < 0)
        {
#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
          priv->diagnostic_i2c_errors++;
          priv->diagnostic_last_error = ret;
#endif
          return ret;
        }
    }

  ret = OK;

clear_status:
  clear_ret = gt911_clear_status(priv);

#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
  if (clear_ret < 0)
    {
      priv->diagnostic_i2c_errors++;
      priv->diagnostic_last_error = clear_ret;
    }
  else
    {
      readback_ret = gt911_i2c_read(priv, GT911_REG_COORD_STATUS,
                                    &status_after_clear,
                                    sizeof(status_after_clear));
      if (readback_ret < 0)
        {
          priv->diagnostic_i2c_errors++;
          priv->diagnostic_last_error = readback_ret;
        }
    }

  gt911_diagnostic_frame(priv, status, data, data_len, clear_ret,
                         readback_ret, status_after_clear);
#endif

  if (ret >= 0 && clear_ret < 0)
    {
      ret = clear_ret;
    }

  if (ret < 0)
    {
      return ret;
    }

  /* Acknowledge the controller before publishing the frame.  This follows
   * the BOX-3 and Espressif GT911 transaction order and prevents upper-half
   * event handling from delaying the 0x814e status clear.
   */

  ret = gt911_decode_points(priv, data, touch_count);

#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
  if (ret >= 0 && touch_count > 0)
    {
      priv->diagnostic_frames++;
    }
  else if (ret < 0)
    {
      priv->diagnostic_last_error = ret;
    }
#endif

  return ret;
}

/****************************************************************************
 * Name: gt911_schedule_poll
 ****************************************************************************/

static int gt911_schedule_poll(FAR struct gt911_dev_s *priv)
{
  if (!priv->polling)
    {
      return OK;
    }

  return wd_start(&priv->poll_timer, MSEC2TICK(priv->poll_interval_ms),
                  gt911_poll_timeout, (wdparm_t)priv);
}

/****************************************************************************
 * Name: gt911_poll_timeout
 ****************************************************************************/

static void gt911_poll_timeout(wdparm_t arg)
{
  FAR struct gt911_dev_s *priv = (FAR struct gt911_dev_s *)arg;
  int ret;

  ret = work_queue(LPWORK, &priv->work, gt911_worker, priv, 0);
  if (ret < 0)
    {
#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
      priv->diagnostic_queue_errors++;
      priv->diagnostic_last_error = ret;
#endif
      gt911_schedule_poll(priv);
    }
#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
  else
    {
      priv->diagnostic_queued++;
    }
#endif
}

/****************************************************************************
 * Name: gt911_interrupt
 ****************************************************************************/

static int gt911_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct gt911_dev_s *priv = (FAR struct gt911_dev_s *)arg;
  int ret;

  (void)irq;
  (void)context;

  if (priv->board->irq_enable != NULL)
    {
      priv->board->irq_enable(priv->board, false);
    }

  ret = work_queue(LPWORK, &priv->work, gt911_worker, priv, 0);
  if (ret < 0 && priv->board->irq_enable != NULL)
    {
      priv->board->irq_enable(priv->board, true);
    }

  return OK;
}

/****************************************************************************
 * Name: gt911_worker
 ****************************************************************************/

static void gt911_worker(FAR void *arg)
{
  FAR struct gt911_dev_s *priv = (FAR struct gt911_dev_s *)arg;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret >= 0)
    {
#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
      ret = gt911_process(priv);
      nxmutex_unlock(&priv->lock);

      if (ret < 0)
        {
          priv->diagnostic_last_error = ret;
        }

      gt911_diagnostic_report(priv);
#else
      (void)gt911_process(priv);
      nxmutex_unlock(&priv->lock);
#endif
    }

  if (priv->polling)
    {
      (void)gt911_schedule_poll(priv);
    }
  else if (priv->board->irq_enable != NULL)
    {
      priv->board->irq_enable(priv->board, true);
    }
}

/****************************************************************************
 * Name: gt911_probe
 ****************************************************************************/

static int gt911_probe(FAR struct gt911_dev_s *priv)
{
  uint8_t product_id[4];
  int ret;

  ret = gt911_i2c_read(priv, GT911_REG_PRODUCT_ID, product_id,
                       sizeof(product_id));
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GT911 probe failed: stage=product-id-read "
             "i2c_addr=0x%02x frequency=%lu reg=0x%04x ret=%d\n",
             priv->address, (unsigned long)priv->frequency,
             GT911_REG_PRODUCT_ID, ret);
    }
  else
    {
      syslog(LOG_INFO, "GT911 product id: %02x %02x %02x %02x (%.*s)\n",
             product_id[0], product_id[1], product_id[2], product_id[3],
             (int)sizeof(product_id), (FAR const char *)product_id);
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: gt911_register
 ****************************************************************************/

int gt911_register(FAR const char *devpath,
                   FAR struct i2c_master_s *i2c,
                   FAR const struct gt911_config_s *config)
{
  FAR struct gt911_dev_s *priv;
  FAR const struct gt911_board_s *board;
  uint8_t max_points;
  uint8_t event_buffer_count;
  int ret;

  if (devpath == NULL || i2c == NULL || config == NULL)
    {
      return -EINVAL;
    }

  board = config->board;
  if ((board == NULL || board->irq_attach == NULL ||
       board->irq_enable == NULL) && config->poll_interval_ms == 0)
    {
      return -EINVAL;
    }

  max_points = config->max_points == 0 ? GT911_MAX_POINTS :
                                         config->max_points;
  if (max_points > GT911_MAX_POINTS)
    {
      return -EINVAL;
    }

  event_buffer_count = config->event_buffer_count == 0 ?
                       GT911_EVENT_BUFFER_COUNT : config->event_buffer_count;
  priv = kmm_zalloc(sizeof(*priv) + SIZEOF_TOUCH_SAMPLE_S(max_points));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

  priv->i2c = i2c;
  priv->config = config;
  priv->board = board;
  priv->frequency = config->frequency == 0 ? GT911_I2C_FREQUENCY :
                                             config->frequency;
  priv->address = config->address == 0 ? GT911_I2C_ADDRESS : config->address;
  priv->poll_interval_ms = config->poll_interval_ms;
  priv->max_points = max_points;
  priv->polling = config->poll_interval_ms != 0;
  priv->lower.maxpoint = max_points;
  nxmutex_init(&priv->lock);

  if (board != NULL && board->reset != NULL)
    {
      ret = board->reset(board);
      if (ret < 0)
        {
          goto err_destroy_lock;
        }
    }

  ret = gt911_probe(priv);
  if (ret < 0)
    {
      goto err_destroy_lock;
    }

  ret = touch_register(&priv->lower, devpath, event_buffer_count);
  if (ret < 0)
    {
      goto err_destroy_lock;
    }

  if (priv->polling)
    {
      ret = gt911_schedule_poll(priv);
      if (ret < 0)
        {
          goto err_unregister_touch;
        }

#ifdef CONFIG_INPUT_GT911_DIAGNOSTICS
      printf("gt911: diagnostics enabled poll_ms=%u address=0x%02x\n",
             priv->poll_interval_ms, priv->address);
#endif
      return OK;
    }

  ret = board->irq_attach(board, gt911_interrupt, priv);
  if (ret < 0)
    {
      goto err_unregister_touch;
    }

  board->irq_enable(board, true);
  return OK;

err_unregister_touch:
  touch_unregister(&priv->lower, devpath);
err_destroy_lock:
  nxmutex_destroy(&priv->lock);
  kmm_free(priv);
  return ret;
}

#endif /* CONFIG_INPUT_GT911 && CONFIG_I2C */
