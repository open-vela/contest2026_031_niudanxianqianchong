#pragma once
#include "smart_home_lvgl.h"
#include "smart_home_v9_device_view.h"

void smart_home_lvgl_device_dialog_open(smart_home_lvgl_t *ui,
                                         const smart_home_v9_device_view_t *view);
void smart_home_lvgl_device_dialog_close(void);
