/* SPDX-License-Identifier: Apache-2.0 */

#ifndef SMART_HOME_CAMERA_SERVICE_H
#define SMART_HOME_CAMERA_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The source is fixed by the current SC2336 V4L2 profile.  The preview is
 * deliberately smaller than the screen: it keeps the first LVGL integration
 * at 15 FPS while the sensor still captures at 30 FPS.  512x300 is exactly
 * half the source in both axes, preserving aspect ratio for PPA scaling
 * and the nearest-neighbour CPU fallback.
 */

#define SMART_HOME_CAMERA_PREVIEW_WIDTH   512u
#define SMART_HOME_CAMERA_PREVIEW_HEIGHT  300u
#define SMART_HOME_CAMERA_PREVIEW_BYTES   \
  (SMART_HOME_CAMERA_PREVIEW_WIDTH * SMART_HOME_CAMERA_PREVIEW_HEIGHT * 2u)

typedef enum
{
  SMART_HOME_CAMERA_OFF = 0,
  SMART_HOME_CAMERA_STARTING,
  SMART_HOME_CAMERA_RUNNING,
  SMART_HOME_CAMERA_ERROR,
} smart_home_camera_state_t;

struct smart_home_camera_status_s
{
  smart_home_camera_state_t state;
  int last_error;
  uint32_t capture_sequence;
  uint32_t preview_sequence;
  uint32_t capture_fps_x100;
  uint32_t preview_fps_x100;
  uint32_t dropped_frames;
  uint32_t ppa_frames;
  uint32_t cpu_frames;
  uint32_t ppa_errors;
  int ppa_last_error;
  uint64_t ppa_total_us;
  uint64_t cpu_total_us;
  uint32_t scale_max_us;
};

int smart_home_camera_start(void);
int smart_home_camera_stop(void);
int smart_home_camera_get_status(struct smart_home_camera_status_s *status);

/* Copy the latest completed RGB565 preview.  The caller owns dst and may use
 * it after this function returns.  This interface never exposes a V4L2 DMA
 * buffer to LVGL. */
int smart_home_camera_copy_latest(uint8_t *dst, size_t dst_bytes,
                                  uint32_t *sequence);

#endif /* SMART_HOME_CAMERA_SERVICE_H */
