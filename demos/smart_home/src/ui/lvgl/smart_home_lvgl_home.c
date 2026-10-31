/**
 * SmartHome 1024x600 product home screen.
 *
 * The home page intentionally contains only summaries and navigation.  Device
 * state remains owned by the native smart_home service; the labels here are
 * refreshed on the LVGL owner thread together with the device cards.
 */

#include "smart_home_lvgl_internal.h"
#include "images/smart_home_icons.h"
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
#include "../../miloco/smart_home_miloco.h"
#endif
#include "icons/smart_home_lvgl_png_icons.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

enum home_action_e {
    HOME_ACTION_DEVICES = 1,
    HOME_ACTION_SCENES,
    HOME_ACTION_SECURITY,
    HOME_ACTION_AGENT,
    HOME_ACTION_MORE,
    HOME_ACTION_MIHOME,
};

/* Scene modes moved from the removed scenes tab into the home card popup.
 * A mode click pre-fills the chat input; the agent owns the execution. */
static const struct scene_mode_s {
    const char *name;
    const char *command;
    const char *desc;
    const char *icon;
    uint32_t badge_bg;
} g_scene_modes[] = {
    { "回家模式", "执行回家模式", "按已接入设备执行",
      ICON_NAV_HOME, 0xFFF5EC },
    { "观影模式", "执行观影模式", "按已接入设备执行",
      ICON_MEDIA_VIDEO, 0xF1F4FF },
    { "睡眠模式", "执行睡眠模式", "按已接入设备执行",
      ICON_SCENE_SLEEP, 0xF4F2FF },
    { "离家模式", "执行离家模式", "按已接入设备执行",
      ICON_SCENE_AWAY, 0xEDF8F3 },
};

static void scene_popup_dismiss(smart_home_lvgl_t *ui)
{
    if (ui && ui->scene_popup) {
        lv_obj_add_flag(ui->scene_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void scene_popup_mask_cb(lv_event_t *event)
{
    scene_popup_dismiss(lv_event_get_user_data(event));
}

static void scene_mode_click_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    lv_obj_t *button = lv_event_get_current_target(event);
    int mode = (int)(intptr_t)lv_obj_get_user_data(button);

    if (!ui || mode < 0 ||
        mode >= (int)(sizeof(g_scene_modes) / sizeof(g_scene_modes[0]))) {
        return;
    }
    scene_popup_dismiss(ui);
    smart_home_lvgl_load_tab(ui, SMART_HOME_TAB_CHAT);
    if (ui->chat_input) {
        lv_textarea_set_text(ui->chat_input, g_scene_modes[mode].command);
    }
}

void smart_home_lvgl_open_scene_popup(smart_home_lvgl_t *ui)
{
    lv_obj_t *mask;
    lv_obj_t *card;
    lv_obj_t *button;
    lv_obj_t *badge;
    lv_obj_t *label;
    int compact = smart_home_lvgl_compact();
    int popup_w = compact ? 380 : 470;
    int popup_h = compact ? 300 : 350;
    int pad = compact ? 14 : 20;
    int title_h = compact ? 30 : 34;
    int gap = 14;
    int btn_w = (popup_w - 2 * pad - gap) / 2;
    int btn_h = (popup_h - 2 * pad - title_h - gap) / 2;
    int i;

    if (!ui || !ui->screen_home) {
        return;
    }
    if (ui->scene_popup) {
        lv_obj_clear_flag(ui->scene_popup, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* Full-screen click-catcher doubles as the popup root; clicking the
     * dimmed background dismisses without selecting a mode. */
    mask = lv_obj_create(ui->screen_home);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, smart_home_lvgl_disp_w(), smart_home_lvgl_disp_h());
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_30, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mask, scene_popup_mask_cb, LV_EVENT_CLICKED, ui);
    ui->scene_popup = mask;

    card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, popup_w, popup_h);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    smart_home_lvgl_card_style(card);

    label = smart_home_lvgl_label_create(card, "选择模式",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 18);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    for (i = 0; i < (int)(sizeof(g_scene_modes) / sizeof(g_scene_modes[0]));
         i++) {
        int col = i % 2;
        int row = i / 2;

        button = lv_obj_create(card);
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, btn_w, btn_h);
        lv_obj_align(button, LV_ALIGN_TOP_LEFT,
                     col * (btn_w + gap), title_h + row * (btn_h + gap));
        smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_SURFACE_SOFT);
        lv_obj_set_style_radius(button, 12, 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, SMART_HOME_UI_COLOR_BORDER, 0);
        lv_obj_set_style_pad_all(button, 10, 0);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(button, (void *)(intptr_t)i);
        smart_home_lvgl_press_feedback(button);
        lv_obj_add_event_cb(button, scene_mode_click_cb, LV_EVENT_CLICKED, ui);

        badge = lv_obj_create(button);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, 44, 44);
        lv_obj_align(badge, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_radius(badge, 12, 0);
        smart_home_lvgl_set_bg(badge, lv_color_hex(g_scene_modes[i].badge_bg));
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
        {
            lv_obj_t *glyph = smart_home_lvgl_icon_create(
                badge, g_scene_modes[i].icon, 26, 26);

            if (glyph) {
                lv_obj_center(glyph);
                lv_obj_set_style_image_recolor(
                    glyph, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 0);
                lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
            }
        }

        label = smart_home_lvgl_label_create(button, g_scene_modes[i].name,
                                             SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                             16);
        lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, compact ? -30 : -34);
        label = smart_home_lvgl_label_create(button, g_scene_modes[i].desc,
                                             SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                             12);
        lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
}

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
                                         18);
    lv_obj_set_width(clock, 56);
    lv_obj_set_style_text_align(clock, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(clock, LV_ALIGN_TOP_RIGHT, -16, 16);
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
        /* 16 px right inset, 56 px clock, 12 px gaps and 20 px icons. */
        lv_obj_align(icon, LV_ALIGN_TOP_RIGHT, -(16 + 56 + 12 + (3 - i) * 32), 18);
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

static lv_obj_t *home_card(lv_obj_t *screen, int x, int y, int w, int h,
                           lv_color_t color)
{
    lv_obj_t *card = lv_obj_create(screen);

    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    smart_home_lvgl_card_style(card);
    smart_home_lvgl_set_bg(card, color);
    return card;
}

static void home_feature_text(lv_obj_t *card, const char *title,
                              const char *body, lv_color_t title_color)
{
    lv_obj_t *label = smart_home_lvgl_label_create(card, title, title_color, 16);

    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, -24);
    label = smart_home_lvgl_label_create(card, body,
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY, 13);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

/* 天气条件→图标：wttr.in 返回英文条件词，映射到已有图标资产。 */
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

static void home_weather_set_icon(smart_home_lvgl_t *ui, const char *name)
{
    if (ui->home_weather_badge) {
        int size = lv_obj_get_width(ui->home_weather_badge) > 40 ? 60 : 32;
        const lv_image_dsc_t *asset = smart_home_lvgl_png_icon_get(name, size);
        lv_obj_t *glyph = lv_obj_get_child(ui->home_weather_badge, 0);

        if (asset && glyph && lv_obj_check_type(glyph, &lv_image_class)) {
            lv_image_set_src(glyph, asset);
        }
    }
}

static lv_obj_t *home_icon_badge(lv_obj_t *card, const char *icon,
                                 lv_color_t color, int size)
{
    lv_obj_t *badge = lv_obj_create(card);
    lv_obj_t *glyph;

    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, size, size);
    lv_obj_align(badge, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(badge, size / 3, 0);
    smart_home_lvgl_set_bg(badge, color);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    glyph = smart_home_lvgl_icon_create(badge, icon, size - 16, size - 16);
    if (glyph) {
        lv_obj_set_style_text_color(glyph, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_image_recolor(glyph, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
        lv_obj_center(glyph);
    }
    return badge;
}

static void home_music_controls(lv_obj_t *card)
{
    static const char *const controls[] = {
        ICON_MEDIA_PREVIOUS,
        ICON_MEDIA_PLAY,
        ICON_MEDIA_NEXT,
    };
    lv_obj_t *row;
    int i;
    const int item_w = 38;
    const int item_h = 32;
    const int gap = 6;
    const int row_w = (int)(sizeof(controls) / sizeof(controls[0])) * item_w +
                      ((int)(sizeof(controls) / sizeof(controls[0])) - 1) * gap;

    row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, row_w, item_h);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    for (i = 0; i < (int)(sizeof(controls) / sizeof(controls[0])); i++) {
        lv_obj_t *icon = smart_home_lvgl_icon_create(row, controls[i], 32, 32);

        if (!icon) {
            continue;
        }
        lv_obj_set_pos(icon, i * (item_w + gap) + (item_w - 32) / 2, 0);
        lv_obj_set_style_image_recolor(icon,
                                       SMART_HOME_UI_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void home_action_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    int action = (int)(intptr_t)lv_obj_get_user_data(
        lv_event_get_current_target(event));

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui) {
        return;
    }

    switch (action) {
    case HOME_ACTION_DEVICES:
        if (ui->panel_title) {
            lv_label_set_text(ui->panel_title, "我的设备");
        }
        smart_home_lvgl_load_tab(ui, SMART_HOME_TAB_DEVICES);
        break;
    case HOME_ACTION_MIHOME:
        /* 引导链：未连网关 → 网关设置页；未绑定账号 → 扫码绑定页；
         * 一切就绪 → 设备页。 */
        if (!ui->app || !ui->app->miloco ||
            !smart_home_miloco_reachable(ui->app->miloco)) {
            if (ui->screen_miloco) {
                smart_home_lvgl_refresh_miloco_screen(ui);
                lv_scr_load_anim(ui->screen_miloco, LV_SCR_LOAD_ANIM_MOVE_LEFT,
                                 180, 0, false);
                break;
            }
        } else if (!smart_home_miloco_bound(ui->app->miloco)) {
            smart_home_lvgl_open_miloco_bind(ui);
            break;
        }
        if (ui->panel_title) {
            lv_label_set_text(ui->panel_title, "米家设备管理");
        }
        smart_home_lvgl_load_tab(ui, SMART_HOME_TAB_DEVICES);
        break;
    case HOME_ACTION_SCENES:
        smart_home_lvgl_open_scene_popup(ui);
        break;
    case HOME_ACTION_SECURITY:
        smart_home_lvgl_load_tab(ui, SMART_HOME_TAB_SECURITY);
        break;
    case HOME_ACTION_MORE:
        smart_home_lvgl_load_tab(ui, SMART_HOME_TAB_MORE);
        break;
    case HOME_ACTION_AGENT:
        smart_home_lvgl_load_tab(ui, SMART_HOME_TAB_CHAT);
        break;
    default:
        break;
    }
}

static void home_make_clickable(lv_obj_t *card, smart_home_lvgl_t *ui,
                                enum home_action_e action)
{
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card, (void *)(intptr_t)action);
    smart_home_lvgl_press_feedback(card);
    lv_obj_add_event_cb(card, home_action_cb, LV_EVENT_CLICKED, ui);
}

void smart_home_lvgl_refresh_home(smart_home_lvgl_t *ui)
{
    const smart_home_state_t *state;
    char text[96];
    int on_count = 0;
    int i;

    if (!ui || !ui->device_state) {
        return;
    }

    state = ui->device_state;
    for (i = 0; i < SMART_HOME_MAX_DEVICES; i++) {
        const smart_home_device_t *device = &state->devices[i];

        if (!device->used) {
            continue;
        }
        if (device->on) {
            on_count++;
        }
    }

    if (ui->home_env_label) {
        snprintf(text, sizeof(text), "演示湿度 %d%%",
                 state->env_humidity);
        lv_label_set_text(ui->home_env_label, text);
    }
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    /* 米家桥接模式下首页统计以网关真实状态为准，不再数本地虚拟设备。 */
    {
        bool reachable = ui->app && ui->app->miloco &&
                         smart_home_miloco_reachable(ui->app->miloco);
        size_t miloco_count = 0;
        size_t online_count = 0;

        if (reachable) {
            /* 快照数组静态化：本函数在 build_home_screen 深调用链上执行，
             * 避免大数组占用主线程栈；仅 LVGL 线程调用，线程安全。 */
            static smart_home_miloco_device_t
                devices[SMART_HOME_MILOCO_MAX_DEVICES];
            size_t i;

            miloco_count = smart_home_miloco_list(
                ui->app->miloco, devices, SMART_HOME_MILOCO_MAX_DEVICES, NULL);
            for (i = 0; i < miloco_count; i++) {
                if (devices[i].online) {
                    online_count++;
                }
            }
        }
        {
            bool bound = reachable &&
                         smart_home_miloco_bound(ui->app->miloco);

            if (ui->home_miloco_sub_label) {
                lv_label_set_text(ui->home_miloco_sub_label,
                                  !reachable ? "未连接 · 查看网关设置" :
                                  bound ? "已连接 · 米家已绑定" :
                                          "已连接 · 待绑定账号");
            }
            if (ui->home_ac_label) {
                if (!reachable) {
                    snprintf(text, sizeof(text),
                             "米家网关未连接\n点击查看网关设置");
                } else if (!bound) {
                    snprintf(text, sizeof(text),
                             "米家账号未绑定\n点击扫码绑定");
                } else {
                    snprintf(text, sizeof(text),
                             "%u 台米家设备\n点击进入设备管理",
                             (unsigned)miloco_count);
                }
                lv_label_set_text(ui->home_ac_label, text);
            }
        }
        /* 天气卡显示（真实数据或占位）。 */
        {
            smart_home_miloco_weather_t wx;
            char buf[96];

            if (ui->app && ui->app->miloco &&
                smart_home_miloco_get_weather(ui->app->miloco, &wx) && wx.valid) {
                if (ui->home_weather_city_label) {
                    lv_label_set_text(ui->home_weather_city_label,
                        !strcmp(wx.city, "Shenzhen") ? "深圳" : wx.city);
                }
                if (ui->screensaver_weather_label) {
                    snprintf(buf, sizeof(buf), "%s · %d°C",
                        wx.condition_cn[0] ? wx.condition_cn :
                            SMART_HOME_UI_DEFAULT_WEATHER, wx.temperature);
                    lv_label_set_text(ui->screensaver_weather_label, buf);
                }
                snprintf(buf, sizeof(buf), "%d°", wx.temperature);
                if (ui->home_weather_temp_label) {
                    lv_label_set_text(ui->home_weather_temp_label, buf);
                }
                if (ui->screensaver_temp_label) {
                    lv_label_set_text(ui->screensaver_temp_label, buf);
                }
                if (ui->screensaver_condition_label) {
                    lv_label_set_text(ui->screensaver_condition_label,
                        wx.condition_cn[0] ? wx.condition_cn :
                            SMART_HOME_UI_DEFAULT_WEATHER);
                }
                if (ui->home_weather_condition_label) {
                    lv_label_set_text(ui->home_weather_condition_label,
                        wx.condition_cn[0] ? wx.condition_cn :
                            SMART_HOME_UI_DEFAULT_WEATHER);
                }
                snprintf(buf, sizeof(buf), "%s · 湿度 %d%%\n风速 %d km/h",
                         wx.condition_cn, wx.humidity, wx.wind_kmph);
                if (ui->home_env_label) {
                    lv_label_set_text(ui->home_env_label, buf);
                }
                /* 图标随真实天气同步：晴=太阳，降水=云雨，其他保持云雨。 */
                home_weather_set_icon(ui, weather_icon_for(wx.condition_en));
            } else {
                if (ui->home_weather_temp_label)
                    lv_label_set_text(ui->home_weather_temp_label, "--°");
                if (ui->home_weather_city_label)
                    lv_label_set_text(ui->home_weather_city_label, "");
                if (ui->home_env_label)
                    lv_label_set_text(ui->home_env_label, SMART_HOME_UI_DEFAULT_WEATHER);
                if (ui->home_weather_condition_label)
                    lv_label_set_text(ui->home_weather_condition_label,
                                      SMART_HOME_UI_DEFAULT_WEATHER);
                if (ui->screensaver_weather_label)
                    lv_label_set_text(ui->screensaver_weather_label, SMART_HOME_UI_DEFAULT_WEATHER);
                if (ui->screensaver_temp_label)
                    lv_label_set_text(ui->screensaver_temp_label, "--°");
                if (ui->screensaver_condition_label)
                    lv_label_set_text(ui->screensaver_condition_label,
                                      SMART_HOME_UI_DEFAULT_WEATHER);
                home_weather_set_icon(ui, "asset:sun");
            }
        }
        if (ui->home_status_label) {
            if (reachable) {
                snprintf(text, sizeof(text), "在线 %u / 共 %u 台\n米家设备状态",
                         (unsigned)online_count, (unsigned)miloco_count);
            } else {
                snprintf(text, sizeof(text), "设备状态未知\n网关未连接");
            }
            lv_label_set_text(ui->home_status_label, text);
        }
    }
#else
    if (ui->home_ac_label) {
        snprintf(text, sizeof(text), "%d 台设备已接入\n点击进入设备管理",
                 smart_home_device_count(state));
        lv_label_set_text(ui->home_ac_label, text);
    }
    if (ui->home_status_label) {
        snprintf(text, sizeof(text), "%d 个演示设备开启\n本地演示状态", on_count);
        lv_label_set_text(ui->home_status_label, text);
    }
#endif
    smart_home_lvgl_v9_refresh_views(ui);
    (void)on_count;
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

static void home_time_timer_cb(lv_timer_t *timer)
{
    smart_home_lvgl_t *ui = lv_timer_get_user_data(timer);
    smart_home_lvgl_update_clock(ui);
    smart_home_lvgl_refresh_home(ui);
}

void smart_home_lvgl_build_home_screen(smart_home_lvgl_t *ui)
{
    lv_obj_t *screen;
    lv_obj_t *card;
    lv_obj_t *badge;
    lv_obj_t *label;
    int x = smart_home_lvgl_pad_x();
    int y = SMART_HOME_TOPBAR_H + 12;
    int gap = 12;
    int compact = smart_home_lvgl_compact();
    int weather_w = compact ? 154 : 210;
    int scene_w = compact ? 128 : 150;
    int primary_w = compact ? 220 : 360;
    int monitor_w = smart_home_lvgl_content_w() - weather_w - scene_w -
                    primary_w - gap * 3;
    int top_h = compact ? 126 : 190;
    int bottom_h = compact ? 112 : 178;

    if (!ui) {
        return;
    }
    if (monitor_w < (compact ? 130 : 180)) {
        monitor_w = compact ? 130 : 180;
    }

    screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    smart_home_lvgl_set_bg(screen, SMART_HOME_UI_COLOR_BG);
    ui->screen_home = screen;
    smart_home_lvgl_build_top_bar(screen, ui, "OpenVela HOME");

    card = home_card(screen, x, y, weather_w, top_h + bottom_h + gap,
                     SMART_HOME_UI_COLOR_SURFACE_SOFT);
    /* Keep location in the fixed top metadata block. The dynamic environment
     * summary occupies the bottom, so the two strings never share an anchor. */
    ui->home_date_label = smart_home_lvgl_label_create(card, "",
                                                       SMART_HOME_UI_COLOR_TEXT_SECONDARY, 13);
    lv_obj_align(ui->home_date_label, LV_ALIGN_TOP_LEFT, 0, 0);
    ui->home_time_label = smart_home_lvgl_label_create(card, SMART_HOME_UI_DEFAULT_CLOCK,
                                                       SMART_HOME_UI_COLOR_TEXT_PRIMARY, 32);
    lv_obj_align(ui->home_time_label, LV_ALIGN_TOP_LEFT, 0, 24);
    ui->home_weather_city_label = smart_home_lvgl_label_create(card, "",
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY, 13);
    lv_obj_align(ui->home_weather_city_label, LV_ALIGN_TOP_LEFT, 0, 70);
    ui->home_weather_badge = home_icon_badge(card, "asset:sun",
                                             lv_color_hex(0xEAF4F5), 76);
    /* Make the weather pictogram the visual anchor of the tall card. */
    lv_obj_align(ui->home_weather_badge, LV_ALIGN_CENTER, 0, -10);
    ui->home_weather_temp_label =
        smart_home_lvgl_label_create(card, "--°", SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                      32);
    lv_obj_align(ui->home_weather_temp_label, LV_ALIGN_CENTER, 0, 42);
    ui->home_env_label = smart_home_lvgl_label_create(card, "",
                                                      SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                                      14);
    lv_obj_set_width(ui->home_env_label, lv_pct(100));
    lv_label_set_long_mode(ui->home_env_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->home_env_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    card = home_card(screen, x + weather_w + gap, y, scene_w, top_h,
                     SMART_HOME_UI_COLOR_SURFACE_ON);
    home_icon_badge(card, ICON_NAV_HOME, lv_color_hex(0xFFF8F1), 48);
    home_feature_text(card, "家庭模式", "选择模式并发起请求",
                      SMART_HOME_UI_COLOR_PRIMARY_DARK);
    home_make_clickable(card, ui, HOME_ACTION_SCENES);

    card = home_card(screen, x + weather_w + gap + scene_w + gap, y,
                     primary_w, top_h, SMART_HOME_UI_COLOR_SURFACE);
    home_icon_badge(card, ICON_MIJIA, lv_color_hex(0xFFF8F4), 48);
    label = smart_home_lvgl_label_create(card, "米家设备",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 18);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 58, 0);
    ui->home_miloco_sub_label = smart_home_lvgl_label_create(
        card, "", SMART_HOME_UI_COLOR_TEXT_SECONDARY, 13);
    lv_obj_align(ui->home_miloco_sub_label, LV_ALIGN_TOP_LEFT, 58, 25);
    ui->home_ac_label = smart_home_lvgl_label_create(card, "",
                                                     SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                                     16);
    lv_obj_align(ui->home_ac_label, LV_ALIGN_CENTER, 0, 16);
    home_make_clickable(card, ui, HOME_ACTION_MIHOME);

    card = home_card(screen, x + weather_w + gap + scene_w + gap + primary_w + gap,
                     y, monitor_w, top_h, SMART_HOME_UI_COLOR_SURFACE_SOFT);
    badge = home_icon_badge(card, ICON_STATUS_CAMERA, lv_color_hex(0xE9F4F1), 64);
    lv_obj_align(badge, LV_ALIGN_CENTER, 0, -6);
    home_feature_text(card, "家庭安防",
#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
                      "查看摄像头预览 ›",
#else
                      "摄像头预览未启用",
#endif
                      SMART_HOME_UI_COLOR_TEXT_PRIMARY);
    home_make_clickable(card, ui, HOME_ACTION_SECURITY);

    card = home_card(screen, x + weather_w + gap, y + top_h + gap, scene_w,
                     bottom_h, SMART_HOME_UI_COLOR_SURFACE);
    home_icon_badge(card, ICON_NAV_CHAT, lv_color_hex(0xF2F0FF), 54);
    home_feature_text(card, "智能管家", "问问家庭状态",
                      SMART_HOME_UI_COLOR_TEXT_PRIMARY);
    home_make_clickable(card, ui, HOME_ACTION_AGENT);

    card = home_card(screen, x + weather_w + gap + scene_w + gap,
                     y + top_h + gap, primary_w, bottom_h,
                     SMART_HOME_UI_COLOR_SURFACE_ON);
    home_icon_badge(card, ICON_NAV_DEVICES, lv_color_hex(0xFFF8F2), 48);
    home_feature_text(card, "我的设备", "查看接入设备与状态",
                      SMART_HOME_UI_COLOR_TEXT_PRIMARY);
    home_make_clickable(card, ui, HOME_ACTION_DEVICES);

    card = home_card(screen, x + weather_w + gap + scene_w + gap + primary_w + gap,
                     y + top_h + gap, monitor_w, bottom_h,
                     SMART_HOME_UI_COLOR_SURFACE);
    home_icon_badge(card, ICON_NAV_SECURITY, lv_color_hex(0xEDF8F3), 54);
    label = smart_home_lvgl_label_create(card, "设备概况",
        SMART_HOME_UI_COLOR_TEXT_PRIMARY, 16);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 64);
    ui->home_status_label = smart_home_lvgl_label_create(card, "",
                                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                                         14);
    lv_obj_set_width(ui->home_status_label, lv_pct(100));
    lv_label_set_long_mode(ui->home_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->home_status_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    home_make_clickable(card, ui, HOME_ACTION_DEVICES);

    smart_home_lvgl_build_nav_bar(screen, ui);
    smart_home_lvgl_update_clock(ui);
    ui->home_time_timer = lv_timer_create(home_time_timer_cb, 30000, ui);
    smart_home_lvgl_refresh_home(ui);
}
