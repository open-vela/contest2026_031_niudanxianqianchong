/** SmartHome five-item product navigation. */

#include "smart_home_lvgl_internal.h"
#include "images/smart_home_icons.h"

#include <stdint.h>

static const char *const g_nav_titles[SMART_HOME_TAB_COUNT] = {
    "首页", "设备", "聊天", "安防", "更多"
};

static const char *const g_nav_icons[SMART_HOME_TAB_COUNT] = {
    ICON_NAV_HOME, ICON_NAV_DEVICES, ICON_NAV_CHAT, ICON_NAV_SECURITY,
    ICON_NAV_MORE
};

static int tab_for_screen(const smart_home_lvgl_t *ui, const lv_obj_t *screen)
{
    if (!ui || !screen) return SMART_HOME_TAB_HOME;
    if (screen == ui->screen_panel) return SMART_HOME_TAB_DEVICES;
    if (screen == ui->screen_chat) return SMART_HOME_TAB_CHAT;
    if (screen == ui->screen_security) return SMART_HOME_TAB_SECURITY;
    if (screen == ui->screen_more || screen == ui->screen_settings ||
        screen == ui->screen_network
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
        || screen == ui->screen_miloco
        || screen == ui->screen_miloco_bind
#endif
    ) {
        return SMART_HOME_TAB_MORE;
    }
    return SMART_HOME_TAB_HOME;
}

void smart_home_lvgl_load_tab(smart_home_lvgl_t *ui, int tab)
{
    lv_obj_t *screen = NULL;
    int previous_tab;

    if (!ui || tab < 0 || tab >= SMART_HOME_TAB_COUNT) {
        return;
    }
    switch (tab) {
    case SMART_HOME_TAB_HOME:     screen = ui->screen_home; break;
    case SMART_HOME_TAB_DEVICES:  screen = ui->screen_panel; break;
    case SMART_HOME_TAB_CHAT:     screen = ui->screen_chat; break;
    case SMART_HOME_TAB_SECURITY: screen = ui->screen_security; break;
    case SMART_HOME_TAB_MORE:     screen = ui->screen_more; break;
    default: break;
    }
    if (!screen) return;

    /* The P4 camera service is explicitly opt-in on the Security page. Do
     * not retain CSI, ISP and PSRAM frame buffers while the user works in
     * chat/settings or another product page. */
    if (ui->active_tab == SMART_HOME_TAB_SECURITY &&
        tab != SMART_HOME_TAB_SECURITY) {
        smart_home_lvgl_security_camera_stop(ui);
    }
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    /* 米家轮询在设备页/首页可见时运行，其余页面停止。 */
    smart_home_lvgl_miloco_poll_set_enabled(
        ui, tab == SMART_HOME_TAB_DEVICES || tab == SMART_HOME_TAB_HOME);
#endif
    previous_tab = ui->active_tab;
    ui->active_tab = tab;
    if (tab == SMART_HOME_TAB_HOME) {
        /* 首页统计（网络/米家设备数）在切回时取最新值。 */
        smart_home_lvgl_refresh_home(ui);
    }
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (tab == SMART_HOME_TAB_DEVICES && previous_tab != tab &&
        ui->app && ui->app->miloco) {
        /* 设备页的米家轮询定时器在其他页面停摆；在聊天页让 agent
         * 控制设备后切回时，revision 差额要等 1s 定时器首轮才追平。
         * 进入设备页立即同步一次，卡片/开关即时反映最新状态。 */
        uint32_t revision = 0;

        (void)smart_home_miloco_list(ui->app->miloco, NULL, 0, &revision);
        if (revision != ui->miloco_revision) {
            smart_home_lvgl_refresh_cards(ui);
        }
    }
#endif

    /* Chat is entered from three independent product paths.  Load it
     * directly rather than queuing another screen animation: on the P4
     * target this makes the transition deterministic when a previous
     * animation is still winding down after a touch event. */
    if (tab == SMART_HOME_TAB_CHAT) {
        lv_scr_load(screen);
        return;
    }

    /* Waking from the screensaver is a state change, not a lateral
     * navigation: fade in instead of sliding from a navigation side. */
    if (lv_scr_act() == ui->screen_screensaver) {
        lv_scr_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_IN, 220, 0, false);
        return;
    }

    if (lv_scr_act() == screen) {
        return;
    }

    /* Match the slide direction to the tab order: moving to a higher tab
     * reads as forward (new screen enters from the right), a lower tab as
     * going back (enters from the left). */
    lv_scr_load_anim(screen,
                     tab > previous_tab ? LV_SCR_LOAD_ANIM_MOVE_LEFT :
                                          LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                     180, 0, false);
}

static void nav_btn_click(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    int tab = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(event));
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        if (ui && tab == SMART_HOME_TAB_DEVICES && ui->panel_title) {
            lv_label_set_text(ui->panel_title, "我的设备");
        }
        smart_home_lvgl_load_tab(ui, tab);
    }
}

lv_obj_t *smart_home_lvgl_build_nav_bar(lv_obj_t *screen,
                                        smart_home_lvgl_t *ui)
{
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_t *content;
    int gap = smart_home_lvgl_compact() ? 4 : 12;
    int button_w = smart_home_lvgl_compact() ? 70 : 96;
    int content_w = button_w * SMART_HOME_TAB_COUNT +
                    gap * (SMART_HOME_TAB_COUNT - 1);
    int active = tab_for_screen(ui, screen);

    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, smart_home_lvgl_disp_w(), SMART_HOME_NAV_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    smart_home_lvgl_set_bg(bar, SMART_HOME_UI_COLOR_SURFACE);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, SMART_HOME_UI_COLOR_BORDER, 0);

    content = lv_obj_create(bar);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, content_w, SMART_HOME_NAV_H - 4);
    lv_obj_center(content);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_column(content, gap, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* The bar owns the full footer; the compact inner row keeps the five
     * touch targets visually calm instead of stretching each one edge-to-edge. */
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    /* Buttons are attached to the centred content row below. */
    bar = content;

    for (int i = 0; i < SMART_HOME_TAB_COUNT; i++) {
        lv_obj_t *button = lv_btn_create(bar);
        lv_obj_t *button_content;
        lv_color_t color = i == active ? SMART_HOME_UI_COLOR_PRIMARY_DARK :
                                         SMART_HOME_UI_COLOR_TEXT_MUTED;
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, button_w, SMART_HOME_NAV_H - 8);
        lv_obj_set_style_radius(button, 16, 0);
        lv_obj_set_user_data(button, (void *)(intptr_t)i);
        lv_obj_add_event_cb(button, nav_btn_click, LV_EVENT_CLICKED, ui);
        if (i == active) smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_SURFACE_ON);
        else lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        button_content = smart_home_lvgl_icon_with_text(button, g_nav_icons[i],
                                                  20, 20, g_nav_titles[i],
                                                  color, 11);
        /* The icon/text container is the actual hit target on some LVGL
         * input paths. Give it the same handler as its button so tapping
         * the Chat glyph or label always changes screen. */
        lv_obj_add_flag(button_content, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(button_content, (void *)(intptr_t)i);
        lv_obj_add_event_cb(button_content, nav_btn_click, LV_EVENT_CLICKED, ui);
        lv_obj_center(button_content);
    }
    return bar;
}
