/** V9 Settings shell: six internal categories, summary panels and routes. */

#include "smart_home_lvgl_internal.h"
#include "images/smart_home_icons.h"
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
#include "../../miloco/smart_home_miloco.h"
#endif

#include <stdio.h>
#include <string.h>

#define V9_SETTINGS_CATEGORIES 6

static const char *const g_category_names[V9_SETTINGS_CATEGORIES] = {
    "显示与声音", "网络连接", "模型服务", "工具权限", "设备与服务", "系统健康"
};

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *panel[V9_SETTINGS_CATEGORIES];
    lv_obj_t *button[V9_SETTINGS_CATEGORIES];
    lv_obj_t *services_modal;
    char service_status[3][32];
    int category;
} v9_settings_t;

static v9_settings_t g_settings;

static void panel_show(int category)
{
    if (category < 0 || category >= V9_SETTINGS_CATEGORIES) {
        return;
    }
    g_settings.category = category;
    for (int i = 0; i < V9_SETTINGS_CATEGORIES; i++) {
        lv_obj_t *btn = g_settings.button[i];
        lv_obj_t *label;
        if (btn) {
            label = lv_obj_get_child(btn, 0);
            if (i == category) {
                lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(btn, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
                if (label) lv_obj_set_style_text_color(label,
                    SMART_HOME_UI_COLOR_TEXT_PRIMARY, 0);
            } else {
                lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
                if (label) lv_obj_set_style_text_color(label,
                    SMART_HOME_UI_COLOR_TEXT_SECONDARY, 0);
            }
        }
        if (g_settings.panel[i]) {
            if (i == category) {
                lv_obj_clear_flag(g_settings.panel[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(g_settings.panel[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void category_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);

    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        panel_show(index);
    }
}

static lv_obj_t *make_panel(lv_obj_t *right)
{
    lv_obj_t *panel = lv_obj_create(right);

    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
    return panel;
}

static void panel_title(lv_obj_t *panel, const char *text)
{
    lv_obj_t *label = smart_home_lvgl_label_create(
        panel, text, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 22);
    lv_obj_set_pos(label, 0, 0);
}

static void panel_row(lv_obj_t *panel, int y, const char *name,
                      const char *value)
{
    lv_obj_t *label = smart_home_lvgl_label_create(
        panel, name, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 15);
    lv_obj_t *value_label;
    lv_obj_t *line;

    lv_obj_set_pos(label, 0, y);
    value_label = smart_home_lvgl_label_create(
        panel, value, SMART_HOME_UI_COLOR_TEXT_SECONDARY, 15);
    lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, 0, y);

    line = lv_obj_create(panel);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, lv_pct(100), 1);
    lv_obj_set_pos(line, 0, y + 36);
    lv_obj_set_style_bg_color(line, SMART_HOME_UI_COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
}

static void route_cb(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_user_data(event);

    if (lv_event_get_code(event) == LV_EVENT_CLICKED && target) {
        lv_scr_load(target);
    }
}

static void panel_button(lv_obj_t *panel, const char *text, lv_obj_t *target)
{
    lv_obj_t *button = lv_button_create(panel);
    lv_obj_t *label;

    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 180, 40);
    lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    smart_home_lvgl_press_feedback(button);
    lv_obj_add_event_cb(button, route_cb, LV_EVENT_CLICKED, target);

    label = smart_home_lvgl_label_create(button, text,
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 14);
    lv_obj_center(label);
}

static const char *state_text(int ready)
{
    return ready ? "已就绪" : "未就绪";
}

static void build_display_panel(lv_obj_t *panel, lv_obj_t *settings_target)
{
    panel_title(panel, "显示与声音");
    panel_row(panel, 56, "屏幕亮度", "由板级接口提供");
    panel_row(panel, 106, "播报音量", "由音频服务提供");
    panel_row(panel, 156, "自动进入屏保", "5 分钟");
    panel_row(panel, 206, "设备状态动效", "开启");
    panel_button(panel, "打开完整设置", settings_target);
}

static void build_network_panel(smart_home_lvgl_t *ui, lv_obj_t *panel)
{
    int online = ui && ui->app && ui->app->system_status.network_status.online;

    panel_title(panel, "无线网络");
    panel_row(panel, 56, "家庭网络", online ? "已连接" : "未连接");
    panel_row(panel, 106, "外部网络", "由网络服务提供");
    panel_row(panel, 156, "Wi-Fi 凭据", "不回显");
    panel_button(panel, "打开网络设置", ui ? ui->screen_network : NULL);
}

static void build_model_panel(smart_home_lvgl_t *ui, lv_obj_t *panel)
{
    int ready = ui && ui->app &&
                ui->app->system_status.model_status == AGENT_OK;

    panel_title(panel, "模型服务");
    panel_row(panel, 56, "当前服务", state_text(ready));
    panel_row(panel, 106, "模型标识", "由模型配置提供");
    panel_row(panel, 156, "API 密钥", "不回显");
    panel_button(panel, "打开模型服务", ui ? ui->screen_model : NULL);
}

static void build_tools_panel(smart_home_lvgl_t *ui, lv_obj_t *panel)
{
    int ready = ui && ui->app &&
                ui->app->system_status.tools_status == AGENT_OK;

    panel_title(panel, "工具权限");
    panel_row(panel, 56, "本地工具", state_text(ready));
    panel_row(panel, 106, "外部工具", "随服务连接");
    panel_row(panel, 156, "权限生效范围", "本次运行有效");
    panel_button(panel, "打开工具与权限", ui ? ui->screen_tools : NULL);
}

static void services_modal_close(void)
{
    if (g_settings.services_modal) {
        lv_obj_delete(g_settings.services_modal);
        g_settings.services_modal = NULL;
    }
}

static void services_mask_cb(lv_event_t *event)
{
    (void)event;
    services_modal_close();
}

static void services_stop_cb(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
}

static void services_modal_open(smart_home_lvgl_t *ui)
{
    lv_obj_t *mask;
    lv_obj_t *panel;
    lv_obj_t *label;
    static const char *const names[3] = {
        "米家网关", "MCP 服务", "局域网 Node"
    };

    if (!g_settings.screen) {
        return;
    }
    services_modal_close();
    mask = lv_obj_create(g_settings.screen);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, smart_home_lvgl_disp_w(), smart_home_lvgl_disp_h());
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_30, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mask, services_mask_cb, LV_EVENT_CLICKED, NULL);
    g_settings.services_modal = mask;

    panel = lv_obj_create(mask);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 680, 360);
    lv_obj_center(panel);
    smart_home_lvgl_card_style(panel);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_pad_all(panel, 24, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, services_stop_cb, LV_EVENT_CLICKED, NULL);

    label = smart_home_lvgl_label_create(panel, "设备与服务",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 22);
    lv_obj_set_pos(label, 0, 0);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *value;
        int y = 56 + i * 46;

        label = smart_home_lvgl_label_create(panel, names[i],
                                             SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                             16);
        lv_obj_set_pos(label, 0, y);
        value = smart_home_lvgl_label_create(panel,
                                             g_settings.service_status[i],
                                             SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                             14);
        lv_obj_align(value, LV_ALIGN_TOP_RIGHT, 0, y);
    }

    {
        lv_obj_t *button = lv_button_create(panel);
        lv_obj_t *button_label;
        lv_obj_t *targets[3] = {
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
            ui && ui->screen_miloco ? ui->screen_miloco : NULL,
#else
            NULL,
#endif
            ui && ui->screen_mcp ? ui->screen_mcp : NULL,
            ui && ui->screen_node ? ui->screen_node : NULL
        };
        static const char *const captions[3] = { "打开米家", "打开 MCP", "打开 Node" };

        for (int i = 0; i < 3; i++) {
            button = lv_button_create(panel);
            lv_obj_remove_style_all(button);
            lv_obj_set_size(button, 120, 36);
            lv_obj_set_pos(button, i * 132, 290);
            lv_obj_set_style_radius(button, 8, 0);
            lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
            lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
            smart_home_lvgl_press_feedback(button);
            lv_obj_add_event_cb(button, route_cb, LV_EVENT_CLICKED,
                                targets[i]);
            button_label = smart_home_lvgl_label_create(
                button, captions[i], SMART_HOME_UI_COLOR_TEXT_PRIMARY, 13);
            lv_obj_center(button_label);
        }
    }

    {
        lv_obj_t *button = lv_button_create(panel);
        lv_obj_t *button_label;

        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, 100, 36);
        lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_set_style_radius(button, 8, 0);
        lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(button, services_mask_cb, LV_EVENT_CLICKED, NULL);
        button_label = smart_home_lvgl_label_create(
            button, "关闭", SMART_HOME_UI_COLOR_TEXT_PRIMARY, 13);
        lv_obj_center(button_label);
    }
}

static void services_modal_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);

    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        services_modal_open(ui);
    }
}

static void build_services_panel(smart_home_lvgl_t *ui, lv_obj_t *panel)
{
    const char *miloco = "未启用";
    const char *mcp = "未启用";
    const char *node = "未启用";

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (ui && ui->app && ui->app->miloco) {
        if (!smart_home_miloco_reachable(ui->app->miloco)) {
            miloco = "未连接";
        } else if (smart_home_miloco_bound(ui->app->miloco)) {
            miloco = "已绑定";
        } else {
            miloco = "待绑定";
        }
    } else {
        miloco = "未连接";
    }
#endif
#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
    mcp = (ui && ui->app &&
           ui->app->system_status.mcp_bridge_status == AGENT_OK) ?
          "已就绪" : "未就绪";
#endif
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
    node = (ui && ui->app &&
            ui->app->system_status.node_gateway_status == AGENT_OK) ?
           "监听中" : "未开启";
#endif

    snprintf(g_settings.service_status[0], sizeof(g_settings.service_status[0]),
             "%s", miloco);
    snprintf(g_settings.service_status[1], sizeof(g_settings.service_status[1]),
             "%s", mcp);
    snprintf(g_settings.service_status[2], sizeof(g_settings.service_status[2]),
             "%s", node);
    panel_title(panel, "设备与服务");
    panel_row(panel, 56, "米家网关", miloco);
    panel_row(panel, 106, "MCP 服务", mcp);
    panel_row(panel, 156, "局域网 Node", node);

    {
        lv_obj_t *button = lv_button_create(panel);
        lv_obj_t *label;

        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, 180, 40);
        lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_set_style_radius(button, 10, 0);
        lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        smart_home_lvgl_press_feedback(button);
        lv_obj_add_event_cb(button, services_modal_cb, LV_EVENT_CLICKED, ui);
        label = smart_home_lvgl_label_create(button, "查看连接状态",
                                             SMART_HOME_UI_COLOR_TEXT_PRIMARY, 14);
        lv_obj_center(label);
    }
}

static void build_health_panel(smart_home_lvgl_t *ui, lv_obj_t *panel)
{
    int net = ui && ui->app && ui->app->system_status.network_status.online;
    int model = ui && ui->app &&
                ui->app->system_status.model_status == AGENT_OK;
    int tools = ui && ui->app &&
                ui->app->system_status.tools_status == AGENT_OK;

    panel_title(panel, "系统健康");
    panel_row(panel, 56, "家庭网络", net ? "已连接互联网" : "未连接");
    panel_row(panel, 106, "模型服务", state_text(model));
    panel_row(panel, 156, "本地工具", state_text(tools));
    panel_row(panel, 206, "最近错误", "无");
    panel_button(panel, "打开系统健康", ui ? ui->screen_settings : NULL);
}

void smart_home_lvgl_build_settings_screen_v9(smart_home_lvgl_t *ui)
{
    lv_obj_t *screen;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *left;
    lv_obj_t *right;

    if (!ui) {
        return;
    }
    memset(&g_settings, 0, sizeof(g_settings));

    screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    smart_home_lvgl_set_bg(screen, SMART_HOME_UI_COLOR_BG);
    ui->screen_settings_v9 = screen;
    g_settings.screen = screen;
    smart_home_lvgl_build_top_bar(screen, ui, "设置");

    title = smart_home_lvgl_label_create(screen, "设置",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 24);
    lv_obj_set_pos(title, 24, 76);
    subtitle = smart_home_lvgl_label_create(screen, "连接家庭，按习惯调整",
                                            SMART_HOME_UI_COLOR_TEXT_SECONDARY, 14);
    lv_obj_set_pos(subtitle, 24, 110);

    left = lv_obj_create(screen);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, 208, 384);
    lv_obj_set_pos(left, 24, 136);
    smart_home_lvgl_card_style(left);
    lv_obj_set_style_radius(left, 18, 0);
    lv_obj_set_style_pad_all(left, 0, 0);

    right = lv_obj_create(screen);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, 752, 384);
    lv_obj_set_pos(right, 248, 136);
    smart_home_lvgl_card_style(right);
    lv_obj_set_style_radius(right, 18, 0);
    lv_obj_set_style_pad_all(right, 24, 0);

    for (int i = 0; i < V9_SETTINGS_CATEGORIES; i++) {
        lv_obj_t *button = lv_button_create(left);
        lv_obj_t *label;

        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, 184, 50);
        lv_obj_set_pos(button, 12, 12 + i * 58);
        lv_obj_set_style_radius(button, 12, 0);
        lv_obj_set_user_data(button, (void *)(intptr_t)i);
        lv_obj_add_event_cb(button, category_cb, LV_EVENT_CLICKED, NULL);
        smart_home_lvgl_press_feedback(button);
        g_settings.button[i] = button;

        label = smart_home_lvgl_label_create(button, g_category_names[i],
                                             SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                             16);
        lv_obj_set_pos(label, 34, 14);
    }

    g_settings.panel[0] = make_panel(right);
    g_settings.panel[1] = make_panel(right);
    g_settings.panel[2] = make_panel(right);
    g_settings.panel[3] = make_panel(right);
    g_settings.panel[4] = make_panel(right);
    g_settings.panel[5] = make_panel(right);

    build_display_panel(g_settings.panel[0], ui->screen_settings);
    build_network_panel(ui, g_settings.panel[1]);
    build_model_panel(ui, g_settings.panel[2]);
    build_tools_panel(ui, g_settings.panel[3]);
    build_services_panel(ui, g_settings.panel[4]);
    build_health_panel(ui, g_settings.panel[5]);

    panel_show(0);
    smart_home_lvgl_build_nav_bar(screen, ui);
}

int smart_home_lvgl_v9_settings_category_count(void)
{
    return V9_SETTINGS_CATEGORIES;
}

int smart_home_lvgl_v9_settings_select_category(int index)
{
    if (index < 0 || index >= V9_SETTINGS_CATEGORIES) {
        return -1;
    }
    panel_show(index);
    return index;
}
