/* SPDX-License-Identifier: Apache-2.0 */
/** 首次配置引导浮层。实现说明见同名头文件。 */

#include "smart_home_lvgl_internal.h"
#include "smart_home_lvgl_onboarding.h"
#include "icons/smart_home_lvgl_png_icons.h"
#include "images/smart_home_icons.h"
#include "../../agent/smart_home_agent.h"

#include <stdio.h>
#include <string.h>

#define OB_PANEL_X   92
#define OB_PANEL_Y   62
#define OB_PANEL_W   840
#define OB_PANEL_H   476
#define OB_ROW_H     84
#define OB_ROW_GAP   12
#define OB_ICON_PX   26

typedef struct {
    const char *icon;
    const char *title;
    const char *desc;
    const char *todo_label;
    const char *done_label;
    int settings_category;   /* 点击跳转的设置分类索引 */
} ob_item_t;

/* 设置页分类顺序与 smart_home_lvgl_settings_v9.c 的 g_category_names 一致：
 * 0 显示与声音 / 1 网络连接 / 2 模型服务 / 3 语音服务 / 4 工具权限 / 5 设备与服务 / 6 系统健康 */
static const ob_item_t g_ob_items[3] = {
    { "asset:wifi",     "家庭网络", "连接 2.4 GHz Wi-Fi，天气与在线服务依赖它",
      "未连接", "已连接", 1 },
    { "asset:MIJIA",    "米家网关", "绑定米家账号，接入家中真实设备",
      "未绑定", "已绑定", 5 },
    { "asset:sparkle-2", "模型服务", "配置大模型接口，聊天与语音对话依赖它",
      "未配置", "已配置", 2 },
};

typedef struct {
    lv_obj_t *modal;
    lv_obj_t *row_state[3];
    bool skipped;
    bool presented;
} ob_state_t;

static ob_state_t g_ob;

/* ── 状态查询 ─────────────────────────────────────────────── */

void smart_home_lvgl_onboarding_status(const smart_home_lvgl_t *ui,
                                       smart_home_onboarding_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    if (!ui || !ui->app) {
        out->pending = 3;
        return;
    }

    /* 联网状态取自 agent 的系统状态，而不是 smart_home_wifi_is_connected()：
     * ESP_HOSTED 板级配置下 smart_home_wifi.c 不参与编译（走 hosted 网络栈），
     * 直接引用会使固件链接失败。system_status 在两种后端下都存在。 */
    out->wifi_ready = ui->app->system_status.network_status.online;

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (ui->app->miloco) {
        out->miloco_ready = smart_home_miloco_reachable(ui->app->miloco) &&
                            smart_home_miloco_bound(ui->app->miloco);
    }
#endif

    out->model_ready =
        ui->app->system_status.model_status == AGENT_OK;

    out->pending = (out->wifi_ready ? 0 : 1) +
                   (out->miloco_ready ? 0 : 1) +
                   (out->model_ready ? 0 : 1);
}

bool smart_home_lvgl_onboarding_should_present(const smart_home_lvgl_t *ui)
{
    smart_home_onboarding_status_t status;

    if (g_ob.presented || g_ob.skipped) {
        return false;
    }
    smart_home_lvgl_onboarding_status(ui, &status);
    return status.pending > 0;
}

bool smart_home_lvgl_onboarding_visible(void)
{
    return g_ob.modal != NULL;
}

/* ── 绘制 ─────────────────────────────────────────────────── */

static void ob_stop_bubbling(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
}

static void ob_dismiss_cb(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
    smart_home_lvgl_onboarding_dismiss(
        (smart_home_lvgl_t *)lv_event_get_user_data(event), true);
}

static void ob_row_click_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    lv_obj_t *row = lv_event_get_current_target(event);
    int index = (int)(intptr_t)lv_obj_get_user_data(row);

    if (!ui || index < 0 || index >= 3) {
        return;
    }

    /* 关掉浮层再跳转，避免它盖住设置页。跳转后用户从设置页自行返回，
     * 不再自动弹回总览。 */
    smart_home_lvgl_onboarding_dismiss(ui, false);

    smart_home_lvgl_load_tab(ui, SMART_HOME_TAB_SETTINGS);
    if (ui->screen_settings_v9) {
        (void)smart_home_lvgl_v9_settings_select_category(
            g_ob_items[index].settings_category);
    }
}

static void ob_set_row_state(lv_obj_t *row, const ob_item_t *def, bool ready)
{
    lv_obj_t *chip;
    lv_obj_t *dot;
    lv_obj_t *text;

    if (!row) {
        return;
    }
    /* 行内布局固定：图标 / 标题 / 描述 / 状态胶囊 / 按钮 */
    chip = lv_obj_get_child(row, 3);
    if (!chip) {
        return;
    }
    dot = lv_obj_get_child(chip, 0);
    text = lv_obj_get_child(chip, 1);

    lv_obj_set_style_bg_color(chip, ready ? lv_color_hex(0xE6F0EA)
                                          : lv_color_hex(0xF6EEE2), 0);
    if (dot) {
        lv_obj_set_style_bg_color(dot, ready ? SMART_HOME_UI_COLOR_SUCCESS
                                             : SMART_HOME_UI_COLOR_WARNING, 0);
    }
    if (text) {
        lv_label_set_text(text, ready ? def->done_label : def->todo_label);
        lv_obj_set_style_text_color(text, ready ? SMART_HOME_UI_COLOR_SUCCESS
                                               : lv_color_hex(0x8A6E48), 0);
        lv_obj_set_width(text, 64);
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static lv_obj_t *ob_make_row(lv_obj_t *parent, int index,
                             const ob_item_t *def, bool ready,
                             smart_home_lvgl_t *ui)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_t *box;
    lv_obj_t *icon;
    lv_obj_t *label;
    lv_obj_t *chip;
    lv_obj_t *dot;
    lv_obj_t *state_text;
    lv_obj_t *button;

    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, OB_PANEL_W - 64, OB_ROW_H);
    lv_obj_set_pos(row, 0, index * (OB_ROW_H + OB_ROW_GAP));
    lv_obj_set_style_radius(row, 12, 0);
    smart_home_lvgl_set_bg(row, SMART_HOME_UI_COLOR_SURFACE_SOFT);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(row, (void *)(intptr_t)index);
    lv_obj_add_event_cb(row, ob_row_click_cb, LV_EVENT_CLICKED, ui);
    smart_home_lvgl_press_feedback(row);

    /* 图标底座 48x48 */
    box = lv_obj_create(row);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 48, 48);
    lv_obj_set_pos(box, 18, 18);
    lv_obj_set_style_radius(box, 12, 0);
    smart_home_lvgl_set_bg(box, SMART_HOME_UI_COLOR_SURFACE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    icon = lv_image_create(box);
    lv_image_set_src(icon, smart_home_lvgl_png_icon_get(def->icon, OB_ICON_PX));
    lv_obj_set_pos(icon, 11, 11);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    if (lv_image_get_src(icon)) {
        smart_home_lvgl_icon_color(icon, SMART_HOME_UI_COLOR_TEXT_PRIMARY);
    }

    label = smart_home_lvgl_label_create(row, def->title,
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 18);
    lv_obj_set_pos(label, 84, 18);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);

    label = smart_home_lvgl_label_create(row, def->desc,
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY, 14);
    lv_obj_set_pos(label, 84, 46);
    lv_obj_set_width(label, 380);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);

    /* 状态胶囊：宽 92 高 26，右侧与按钮间隔 18 */
    chip = lv_obj_create(row);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, 92, 26);
    lv_obj_set_pos(chip, OB_PANEL_W - 64 - 18 - 118 - 18 - 92, 29);
    lv_obj_set_style_radius(chip, 13, 0);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    dot = lv_obj_create(chip);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_pos(dot, 12, 10);
    lv_obj_set_style_radius(dot, 3, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    state_text = smart_home_lvgl_label_create(chip, "",
                                              SMART_HOME_UI_COLOR_TEXT_SECONDARY, 14);
    lv_obj_set_pos(state_text, 22, 5);
    lv_obj_clear_flag(state_text, LV_OBJ_FLAG_CLICKABLE);

    button = lv_button_create(row);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 118, 40);
    lv_obj_set_pos(button, OB_PANEL_W - 64 - 18 - 118, 22);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, SMART_HOME_UI_COLOR_BORDER, 0);
    smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_SURFACE);
    lv_obj_set_ext_click_area(button, 4);
    lv_obj_add_event_cb(button, ob_row_click_cb, LV_EVENT_CLICKED, ui);
    lv_obj_set_user_data(button, (void *)(intptr_t)index);
    smart_home_lvgl_press_feedback(button);
    label = smart_home_lvgl_label_create(button, ready ? "重新配置" : "去配置",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 16);
    lv_obj_center(label);

    ob_set_row_state(row, def, ready);
    return row;
}

void smart_home_lvgl_onboarding_present(smart_home_lvgl_t *ui)
{
    smart_home_onboarding_status_t status;
    lv_obj_t *modal;
    lv_obj_t *panel;
    lv_obj_t *label;
    lv_obj_t *button;
    lv_obj_t *rows;
    bool ready[3];
    int i;

    if (!ui || !ui->screen_home || g_ob.modal) {
        return;
    }
    smart_home_lvgl_onboarding_status(ui, &status);
    if (status.pending == 0) {
        return;
    }

    ready[0] = status.wifi_ready;
    ready[1] = status.miloco_ready;
    ready[2] = status.model_ready;

    modal = lv_obj_create(ui->screen_home);
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, smart_home_lvgl_disp_w(), smart_home_lvgl_disp_h());
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, 130, 0);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, ob_stop_bubbling, LV_EVENT_CLICKED, NULL);
    g_ob.modal = modal;
    g_ob.presented = true;

    panel = lv_obj_create(modal);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, OB_PANEL_W, OB_PANEL_H);
    lv_obj_set_pos(panel, OB_PANEL_X, OB_PANEL_Y);
    smart_home_lvgl_card_style(panel);
    lv_obj_set_style_radius(panel, 18, 0);
    /* 1px 边框计入内容区；固定布局不可因按钮聚焦而自动滚动。 */
    lv_obj_set_style_pad_all(panel, 31, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, ob_stop_bubbling, LV_EVENT_CLICKED, NULL);

    label = smart_home_lvgl_label_create(panel, "开始使用",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 28);
    lv_obj_set_pos(label, 0, 0);

    label = smart_home_lvgl_label_create(
        panel,
        "这三项让设备完整工作。可以现在配置，也可以稍后在设置中完成。",
        SMART_HOME_UI_COLOR_TEXT_SECONDARY, 15);
    lv_obj_set_pos(label, 0, 40);
    lv_obj_set_width(label, OB_PANEL_W - 64);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

    rows = lv_obj_create(panel);
    lv_obj_remove_style_all(rows);
    lv_obj_set_size(rows, OB_PANEL_W - 64, 3 * OB_ROW_H + 2 * OB_ROW_GAP);
    lv_obj_set_pos(rows, 0, 78);
    lv_obj_set_style_pad_all(rows, 0, 0);
    lv_obj_clear_flag(rows, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(rows, LV_OBJ_FLAG_CLICKABLE);

    for (i = 0; i < 3; i++) {
        g_ob.row_state[i] = ob_make_row(rows, i, &g_ob_items[i], ready[i], ui);
    }

    label = smart_home_lvgl_label_create(
        panel, "稍后可在「设置」中完成，首页会显示待配置提示",
        SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
    lv_obj_set_pos(label, 0, OB_PANEL_H - 64 - 32 - 22);

    button = lv_button_create(panel);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 150, 44);
    lv_obj_set_pos(button, OB_PANEL_W - 64 - 150, OB_PANEL_H - 64 - 44);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, SMART_HOME_UI_COLOR_BORDER, 0);
    smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_SURFACE);
    lv_obj_set_ext_click_area(button, 4);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(button, ob_dismiss_cb, LV_EVENT_CLICKED, ui);
    smart_home_lvgl_press_feedback(button);
    label = smart_home_lvgl_label_create(button, "稍后再说",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 17);
    lv_obj_center(label);
}

void smart_home_lvgl_onboarding_dismiss(smart_home_lvgl_t *ui, bool skipped)
{
    (void)ui;
    if (skipped) {
        g_ob.skipped = true;
    }
    if (g_ob.modal) {
        lv_obj_delete(g_ob.modal);
        g_ob.modal = NULL;
    }
    memset(g_ob.row_state, 0, sizeof(g_ob.row_state));
}

void smart_home_lvgl_onboarding_reset(void)
{
    smart_home_lvgl_onboarding_dismiss(NULL, false);
    g_ob.skipped = false;
    g_ob.presented = false;
}
