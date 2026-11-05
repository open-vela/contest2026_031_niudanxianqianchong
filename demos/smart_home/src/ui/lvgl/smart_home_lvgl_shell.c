/* Shared V9 top bar, weather and clock. LVGL owner thread only. */
#include "smart_home_lvgl_internal.h"
#include "images/smart_home_icons.h"
#include "icons/smart_home_lvgl_png_icons.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static lv_color_t topbar_network_color(const smart_home_lvgl_t *ui)
{
    const smart_home_network_status_t *status;

    if (!ui || !ui->app) {
        return SMART_HOME_UI_COLOR_TEXT_MUTED;
    }
    status = &ui->app->system_status.network_status;
    if (status->online) {
        return SMART_HOME_UI_COLOR_TEXT_SECONDARY;
    }
    if (status->ip_status == SMART_HOME_NETWORK_OK) {
        return SMART_HOME_UI_COLOR_WARNING;
    }
    return SMART_HOME_UI_COLOR_TEXT_MUTED;
}

static const char *topbar_network_icon(const smart_home_lvgl_t *ui)
{
    const smart_home_network_status_t *status;

    if (!ui || !ui->app) {
        return ICON_STATUS_WIFI_OFF;
    }
    status = &ui->app->system_status.network_status;
    return status->online || status->ip_status == SMART_HOME_NETWORK_OK ?
        ICON_STATUS_WIFI : ICON_STATUS_WIFI_OFF;
}

void smart_home_lvgl_refresh_network_indicators(smart_home_lvgl_t *ui)
{
    lv_color_t color;
    int i;

    if (!ui) {
        return;
    }
    color = topbar_network_color(ui);
    for (i = 0; i < ui->topbar_wifi_icon_count; i++) {
        lv_obj_t *icon = ui->topbar_wifi_icons[i];

        if (!icon) {
            continue;
        }
        {
            const lv_image_dsc_t *asset = smart_home_lvgl_png_icon_get(
                topbar_network_icon(ui), 20);

            if (asset) {
                lv_image_set_src(icon, asset);
            }
        }
        lv_obj_set_style_text_color(icon, color, 0);
        smart_home_lvgl_icon_color(icon, color);
    }
}

void smart_home_lvgl_build_top_bar(lv_obj_t *screen, smart_home_lvgl_t *ui,
                                   const char *title)
{
    lv_obj_t *bar;
    lv_obj_t *brand;
    lv_obj_t *clock;
    lv_obj_t *icon;
    int width = smart_home_lvgl_disp_w();
    bool home = ui && screen == ui->screen_home;
    int clock_width = home ? 80 : 56;
    int i;

    if (!screen) {
        return;
    }
    /* Product pages keep one stable 56 px brand bar; page identity belongs
     * in the content heading so the shell never jumps while navigating. */
    (void)title;

    bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, width, SMART_HOME_TOPBAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    smart_home_lvgl_set_bg(bar, SMART_HOME_UI_COLOR_SURFACE);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, SMART_HOME_UI_COLOR_BORDER, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    icon = smart_home_lvgl_icon_create(bar, ICON_BRAND, 36, 36);
    if (icon) {
        lv_obj_set_pos(icon, 24, 10);
        /* The V9 brand mark keeps its own colors. */
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_TRANSP, 0);
    }

    brand = smart_home_lvgl_label_create(bar,
                                         "OpenVela HOME",
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                         18);
    lv_obj_set_pos(brand, 72, 18);

    clock = smart_home_lvgl_label_create(bar, SMART_HOME_UI_DEFAULT_CLOCK,
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                         home ? 26 : 18);
    lv_obj_set_width(clock, clock_width);
    lv_obj_set_style_text_align(clock, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(clock, LV_ALIGN_TOP_RIGHT, -16, home ? 6 : 16);
    if (ui && ui->topbar_clock_count <
                   (int)(sizeof(ui->topbar_clock_labels) /
                         sizeof(ui->topbar_clock_labels[0]))) {
        ui->topbar_clock_labels[ui->topbar_clock_count++] = clock;
    }

    for (i = 0; i < 4; i++) {
        const char *name;
        lv_color_t color;

        if (i == 0) {
            name = "asset:microphone-off";
        } else if (i == 1) {
            name = (ui && ui->topbar_camera_on) ?
                   ICON_STATUS_CAMERA : "asset:camera-off";
        } else if (i == 2) {
            name = "asset:volume";
        } else {
            name = topbar_network_icon(ui);
        }

        icon = smart_home_lvgl_icon_create(bar, name, 20, 20);
        if (!icon) {
            continue;
        }
        color = i == 3 ? topbar_network_color(ui) :
                         SMART_HOME_UI_COLOR_TEXT_SECONDARY;
        /* 为首页较大的时钟预留宽度，状态图标仍保持 12px 间隔。 */
        lv_obj_align(icon, LV_ALIGN_TOP_RIGHT,
                     -(16 + clock_width + 12 + (3 - i) * 32), 18);
        lv_obj_set_style_text_color(icon, color, 0);
        smart_home_lvgl_icon_color(icon, color);
        if (i == 3 && ui && ui->topbar_wifi_icon_count <
                           (int)(sizeof(ui->topbar_wifi_icons) /
                                 sizeof(ui->topbar_wifi_icons[0]))) {
            ui->topbar_wifi_icons[ui->topbar_wifi_icon_count++] = icon;
        }
        if (i == 1 && ui && ui->topbar_camera_icon_count <
                           (int)(sizeof(ui->topbar_camera_icons) /
                                 sizeof(ui->topbar_camera_icons[0]))) {
            ui->topbar_camera_icons[ui->topbar_camera_icon_count++] = icon;
        }
    }
}

void smart_home_lvgl_set_camera_indicator(smart_home_lvgl_t *ui, int on)
{
    int i;

    if (!ui) {
        return;
    }

    ui->topbar_camera_on = on != 0;
    for (i = 0; i < ui->topbar_camera_icon_count; i++) {
        lv_obj_t *icon = ui->topbar_camera_icons[i];
        const lv_image_dsc_t *asset = smart_home_lvgl_png_icon_get(
            on ? "asset:camera" : "asset:camera-off", 20);

        if (icon && asset) {
            lv_image_set_src(icon, asset);
        }
    }
}

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
static const char *weather_icon_for(const char *condition_en)
{
    if (!condition_en || !condition_en[0]) {
        return "asset:cloud-rain";
    }

    /* 晴类 → 太阳 */
    if (strstr(condition_en, "Sunny") != NULL ||
        strstr(condition_en, "Clear") != NULL) {
        return "asset:sun";
    }

    /* 降水/雾/霾类 → 云雨（现有资产中最接近的图形） */
    return "asset:cloud-rain";
}

#endif

static void home_weather_set_icon(smart_home_lvgl_t *ui, const char *name)
{
    if (ui->home_weather_badge) {
        /* 尺寸取徽标自身宽度，避免与构建时设置的 64/32 不一致。 */
        int size = smart_home_lvgl_disp_w() > 0 ?
                   (int)lv_obj_get_width(ui->home_weather_badge) : 32;
        const lv_image_dsc_t *asset = smart_home_lvgl_png_icon_get(name, size);
        lv_obj_t *glyph = lv_obj_get_child(ui->home_weather_badge, 0);

        if (asset && glyph && lv_obj_check_type(glyph, &lv_image_class)) {
            lv_image_set_src(glyph, asset);
        }
    }
}

void smart_home_lvgl_refresh_home(smart_home_lvgl_t *ui)
{
    char temperature[16] = "--°";
    const char *condition = SMART_HOME_UI_DEFAULT_WEATHER;
    const char *icon = "asset:sun";
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    smart_home_miloco_weather_t weather;
#endif

    if (!ui) return;
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (ui->app && ui->app->miloco &&
        smart_home_miloco_get_weather(ui->app->miloco, &weather) && weather.valid) {
        icon = weather_icon_for(weather.condition_en);
        if (weather.condition_cn[0]) condition = weather.condition_cn;
        snprintf(temperature, sizeof(temperature), "%d°", weather.temperature);
    }
#endif
    home_weather_set_icon(ui, icon);
    if (ui->home_weather_temp_label)
        lv_label_set_text(ui->home_weather_temp_label, temperature);
    if (ui->home_weather_condition_label)
        lv_label_set_text(ui->home_weather_condition_label, condition);
    if (ui->screensaver_temp_label)
        lv_label_set_text(ui->screensaver_temp_label, temperature);
    if (ui->screensaver_condition_label)
        lv_label_set_text(ui->screensaver_condition_label, condition);
    smart_home_lvgl_v9_refresh_views(ui);
}

/* UI 共用一份北京时间，日期与时间同时更新，排除板端 2019 初始日期。 */
void smart_home_lvgl_update_clock_at(smart_home_lvgl_t *ui, time_t utc)
{
    struct tm tm_now;
    char clock_text[16] = SMART_HOME_UI_DEFAULT_CLOCK;
    char date_text[40] = "";
    static const char *const weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
    if (!ui) return;
    if (utc >= 1577836800L) {
        utc += 8 * 3600L;
        if (gmtime_r(&utc, &tm_now)) {
            snprintf(clock_text, sizeof(clock_text), "%02d:%02d",
                     tm_now.tm_hour, tm_now.tm_min);
            snprintf(date_text, sizeof(date_text), "%d月%d日 星期%s",
                     tm_now.tm_mon + 1, tm_now.tm_mday, weekdays[tm_now.tm_wday % 7]);
        }
    }
    for (int i = 0; i < ui->topbar_clock_count; i++) {
        if (ui->topbar_clock_labels[i])
            lv_label_set_text(ui->topbar_clock_labels[i], clock_text);
    }
    if (ui->home_time_label) lv_label_set_text(ui->home_time_label, clock_text);
    if (ui->screensaver_time_label)
        lv_label_set_text(ui->screensaver_time_label, clock_text);
    if (ui->home_date_label) lv_label_set_text(ui->home_date_label, date_text);
    if (ui->screensaver_date_label)
        lv_label_set_text(ui->screensaver_date_label, date_text);
}

void smart_home_lvgl_update_clock(smart_home_lvgl_t *ui)
{
    struct timespec ts;
    time_t now = clock_gettime(CLOCK_REALTIME, &ts) == 0 ? ts.tv_sec : 0;
    smart_home_lvgl_update_clock_at(ui, now);
}
