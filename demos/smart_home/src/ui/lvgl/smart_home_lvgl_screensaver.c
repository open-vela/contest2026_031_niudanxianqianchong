/**
 * Lightweight native screensaver.
 *
 * It intentionally uses no bitmap background, timer, network request, camera
 * frame or Agent call.  The first touch gesture only changes LVGL screens and
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
        screen, "12:00", SMART_HOME_UI_COLOR_TEXT_PRIMARY, 32);
    lv_obj_align(ui->screensaver_time_label, LV_ALIGN_CENTER, 0, -26);
    {
        struct timespec ts;
        struct tm tm_now;
        char buf[24];
        static const char *const wd[] = {"日","一","二","三","四","五","六"};

        clock_gettime(CLOCK_REALTIME, &ts);
        if (ts.tv_sec > 1000000000L) {
            /* HTTP Date 为 UTC，加 8 小时偏移显示北京时间。 */
            ts.tv_sec += 8 * 3600L;
            gmtime_r(&ts.tv_sec, &tm_now);
            snprintf(buf, sizeof(buf), "%d 月 %d 日 · 周%s",
                     tm_now.tm_mon + 1, tm_now.tm_mday, wd[tm_now.tm_wday % 7]);
            if (ui->screensaver_time_label) {
                char tb[8];
                snprintf(tb, sizeof(tb), "%02d:%02d",
                         tm_now.tm_hour, tm_now.tm_min);
                lv_label_set_text(ui->screensaver_time_label, tb);
            }
        } else {
            snprintf(buf, sizeof(buf), "时间同步中…");
        }
        label = smart_home_lvgl_label_create(screen, buf,
                                             SMART_HOME_UI_COLOR_TEXT_SECONDARY, 16);
    }
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 20);
    label = smart_home_lvgl_label_create(screen, "晴 · 32°C · 家庭状态正常",
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY, 14);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 54);

    label = smart_home_lvgl_label_create(screen, "上滑进入首页",
                                         SMART_HOME_UI_COLOR_PRIMARY_DARK, 14);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -38);
}
