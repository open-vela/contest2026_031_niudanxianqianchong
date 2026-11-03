#pragma once
#include "smart_home_lvgl.h"
#include "smart_home_v9_device_view.h"
void smart_home_lvgl_room_dialog_open(smart_home_lvgl_t *ui);
void smart_home_lvgl_room_assign_open(smart_home_lvgl_t *ui,
                                      const smart_home_v9_device_view_t *view);
void smart_home_lvgl_room_dialog_close(void);
const char *smart_home_lvgl_room_display(const char *room);
