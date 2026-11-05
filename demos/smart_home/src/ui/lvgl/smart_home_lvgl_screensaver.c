/**
 * Lightweight native screensaver.
 *
 * Clock and weather reuse the home timer and cached snapshots. No network
 * request, camera frame or Agent call is started by this screen.  The first touch gesture only changes LVGL screens and
 * therefore remains safe during early board bring-up.
 */

#include "smart_home_lvgl_internal.h"


static void screensaver_wake_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (!ui || (code != LV_EVENT_GESTURE && code != LV_EVENT_CLICKED)) {
        return;
    }

    smart_home_lvgl_load_tab(ui, SMART_HOME_TAB_HOME);
}

void smart_home_lvgl_build_screensaver_screen(smart_home_lvgl_t *ui)
{
    lv_obj_t *screen;
    lv_obj_t *label;

    if (!ui) {
        return;
    }

    screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    smart_home_lvgl_set_bg(screen, SMART_HOME_UI_COLOR_SURFACE_SOFT);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, screensaver_wake_cb, LV_EVENT_GESTURE, ui);
    lv_obj_add_event_cb(screen, screensaver_wake_cb, LV_EVENT_CLICKED, ui);
    ui->screen_screensaver = screen;

    label = smart_home_lvgl_label_create(screen, "我的家",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 20);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, smart_home_lvgl_pad_x(), 28);

    ui->screensaver_time_label = smart_home_lvgl_label_create(
        screen, SMART_HOME_UI_DEFAULT_CLOCK, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 96);
    lv_obj_align(ui->screensaver_time_label, LV_ALIGN_CENTER, 0, -50);
    ui->screensaver_date_label = smart_home_lvgl_label_create(screen,
        "", SMART_HOME_UI_COLOR_TEXT_SECONDARY, 18);
    lv_obj_align(ui->screensaver_date_label, LV_ALIGN_CENTER, 0, 30);
    ui->screensaver_temp_label = smart_home_lvgl_label_create(screen,
        "--°", SMART_HOME_UI_COLOR_TEXT_PRIMARY, 48);
    lv_obj_align(ui->screensaver_temp_label, LV_ALIGN_CENTER, 0, 90);
    ui->screensaver_condition_label = smart_home_lvgl_label_create(screen,
        SMART_HOME_UI_DEFAULT_WEATHER, SMART_HOME_UI_COLOR_TEXT_SECONDARY, 18);
    lv_obj_align(ui->screensaver_condition_label, LV_ALIGN_CENTER, 0, 130);
    label = smart_home_lvgl_label_create(screen, "轻触进入首页",
                                         SMART_HOME_UI_COLOR_PRIMARY_DARK, 14);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -38);

    smart_home_lvgl_update_clock(ui);
    smart_home_lvgl_refresh_home(ui);
}
