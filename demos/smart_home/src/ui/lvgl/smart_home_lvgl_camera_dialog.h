/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "smart_home_lvgl.h"
#include "smart_home_v9_device_view.h"
#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
int smart_home_lvgl_camera_dialog_open(smart_home_lvgl_t *ui,
                                        const smart_home_v9_device_view_t *view);
void smart_home_lvgl_camera_dialog_close(void);
#else
static inline void smart_home_lvgl_camera_dialog_close(void) {}
#endif
