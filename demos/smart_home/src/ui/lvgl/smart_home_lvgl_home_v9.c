/** V9 merged Home: weather header, room filter and horizontal device cards. */

#include "smart_home_lvgl_internal.h"
#include "images/smart_home_icons.h"
#include "icons/smart_home_lvgl_png_icons.h"
#include "smart_home_v9_device_view.h"
#include "smart_home_lvgl_device_dialog.h"
#include "smart_home_lvgl_camera_dialog.h"
#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
#include "../../camera/smart_home_camera_control.h"
#endif
#include "smart_home_lvgl_room_dialog.h"
#ifdef CONFIG_SMART_HOME_ONBOARDING
#include "smart_home_lvgl_onboarding.h"
#endif
#include "../../device/smart_home_device_service.h"
#include "../../smart_home_memory.h"
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
#include "../../miloco/smart_home_miloco.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef CONFIG_SMART_HOME_DEMO_DATA_ROOT
#define CONFIG_SMART_HOME_DEMO_DATA_ROOT "/data"
#endif

#define V9_DEVICE_COUNT 6
#define V9_MAX_CARDS    12
#define V9_ROOM_COUNT   5
#define V9_MAX_ROOMS (SMART_HOME_MAX_ROOMS + SMART_HOME_V9_MAX_VIEWS)
#define V9_CARD_W       176
#define V9_CARD_H       244
#define V9_CARD_GAP     16
/* 页头天气图标：128px 为 32px 源图的 4 倍整数放大，边缘不会出现
 * 非整数缩放造成的宽窄不均。y 取 58，顶部与日期行、底部与房间栏
 * 都留出余量，不与卡片区重叠。
 *
 * 水平位置紧贴温度块（设计稿的小太阳也在温度右侧紧邻处）：
 * 温度「22°」墨迹右边界 314，图标左边界 350，间距 36px 与设计稿
 * 的小太阳一致。若以后要改为靠右或居中，只需调 V9_WEATHER_ICON_X。
 */
#define V9_WEATHER_ICON_PX 128
#define V9_WEATHER_ICON_Y  58
#define V9_WEATHER_ICON_X  350
#define V9_VIEW_X       24
#define V9_VIEW_Y       260
#define V9_VIEW_W       976
#define V9_VIEW_H       256

/* 场景切换器：房间筛选行右侧，右边界与天气图标、卡片区共用 24px 边距。 */
#define V9_SCENE_BTN_X   844
#define V9_SCENE_BTN_Y   206
#define V9_SCENE_BTN_W   156
#define V9_SCENE_BTN_H   48
#define V9_SCENE_ITEM_H  40
#define V9_SCENE_MENU_PAD 6
#define V9_SCENE_ICON_PX 20
#define V9_SCENE_COUNT   4

typedef struct {
    const char *id;
    const char *name;
    const char *room;
    const char *status_on;
    const char *status_off;
    const char *metric_on;
    const char *metric_off;
    const char *caption;
    int art_w;
    int art_h;
    int art_x;
    int art_y;
    int on;
} v9_device_def_t;

static const v9_device_def_t g_v9_devices[V9_DEVICE_COUNT] = {
    { "light", "吊灯", "客厅", "已开启", "已关闭", "80%", "0%", "亮度",
      92, 108, 72, 122, 1 },
    { "ac", "立式空调", "客厅", "制冷中", "已关闭", "24°", "--", "目标温度",
      98, 138, 66, 94, 1 },
    { "washer", "洗衣机", "阳台", "洗涤中", "待机", "32分", "--", "剩余时间",
      99, 114, 65, 118, 1 },
    { "robot", "扫地机器人", "客厅", "清扫中", "待机", "75%", "--", "电量",
      101, 58, 63, 174, 1 },
    { "camera", "摄像头", "客厅", "预览中", "预览关闭", "在线", "在线", "设备状态",
      90, 116, 74, 118, 0 },
    { "fan", "落地风扇", "卧室", "运行中", "已关闭", "2档", "--", "风速",
      88, 124, 76, 108, 1 },
};

static const char *const g_v9_rooms[V9_ROOM_COUNT] = {
    "客厅", "卧室", "书房", "阳台", "厨房"
};

static const char *v9_art_state(const v9_device_def_t *def, int on,
                                const smart_home_v9_device_view_t *view)
{
    if (!strcmp(def->id, "camera")) {
        if (view) {
            if (!view->online) return "offline";
            if (view->source == SMART_HOME_V9_SOURCE_BOARD &&
                view->activity == SMART_HOME_V9_CAMERA_PREVIEWING) {
                return "patrol";
            }
            return "online";
        }

        /* The demo camera represents an available camera; the switch previews it. */
        return on ? "patrol" : "online";
    }
    return on ? "on" : "off";
}

/* ── 场景切换器 ──────────────────────────────────────────────
 * 四个场景的 scene id 必须与 res/skills/smart_home_scenes.md 及
 * miloco 的 g_miloco_scenes 一致，run_scene 才能解析。
 */
typedef struct {
    const char *id;
    const char *title;
    const char *icon;
} v9_scene_def_t;

static const v9_scene_def_t g_v9_scenes[V9_SCENE_COUNT] = {
    { "home",  "回家模式", ICON_NAV_HOME },
    { "movie", "观影模式", ICON_MEDIA_VIDEO },
    { "sleep", "睡眠模式", ICON_SCENE_SLEEP },
    { "away",  "离家模式", ICON_SCENE_AWAY },
};

static int v9_scene_index(const char *id)
{
    int i;

    if (!id) {
        return -1;
    }
    for (i = 0; i < V9_SCENE_COUNT; i++) {
        if (!strcmp(g_v9_scenes[i].id, id)) {
            return i;
        }
    }
    return -1;
}

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *view;
    lv_obj_t *empty;
    lv_obj_t *room_btn[V9_MAX_ROOMS + 1];
    lv_obj_t *room_strip;
    char rooms[V9_MAX_ROOMS][SMART_HOME_ROOM_NAME_SIZE];
    int room_count;
    lv_obj_t *card[V9_MAX_CARDS];
    lv_obj_t *room_label[V9_MAX_CARDS];
    lv_obj_t *name_label[V9_MAX_CARDS];
    lv_obj_t *status[V9_MAX_CARDS];
    lv_obj_t *metric[V9_MAX_CARDS];
    lv_obj_t *sw[V9_MAX_CARDS];
    lv_obj_t *art[V9_MAX_CARDS];
    char art_path[V9_MAX_CARDS][256];
    char key[V9_MAX_CARDS][SMART_HOME_V9_KEY_SIZE];
    char room_text[V9_MAX_CARDS][SMART_HOME_V9_ROOM_SIZE];
    char name_text[V9_MAX_CARDS][SMART_HOME_V9_NAME_SIZE];
    smart_home_v9_device_view_t device_view[V9_MAX_CARDS];
    int real[V9_MAX_CARDS];
    int on[V9_MAX_CARDS];
    int def_index[V9_MAX_CARDS];
    int source[V9_MAX_CARDS];
    int pending[V9_MAX_CARDS];
    int pending_desired[V9_MAX_CARDS];
    uint32_t pending_ms[V9_MAX_CARDS];
    int card_count;
    int dynamic;
    int room_filter;
    int visible;
    lv_obj_t *scene_btn;
    lv_obj_t *scene_icon;
    lv_obj_t *scene_label;
    lv_obj_t *scene_menu;
    lv_obj_t *scene_item[V9_SCENE_COUNT];
    lv_obj_t *scene_item_icon[V9_SCENE_COUNT];
    int scene;
    lv_obj_t *modal;
} v9_home_state_t;

static v9_home_state_t g_v9;


static void v9_scene_menu_close(void);

static void v9_close_modal(void)
{
    v9_scene_menu_close();
    smart_home_lvgl_device_dialog_close();
    smart_home_lvgl_room_dialog_close();
    smart_home_lvgl_camera_dialog_close();
    if (g_v9.modal) {
        lv_obj_delete(g_v9.modal);
        g_v9.modal = NULL;
    }
}

static void v9_mask_click_cb(lv_event_t *event)
{
    (void)event;
    v9_close_modal();
}

static void v9_stop_bubbling_cb(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
}

static lv_obj_t *v9_modal_panel(const char *title)
{
    lv_obj_t *mask;
    lv_obj_t *panel;
    lv_obj_t *label;

    v9_close_modal();
    mask = lv_obj_create(g_v9.screen);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, smart_home_lvgl_disp_w(), smart_home_lvgl_disp_h());
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_30, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mask, v9_mask_click_cb, LV_EVENT_CLICKED, NULL);
    g_v9.modal = mask;

    panel = lv_obj_create(mask);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 840, 480);
    lv_obj_center(panel);
    smart_home_lvgl_card_style(panel);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_pad_all(panel, 24, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, v9_stop_bubbling_cb, LV_EVENT_CLICKED, NULL);

    label = smart_home_lvgl_label_create(panel, title,
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 24);
    lv_obj_set_pos(label, 0, 0);
    return panel;
}

static void v9_modal_close_button(lv_obj_t *panel)
{
    lv_obj_t *button = lv_button_create(panel);
    lv_obj_t *label;

    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 120, 40);
    lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_PRIMARY);
    lv_obj_set_style_radius(button, 10, 0);
    smart_home_lvgl_press_feedback(button);
    lv_obj_add_event_cb(button, v9_mask_click_cb, LV_EVENT_CLICKED, NULL);

    label = smart_home_lvgl_label_create(button, "关闭",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 16);
    lv_obj_center(label);
}

static void v9_info_modal(const char *title, const char *body)
{
    lv_obj_t *panel = v9_modal_panel(title);
    lv_obj_t *label = smart_home_lvgl_label_create(
        panel, body, SMART_HOME_UI_COLOR_TEXT_SECONDARY, 16);

    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, 760);
    lv_obj_set_pos(label, 0, 64);
    v9_modal_close_button(panel);
}

static void v9_update_card(int index)
{
    const v9_device_def_t *def = &g_v9_devices[g_v9.def_index[index]];
    int on = g_v9.on[index];
    const char *state = v9_art_state(def, on, NULL);

    if (g_v9.status[index]) {
        lv_label_set_text(g_v9.status[index], "");
    }
    if (g_v9.metric[index]) {
        lv_label_set_text(g_v9.metric[index], on ? def->metric_on : def->metric_off);
    }
    if (g_v9.art[index]) {
        snprintf(g_v9.art_path[index], sizeof(g_v9.art_path[index]),
                 "A:%s/res/devices/%s-%s.png",
                 CONFIG_SMART_HOME_DEMO_DATA_ROOT, def->id, state);
        lv_image_set_src(g_v9.art[index], g_v9.art_path[index]);
    }
    if (g_v9.sw[index]) {
        if (on) {
            lv_obj_add_state(g_v9.sw[index], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(g_v9.sw[index], LV_STATE_CHECKED);
        }
    }
}

static void v9_reflow(void)
{
    int visible = 0;

    for (int i = 0; i < g_v9.card_count; i++) {
        int match = g_v9.room_filter == 0 ||
                    !strcmp(g_v9.room_text[i],
                            g_v9.rooms[g_v9.room_filter - 1]);

        if (!match) {
            lv_obj_add_flag(g_v9.card[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(g_v9.card[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(g_v9.card[i], 4 + visible * (V9_CARD_W + V9_CARD_GAP), 6);
        visible++;
    }
    g_v9.visible = visible;
    if (g_v9.empty) {
        if (visible == 0) {
            lv_obj_clear_flag(g_v9.empty, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_v9.empty, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

int smart_home_lvgl_v9_select_room(int index)
{
    if (index < 0 || index > g_v9.room_count) {
        return -1;
    }
    g_v9.room_filter = index;
    for (int i = 0; i <= g_v9.room_count; i++) {
        lv_obj_t *btn = g_v9.room_btn[i];
        lv_obj_t *label;
        if (!btn) continue;
        label = lv_obj_get_child(btn, 0);
        if (i == index) {
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
    v9_reflow();
    if (g_v9.view) {
        lv_obj_scroll_to_x(g_v9.view, 0, LV_ANIM_OFF);
    }
    return g_v9.visible;
}

static void v9_room_click_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(event));

    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    (void)smart_home_lvgl_v9_select_room(index);
}

static const char *display_room(const char *room)
{
    if (!room || !room[0]) return "未分配";
    if (!strcmp(room, "living_room")) return "客厅";
    if (!strcmp(room, "bedroom")) return "卧室";
    return room;
}

static int card_index_for_kind(smart_home_v9_kind_t kind)
{
    switch (kind) {
    case SMART_HOME_V9_KIND_LIGHT:  return 0;
    case SMART_HOME_V9_KIND_AC:     return 1;
    case SMART_HOME_V9_KIND_WASHER: return 2;
    case SMART_HOME_V9_KIND_ROBOT:  return 3;
    case SMART_HOME_V9_KIND_CAMERA: return 4;
    case SMART_HOME_V9_KIND_FAN:    return 5;
    default:                        return -1;
    }
}

static const char *view_status(const smart_home_v9_device_view_t *view)
{
    if (view->source == SMART_HOME_V9_SOURCE_MILOCO) {
        if (!view->online) return "离线";
        if (!(view->capabilities & SMART_HOME_V9_CAP_POWER)) return "";
    }
    if (!view->state_valid) return "状态待获取";
    if (view->source == SMART_HOME_V9_SOURCE_BOARD) {
        switch (view->activity) {
        case SMART_HOME_V9_CAMERA_STARTING: return "启动中";
        case SMART_HOME_V9_CAMERA_PREVIEWING: return "预览中";
        case SMART_HOME_V9_CAMERA_STOPPING: return "停止中";
        case SMART_HOME_V9_CAMERA_ERROR: return "操作失败";
        default: return "";
        }
    }
    switch (view->kind) {
    case SMART_HOME_V9_KIND_WASHER:
        return view->on ? "洗涤中" : "";
    case SMART_HOME_V9_KIND_ROBOT:
        return view->on ? "清扫中" : "";
    case SMART_HOME_V9_KIND_CAMERA:
        return view->source == SMART_HOME_V9_SOURCE_MILOCO ? "" :
            (view->on ? "预览中" : "");
    default:
        return "";
    }
}

static void v9_apply_view(int index,
                          const smart_home_v9_device_view_t *view)
{
    const v9_device_def_t *def;
    char text[64];
    const char *state;

    if (!view || index < 0 || index >= V9_MAX_CARDS ||
        g_v9.def_index[index] < 0 ||
        g_v9.def_index[index] >= V9_DEVICE_COUNT) {
        return;
    }
    def = &g_v9_devices[g_v9.def_index[index]];
    state = v9_art_state(def, view->on, view);
    g_v9.device_view[index] = *view;
    g_v9.real[index] = 1;
    g_v9.source[index] = view->source;
    g_v9.on[index] = view->on ? 1 : 0;
    snprintf(g_v9.key[index], sizeof(g_v9.key[index]), "%s", view->key);
    snprintf(g_v9.name_text[index], sizeof(g_v9.name_text[index]), "%s",
             view->name[0] ? view->name : def->name);
    snprintf(g_v9.room_text[index], sizeof(g_v9.room_text[index]), "%s",
             display_room(view->room_label));

    if (g_v9.room_label[index]) {
        lv_label_set_text(g_v9.room_label[index], g_v9.room_text[index]);
    }
    if (g_v9.name_label[index]) {
        lv_label_set_text(g_v9.name_label[index], g_v9.name_text[index]);
    }
    if (g_v9.status[index]) {
        lv_label_set_text(g_v9.status[index], view_status(view));
    }
    if (g_v9.metric[index]) {
        if (view->source == SMART_HOME_V9_SOURCE_BOARD) {
            if (view->activity == SMART_HOME_V9_CAMERA_PREVIEWING)
                snprintf(text, sizeof text, "%d FPS", view->primary_value);
            else snprintf(text, sizeof text, "本机");
        } else if (view->kind == SMART_HOME_V9_KIND_CAMERA) {
            snprintf(text, sizeof(text), "%s",
                     view->online ? "在线" : "离线");
        } else if (view->primary_value > 0) {
            snprintf(text, sizeof(text), "%d%s", view->primary_value,
                     view->unit ? view->unit : "");
        } else {
            snprintf(text, sizeof(text), "--");
        }
        lv_label_set_text(g_v9.metric[index], text);
    }
    if (g_v9.art[index]) {
        snprintf(g_v9.art_path[index], sizeof(g_v9.art_path[index]),
                 "A:%s/res/devices/%s-%s.png",
                 CONFIG_SMART_HOME_DEMO_DATA_ROOT, def->id, state);
        lv_image_set_src(g_v9.art[index], g_v9.art_path[index]);
    }
    if (g_v9.sw[index]) {
        if (view->on) {
            lv_obj_add_state(g_v9.sw[index], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(g_v9.sw[index], LV_STATE_CHECKED);
        }
    }
}

static void v9_switch_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    lv_obj_t *sw = lv_event_get_current_target(event);
    int index = (int)(intptr_t)lv_obj_get_user_data(sw);
    int desired;

    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || index < 0 ||
        index >= g_v9.card_count) {
        return;
    }
    desired = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;

    if (g_v9.real[index]) {
#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
        if (g_v9.source[index] == SMART_HOME_V9_SOURCE_BOARD) {
            if (desired) {
                v9_close_modal();
                int opened = smart_home_lvgl_camera_dialog_open(ui, &g_v9.device_view[index]);
                if (opened || smart_home_camera_request(true))
                    v9_info_modal("操作未完成", "无法提交摄像头操作，请重试。");
            } else (void)smart_home_camera_request(false);
            smart_home_lvgl_v9_refresh_views(ui);
            return;
        }
#endif
        if (g_v9.source[index] == SMART_HOME_V9_SOURCE_LOCAL &&
            ui && ui->app && ui->device_state) {
            smart_home_device_t *device;
            int id = atoi(g_v9.key[index] + 6); /* "local:" */
            int value;
            int ret;

            device = smart_home_device_find_by_id(ui->device_state, id);
            if (!device) {
                if (g_v9.on[index]) lv_obj_add_state(sw, LV_STATE_CHECKED);
                else lv_obj_remove_state(sw, LV_STATE_CHECKED);
                return;
            }
            value = device->type == SMART_HOME_DEVICE_LIGHT ?
                    device->brightness : device->temperature;
            ret = smart_home_device_service_set_device_control(
                &ui->app->device_service, id, desired, value,
                device->ac_mode, device->ac_fan_speed);
            if (ret == AGENT_OK) {
                g_v9.device_view[index].on = desired != 0;
                v9_apply_view(index, &g_v9.device_view[index]);
            } else {
                if (g_v9.on[index]) lv_obj_add_state(sw, LV_STATE_CHECKED);
                else lv_obj_remove_state(sw, LV_STATE_CHECKED);
                v9_info_modal("操作未完成", "设备服务未确认本次操作。");
            }
            return;
        }
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
        if (g_v9.source[index] == SMART_HOME_V9_SOURCE_MILOCO &&
            ui && ui->app && ui->app->miloco) {
            const char *did = g_v9.key[index] + 5; /* "milo:" */
            int ret = smart_home_miloco_submit_power(ui->app->miloco, did,
                                                     desired);
            if (ret == AGENT_OK) {
                g_v9.pending[index] = 1;
                g_v9.pending_desired[index] = desired;
                g_v9.pending_ms[index] = lv_tick_get();
                if (g_v9.status[index]) {
                    lv_label_set_text(g_v9.status[index], "等待确认");
                }
            } else {
                if (g_v9.on[index]) lv_obj_add_state(sw, LV_STATE_CHECKED);
                else lv_obj_remove_state(sw, LV_STATE_CHECKED);
                v9_info_modal("提交失败", "米家网关未接受本次操作。");
            }
            return;
        }
#endif
    }

    g_v9.on[index] = desired;
    v9_update_card(index);
}

static int kind_def_index(smart_home_v9_kind_t kind);
static void v9_card_click_cb(lv_event_t *event)
{
    lv_obj_t *card = lv_event_get_current_target(event);
    int index = (int)(intptr_t)lv_obj_get_user_data(card);
    if (lv_event_get_code(event) != LV_EVENT_CLICKED ||
        lv_event_get_target(event) != card || index < 0 || index >= g_v9.card_count) return;
    smart_home_v9_device_view_t view;
    if (g_v9.real[index]) view = g_v9.device_view[index];
    else {
        smart_home_v9_device_view_t demos[V9_DEVICE_COUNT];
        smart_home_v9_demo_views(demos, V9_DEVICE_COUNT);
        view = demos[g_v9.def_index[index]];
        view.on = g_v9.on[index] != 0;
    }
    v9_close_modal();
#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
    if (view.source == SMART_HOME_V9_SOURCE_BOARD) {
        smart_home_lvgl_camera_dialog_open(lv_event_get_user_data(event), &view);
        return;
    }
#endif
    smart_home_lvgl_device_dialog_open(lv_event_get_user_data(event), &view);
}

static void v9_add_room_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        v9_close_modal();
        smart_home_lvgl_room_dialog_open(lv_event_get_user_data(event));
    }
}

static void v9_update_time(smart_home_lvgl_t *ui)
{
    smart_home_lvgl_update_clock(ui);
}

static void v9_time_timer_cb(lv_timer_t *timer)
{
    v9_update_time((smart_home_lvgl_t *)lv_timer_get_user_data(timer));
}

static lv_obj_t *v9_text(lv_obj_t *parent, const char *text, int x, int y,
                         int size, lv_color_t color)
{
    lv_obj_t *label = smart_home_lvgl_label_create(parent, text, color, size);
    lv_obj_set_pos(label, x, y);
    return label;
}


/* ── 场景切换器 ──────────────────────────────────────────── */

static void v9_scene_menu_close(void);

static void v9_scene_apply_label(void)
{
    const v9_scene_def_t *def;

    if (g_v9.scene < 0 || g_v9.scene >= V9_SCENE_COUNT) {
        return;
    }
    def = &g_v9_scenes[g_v9.scene];
    if (g_v9.scene_label) {
        lv_label_set_text(g_v9.scene_label, def->title);
    }
    if (g_v9.scene_icon) {
        const lv_image_dsc_t *asset =
            smart_home_lvgl_png_icon_get(def->icon, V9_SCENE_ICON_PX);

        if (asset) {
            lv_image_set_src(g_v9.scene_icon, asset);
            /* 必须先有 src 再判定格式：icon_color 在 src 为空时会按 A8 处理，
             * 把 ARGB8888 像素图整块涂成单色。 */
            smart_home_lvgl_icon_color(g_v9.scene_icon,
                                       SMART_HOME_UI_COLOR_TEXT_PRIMARY);
        }
    }
}

/* 菜单里高亮当前项：LVGL 主题对 LV_PART_SELECTED|LV_STATE_CHECKED 的
 * 优先级高于 LV_PART_SELECTED，两个选择器都要设，否则仍显示主题蓝。 */
static void v9_scene_item_selected(int index, int selected)
{
    lv_obj_t *item = g_v9.scene_item[index];

    if (!item) {
        return;
    }
    lv_obj_set_style_bg_color(item,
        selected ? SMART_HOME_UI_COLOR_SURFACE_ON : SMART_HOME_UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    smart_home_lvgl_icon_color(g_v9.scene_item_icon[index], selected ?
        SMART_HOME_UI_COLOR_TEXT_PRIMARY : SMART_HOME_UI_COLOR_TEXT_SECONDARY);
}

static void v9_scene_menu_refresh(void)
{
    int i;

    for (i = 0; i < V9_SCENE_COUNT; i++) {
        v9_scene_item_selected(i, i == g_v9.scene);
    }
}

/* 菜单只做显隐，不销毁：菜单项的点击回调运行在自己的对象上，若在回调里
 * 删除父容器，回调返回后 LVGL 仍会访问已释放的子对象。 */
static void v9_scene_menu_close(void)
{
    if (g_v9.scene_menu) {
        lv_obj_add_flag(g_v9.scene_menu, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 场景执行：直接走 miloco 语义锚点匹配（与 run_scene 工具同一条路径），
 * 不经过 agent，因此点一下即刻提交，不需要用户再按发送、也不再等 LLM。 */
static void v9_scene_run(smart_home_lvgl_t *ui, int index)
{
    const v9_scene_def_t *def;

    if (!ui || index < 0 || index >= V9_SCENE_COUNT) {
        return;
    }
    def = &g_v9_scenes[index];
    g_v9.scene = index;
    v9_scene_apply_label();

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    {
        smart_home_miloco_t *gateway = ui->app ? ui->app->miloco : NULL;
        char report[256];
        int ret;

        if (!gateway || !smart_home_miloco_reachable(gateway)) {
            v9_info_modal("场景未执行",
                          "米家网关未连接，请先在设置中完成米家绑定。");
            return;
        }
        report[0] = '\0';
        ret = smart_home_miloco_run_scene(gateway, def->id, report,
                                          sizeof(report));
        if (ret == 0) {
            v9_info_modal("场景已提交",
                          report[0] ? report : "已对在线设备提交场景动作。");
        } else {
            v9_info_modal("场景未执行",
                          report[0] ? report : "网关拒绝了本次场景请求。");
        }
    }
#else
    v9_info_modal("场景未执行", "当前固件未启用米家网关。");
#endif
}

/* ── 下拉菜单 ────────────────────────────────────────────── */

static void v9_scene_item_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    lv_obj_t *item = lv_event_get_current_target(event);
    int index = (int)(intptr_t)lv_obj_get_user_data(item);

    v9_scene_menu_close();
    v9_scene_run(ui, index);
}

static void v9_scene_menu_open(smart_home_lvgl_t *ui)
{
    lv_obj_t *menu;
    int i;
    int y = V9_SCENE_BTN_Y + V9_SCENE_BTN_H + 6;

    if (!ui || !g_v9.screen) {
        return;
    }
    if (g_v9.scene_menu) {
        lv_obj_clear_flag(g_v9.scene_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_v9.scene_menu);
        v9_scene_menu_refresh();
        return;
    }
    /* 挂到顶层图层而不是首页：首页的最后一个子对象必须是底部导航栏
     * （测试与相机弹窗都依赖这个约定）。顶层图层坐标与屏幕一致。 */
    menu = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(menu);
    lv_obj_set_size(menu, V9_SCENE_BTN_W,
                    V9_SCENE_COUNT * V9_SCENE_ITEM_H + 2 * V9_SCENE_MENU_PAD);
    lv_obj_set_pos(menu, V9_SCENE_BTN_X, y);
    lv_obj_set_style_radius(menu, 12, 0);
    lv_obj_set_style_bg_color(menu, SMART_HOME_UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(menu, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(menu, 1, 0);
    lv_obj_set_style_border_color(menu, SMART_HOME_UI_COLOR_BORDER, 0);
    lv_obj_set_style_pad_all(menu, V9_SCENE_MENU_PAD, 0);
    lv_obj_set_style_shadow_width(menu, 18, 0);
    lv_obj_set_style_shadow_opa(menu, LV_OPA_20, 0);
    lv_obj_set_style_shadow_ofs_y(menu, 4, 0);
    lv_obj_clear_flag(menu, LV_OBJ_FLAG_SCROLLABLE);
    g_v9.scene_menu = menu;

    for (i = 0; i < V9_SCENE_COUNT; i++) {
        lv_obj_t *item = lv_obj_create(menu);
        lv_obj_t *icon;
        lv_obj_t *label;

        lv_obj_remove_style_all(item);
        lv_obj_set_size(item, V9_SCENE_BTN_W - 2 * V9_SCENE_MENU_PAD,
                        V9_SCENE_ITEM_H);
        lv_obj_set_pos(item, 0, i * V9_SCENE_ITEM_H);
        lv_obj_set_style_radius(item, 8, 0);
        lv_obj_set_style_pad_all(item, 0, 0);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(item, (void *)(intptr_t)i);
        lv_obj_add_event_cb(item, v9_scene_item_cb, LV_EVENT_CLICKED, ui);
        smart_home_lvgl_press_feedback(item);

        icon = lv_image_create(item);
        lv_image_set_src(icon,
            smart_home_lvgl_png_icon_get(g_v9_scenes[i].icon, V9_SCENE_ICON_PX));
        lv_obj_set_pos(icon, 10, (V9_SCENE_ITEM_H - V9_SCENE_ICON_PX) / 2);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        if (lv_image_get_src(icon)) {
            smart_home_lvgl_icon_color(icon, SMART_HOME_UI_COLOR_TEXT_SECONDARY);
        }
        g_v9.scene_item_icon[i] = icon;

        label = smart_home_lvgl_label_create(item, g_v9_scenes[i].title,
            SMART_HOME_UI_COLOR_TEXT_SECONDARY, 16);
        lv_obj_set_pos(label, 10 + V9_SCENE_ICON_PX + 8,
                       (V9_SCENE_ITEM_H - 20) / 2);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
        g_v9.scene_item[i] = item;
    }
    v9_scene_menu_refresh();
}

static void v9_scene_btn_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);

    if (g_v9.scene_menu && !lv_obj_has_flag(g_v9.scene_menu, LV_OBJ_FLAG_HIDDEN)) {
        v9_scene_menu_close();
    } else {
        v9_scene_menu_open(ui);
    }
}

static lv_obj_t *v9_make_scene_button(lv_obj_t *screen, smart_home_lvgl_t *ui)
{
    lv_obj_t *button = lv_button_create(screen);
    lv_obj_t *icon;
    lv_obj_t *label;

    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, V9_SCENE_BTN_W, V9_SCENE_BTN_H);
    lv_obj_set_pos(button, V9_SCENE_BTN_X, V9_SCENE_BTN_Y);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, SMART_HOME_UI_COLOR_BORDER, 0);
    lv_obj_set_ext_click_area(button, 4);
    lv_obj_add_event_cb(button, v9_scene_btn_cb, LV_EVENT_CLICKED, ui);
    smart_home_lvgl_press_feedback(button);
    g_v9.scene_btn = button;

    icon = lv_image_create(button);
    lv_obj_set_pos(icon, 13, (V9_SCENE_BTN_H - V9_SCENE_ICON_PX) / 2);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    g_v9.scene_icon = icon;
    /* src 在 v9_scene_apply_label() 里赋值，避免此处 icon_color 误判格式。 */

    label = smart_home_lvgl_label_create(button, "", 
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 17);
    lv_obj_set_pos(label, 13 + V9_SCENE_ICON_PX + 7,
                   (V9_SCENE_BTN_H - 22) / 2);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    g_v9.scene_label = label;

    /* 右侧指示箭头：用 lv_line 画折线，避免额外生成一个 20px 图标资产。 */
    {
        static lv_point_precise_t arrow_pts[3];
        lv_obj_t *arrow = lv_line_create(button);

        arrow_pts[0].x = 0;
        arrow_pts[0].y = 0;
        arrow_pts[1].x = 5;
        arrow_pts[1].y = 5;
        arrow_pts[2].x = 10;
        arrow_pts[2].y = 0;
        lv_line_set_points(arrow, arrow_pts, 3);
        lv_obj_set_pos(arrow, V9_SCENE_BTN_W - 22, (V9_SCENE_BTN_H - 6) / 2);
        lv_obj_set_style_line_color(arrow, SMART_HOME_UI_COLOR_TEXT_SECONDARY, 0);
        lv_obj_set_style_line_width(arrow, 2, 0);
        lv_obj_set_style_line_rounded(arrow, true, 0);
        lv_obj_clear_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
    }
    return button;
}

static lv_obj_t *v9_make_room_button(lv_obj_t *screen, int index,
                                     const char *text)
{
    lv_obj_t *button = lv_button_create(screen);
    lv_obj_t *label;

    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 78, 40);
    lv_obj_set_pos(button, index * 84, 4);
    lv_obj_set_style_radius(button, 20, 0);
    lv_obj_set_ext_click_area(button, 4);
    if (index == 0) {
        lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    }
    smart_home_lvgl_press_feedback(button);
    lv_obj_set_user_data(button, (void *)(intptr_t)index);
    lv_obj_add_event_cb(button, v9_room_click_cb, LV_EVENT_CLICKED, NULL);

    label = smart_home_lvgl_label_create(
        button, text,
        index == 0 ? SMART_HOME_UI_COLOR_TEXT_PRIMARY :
                     SMART_HOME_UI_COLOR_TEXT_SECONDARY, 16);
    lv_obj_set_width(label, 70);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return button;
}

static void v9_refresh_rooms(smart_home_lvgl_t *ui)
{
    char rooms[V9_MAX_ROOMS][SMART_HOME_ROOM_NAME_SIZE] = {{0}};
    smart_home_state_t state;
    int count = 0;
    if (ui->app && smart_home_device_service_copy_state(&ui->app->device_service, &state) == AGENT_OK) {
        for (int i = 0; i < state.room_count; i++)
            snprintf(rooms[count++], SMART_HOME_ROOM_NAME_SIZE, "%s",
                     display_room(state.rooms[i]));
    } else {
        for (int i = 0; i < V9_ROOM_COUNT; i++)
            snprintf(rooms[count++], SMART_HOME_ROOM_NAME_SIZE, "%s", g_v9_rooms[i]);
    }
    /* Gateway rooms remain visible even when not part of the local catalog. */
    for (int i = 0; i < g_v9.card_count && count < V9_MAX_ROOMS; i++) {
        int found = 0;
        for (int n = 0; n < count; n++) if (!strcmp(rooms[n], g_v9.room_text[i])) found = 1;
        if (!found && g_v9.room_text[i][0])
            snprintf(rooms[count++], SMART_HOME_ROOM_NAME_SIZE, "%s", g_v9.room_text[i]);
    }
    if (g_v9.room_btn[0] && count == g_v9.room_count &&
        !memcmp(rooms, g_v9.rooms, sizeof rooms)) return;
    char selected[SMART_HOME_ROOM_NAME_SIZE] = "";
    if (g_v9.room_filter > 0)
        snprintf(selected, sizeof selected, "%s", g_v9.rooms[g_v9.room_filter - 1]);
    memcpy(g_v9.rooms, rooms, sizeof rooms);
    g_v9.room_count = count;
    g_v9.room_filter = 0;
    memset(g_v9.room_btn, 0, sizeof g_v9.room_btn);
    lv_obj_clean(g_v9.room_strip);
    g_v9.room_btn[0] = v9_make_room_button(g_v9.room_strip, 0, "全部");
    for (int i = 0; i < count; i++) {
        g_v9.room_btn[i + 1] = v9_make_room_button(g_v9.room_strip, i + 1, rooms[i]);
        if (!strcmp(rooms[i], selected)) g_v9.room_filter = i + 1;
    }
    /* 与房间一起参与横向滚动，始终紧跟最后一个房间。 */
    lv_obj_t *add = lv_button_create(g_v9.room_strip);
    lv_obj_remove_style_all(add);
    lv_obj_set_size(add, 48, 48);
    lv_obj_set_pos(add, (count + 1) * 84, 0);
    lv_obj_set_style_radius(add, 16, 0);
    smart_home_lvgl_set_bg(add, SMART_HOME_UI_COLOR_SURFACE_ON);
    lv_obj_set_ext_click_area(add, 4);
    lv_obj_add_event_cb(add, v9_add_room_cb, LV_EVENT_CLICKED, ui);
    smart_home_lvgl_press_feedback(add);
    lv_obj_t *label = smart_home_lvgl_label_create(add, "+",
        SMART_HOME_UI_COLOR_TEXT_PRIMARY, 24);
    lv_obj_center(label);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    smart_home_lvgl_v9_select_room(g_v9.room_filter);
}

static void v9_make_card(smart_home_lvgl_t *ui, int index)
{
    const v9_device_def_t *def = &g_v9_devices[index];
    lv_obj_t *card = lv_obj_create(g_v9.view);
    lv_obj_t *sw;

    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, V9_CARD_W, V9_CARD_H);
    lv_obj_set_pos(card, 4 + index * (V9_CARD_W + V9_CARD_GAP), 6);
    smart_home_lvgl_card_style(card);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card, (void *)(intptr_t)index);
    lv_obj_add_event_cb(card, v9_card_click_cb, LV_EVENT_CLICKED, ui);
    g_v9.card[index] = card;
    g_v9.def_index[index] = index;
    g_v9.source[index] = SMART_HOME_V9_SOURCE_DEMO;

    g_v9.room_label[index] = v9_text(card, def->room, 14, 16, 15,
                                     SMART_HOME_UI_COLOR_TEXT_SECONDARY);
    g_v9.name_label[index] = v9_text(card, def->name, 14, 49, 18,
                                     SMART_HOME_UI_COLOR_TEXT_PRIMARY);
    snprintf(g_v9.room_text[index], sizeof(g_v9.room_text[index]), "%s",
             def->room);
    snprintf(g_v9.name_text[index], sizeof(g_v9.name_text[index]), "%s",
             def->name);
    g_v9.status[index] = v9_text(card, "", 14, 79, 12,
                                 SMART_HOME_UI_COLOR_TEXT_SECONDARY);

    g_v9.art[index] = lv_image_create(card);
    lv_obj_set_pos(g_v9.art[index], def->art_x, def->art_y);
    lv_obj_set_size(g_v9.art[index], def->art_w, def->art_h);
    lv_obj_clear_flag(g_v9.art[index], LV_OBJ_FLAG_CLICKABLE);

    g_v9.metric[index] = v9_text(card, def->metric_on, 14, 204, 20,
                                 SMART_HOME_UI_COLOR_TEXT_PRIMARY);
    v9_text(card, def->caption, 14, 228, 11,
            SMART_HOME_UI_COLOR_TEXT_SECONDARY);

    sw = lv_switch_create(card);
    lv_obj_set_size(sw, 54, 30);
    lv_obj_set_pos(sw, 111, 13);
    lv_obj_set_ext_click_area(sw, 9);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xCCD6D6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, 15, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, SMART_HOME_UI_COLOR_PRIMARY,
                              LV_PART_MAIN | LV_STATE_CHECKED);
    /* LVGL 9.1 draws the track in INDICATOR and the circle in KNOB. */
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(sw, 0, LV_PART_KNOB);
    lv_obj_set_style_radius(sw, 11, LV_PART_KNOB);
    /* LVGL knob padding expands outward; -4 yields the design's 22 px circle. */
    lv_obj_set_style_pad_all(sw, -4, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
    lv_obj_set_user_data(sw, (void *)(intptr_t)index);
    lv_obj_add_event_cb(sw, v9_switch_cb, LV_EVENT_VALUE_CHANGED, ui);
    g_v9.sw[index] = sw;

    g_v9.on[index] = def->on;
    v9_update_card(index);
}

static int kind_def_index(smart_home_v9_kind_t kind)
{
    switch (kind) {
    case SMART_HOME_V9_KIND_LIGHT:  return 0;
    case SMART_HOME_V9_KIND_AC:     return 1;
    case SMART_HOME_V9_KIND_WASHER: return 2;
    case SMART_HOME_V9_KIND_ROBOT:  return 3;
    case SMART_HOME_V9_KIND_CAMERA: return 4;
    case SMART_HOME_V9_KIND_FAN:    return 5;
    default:                        return -1;
    }
}

static void v9_destroy_cards(void)
{
    for (int i = 0; i < g_v9.card_count; i++) {
        if (g_v9.card[i]) {
            lv_obj_delete(g_v9.card[i]);
        }
        g_v9.card[i] = NULL;
        g_v9.room_label[i] = NULL;
        g_v9.name_label[i] = NULL;
        g_v9.status[i] = NULL;
        g_v9.metric[i] = NULL;
        g_v9.sw[i] = NULL;
        g_v9.art[i] = NULL;
        g_v9.real[i] = 0;
        g_v9.pending[i] = 0;
    }
    g_v9.card_count = 0;
    g_v9.dynamic = 0;
}

static void v9_create_demo_cards(smart_home_lvgl_t *ui)
{
    v9_destroy_cards();
    /* 其余造型定义只用于真实设备映射，不再生成模拟设备卡片。 */
    v9_make_card(ui, 0);
    g_v9.card_count = 1;
}

static void v9_make_dynamic_card(smart_home_lvgl_t *ui, int index,
                                 const smart_home_v9_device_view_t *view)
{
    int def_index = kind_def_index(view->kind);
    const v9_device_def_t *def;
    lv_obj_t *card;
    lv_obj_t *sw;

    if (def_index < 0) {
        return;
    }
    def = &g_v9_devices[def_index];
    card = lv_obj_create(g_v9.view);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, V9_CARD_W, V9_CARD_H);
    lv_obj_set_pos(card, 4 + index * (V9_CARD_W + V9_CARD_GAP), 6);
    smart_home_lvgl_card_style(card);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card, (void *)(intptr_t)index);
    lv_obj_add_event_cb(card, v9_card_click_cb, LV_EVENT_CLICKED, ui);
    g_v9.card[index] = card;
    g_v9.def_index[index] = def_index;
    g_v9.source[index] = view->source;
    g_v9.real[index] = 0;
    g_v9.pending[index] = 0;

    g_v9.room_label[index] = v9_text(card, "", 14, 16, 15,
                                     SMART_HOME_UI_COLOR_TEXT_SECONDARY);
    g_v9.name_label[index] = v9_text(card, "", 14, 49, 18,
                                     SMART_HOME_UI_COLOR_TEXT_PRIMARY);
    g_v9.status[index] = v9_text(card, "", 14, 79, 12,
                                 SMART_HOME_UI_COLOR_TEXT_SECONDARY);
    g_v9.metric[index] = v9_text(card, "", 14, 204, 20,
                                 SMART_HOME_UI_COLOR_TEXT_PRIMARY);
    v9_text(card, def->caption, 14, 228, 11,
            SMART_HOME_UI_COLOR_TEXT_SECONDARY);

    g_v9.art[index] = lv_image_create(card);
    lv_obj_set_pos(g_v9.art[index], def->art_x, def->art_y);
    lv_obj_set_size(g_v9.art[index], def->art_w, def->art_h);
    lv_obj_clear_flag(g_v9.art[index], LV_OBJ_FLAG_CLICKABLE);

    sw = lv_switch_create(card);
    lv_obj_set_size(sw, 54, 30);
    lv_obj_set_pos(sw, 111, 13);
    lv_obj_set_ext_click_area(sw, 9);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xCCD6D6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, 15, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, SMART_HOME_UI_COLOR_PRIMARY,
                              LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(sw, 0, LV_PART_KNOB);
    lv_obj_set_style_radius(sw, 11, LV_PART_KNOB);
    /* LVGL knob padding expands outward; -4 yields the design's 22 px circle. */
    lv_obj_set_style_pad_all(sw, -4, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
    lv_obj_set_user_data(sw, (void *)(intptr_t)index);
    lv_obj_add_event_cb(sw, v9_switch_cb, LV_EVENT_VALUE_CHANGED, ui);
    g_v9.sw[index] = sw;

    v9_apply_view(index, view);
}

/* 每秒轮询一次设备状态；只有设备服务或米家快照的 revision 变化时才重建
 * 视图。空闲时整条 collect/reflow 路径被跳过，避免每秒一次的无效重排。
 */

static uint32_t g_v9_last_dev_rev;
static uint32_t g_v9_last_miloco_rev;
static int g_v9_refresh_forced = 1;

void smart_home_lvgl_v9_refresh_views(smart_home_lvgl_t *ui)
{
    smart_home_v9_device_view_t raw[V9_MAX_CARDS];
    smart_home_v9_device_view_t views[V9_MAX_CARDS];
    size_t count = 0;
    size_t valid = 0;
    smart_home_state_t local;
    int have_local;

    if (!ui || !g_v9.screen) {
        return;
    }

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    const smart_home_miloco_t *gateway = ui->app && ui->app->miloco &&
        smart_home_miloco_reachable(ui->app->miloco) && smart_home_miloco_bound(ui->app->miloco) ? ui->app->miloco : NULL;
#endif

    /* 未变化时只保留两件必须每秒做的事：摄像头指示灯与"等待确认"超时。 */

    {
        uint32_t dev_rev = ui->app ?
            smart_home_device_service_revision(&ui->app->device_service) : 0;
        uint32_t miloco_rev = 0;

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
        if (gateway) {
            (void)smart_home_miloco_list(gateway, NULL, 0, &miloco_rev);
        }
#endif
        if (!g_v9_refresh_forced && g_v9.card_count > 0 &&
            dev_rev == g_v9_last_dev_rev && miloco_rev == g_v9_last_miloco_rev) {
#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
            struct smart_home_camera_status_s camera_status;
            if (!smart_home_camera_get_status(&camera_status))
                smart_home_lvgl_set_camera_indicator(ui, camera_status.state == SMART_HOME_CAMERA_RUNNING && camera_status.preview_sequence > 0);
#endif
            for (int i = 0; i < g_v9.card_count; i++) {
                if (g_v9.pending[i] &&
                    (uint32_t)(lv_tick_get() - g_v9.pending_ms[i]) >= 10000u) {
                    g_v9.pending[i] = 0;
                    if (g_v9.status[i]) {
                        lv_label_set_text(g_v9.status[i], "未确认");
                    }
                }
            }
            return;
        }
        g_v9_last_dev_rev = dev_rev;
        g_v9_last_miloco_rev = miloco_rev;
        g_v9_refresh_forced = 0;
    }

    have_local = ui->app && smart_home_device_service_copy_state(
        &ui->app->device_service, &local) == AGENT_OK;
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    count = smart_home_v9_collect_views(have_local ? &local : NULL, gateway, raw, V9_MAX_CARDS);
#else
    count = smart_home_v9_collect_views(have_local ? &local : NULL, raw, V9_MAX_CARDS);
#endif
#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
    struct smart_home_camera_status_s camera_status;
    if (!smart_home_camera_get_status(&camera_status))
        smart_home_lvgl_set_camera_indicator(ui, camera_status.state == SMART_HOME_CAMERA_RUNNING && camera_status.preview_sequence > 0);
#endif
    for (size_t i = 0; i < count; i++) {
        if (kind_def_index(raw[i].kind) < 0) {
            continue;
        }
        if (raw[i].source == SMART_HOME_V9_SOURCE_MILOCO &&
            raw[i].name[0] == '\0') {
            continue;
        }
        if (have_local && raw[i].source == SMART_HOME_V9_SOURCE_MILOCO) {
            const char *room = smart_home_device_room_override(&local, raw[i].key);
            if (room) {
                snprintf(raw[i].room_id, sizeof raw[i].room_id, "%s", room);
                snprintf(raw[i].room_label, sizeof raw[i].room_label, "%s", display_room(room));
            }
        }
        views[valid++] = raw[i];
    }
    if (valid == 0) {
        if (have_local) {
            if (g_v9.card_count) v9_destroy_cards();
            g_v9.dynamic = 1;
            v9_refresh_rooms(ui);
            v9_reflow();
            return;
        }
        if (g_v9.dynamic || g_v9.card_count == 0) {
            v9_create_demo_cards(ui);
            v9_refresh_rooms(ui);
            v9_reflow();
        }
        return;
    }

    if (g_v9.card_count == (int)valid) {
        int same = 1;

        for (size_t i = 0; i < valid; i++) {
            if (!g_v9.real[i] ||
                strcmp(g_v9.key[i], views[i].key) != 0) {
                same = 0;
                break;
            }
        }
        if (same) {
            for (size_t i = 0; i < valid; i++) {
                int index = (int)i;

                v9_apply_view(index, &views[i]);
                if (g_v9.pending[index]) {
                    if (views[i].state_valid &&
                        views[i].on == g_v9.pending_desired[index]) {
                        g_v9.pending[index] = 0;
                    } else if ((uint32_t)(lv_tick_get() -
                                          g_v9.pending_ms[index]) >= 10000u) {
                        g_v9.pending[index] = 0;
                        if (g_v9.status[index]) {
                            lv_label_set_text(g_v9.status[index], "未确认");
                        }
                    } else if (g_v9.status[index]) {
                        lv_label_set_text(g_v9.status[index], "等待确认");
                    }
                }
            }
            v9_refresh_rooms(ui);
            v9_reflow();
            return;
        }
    }

    v9_destroy_cards();
    for (size_t i = 0; i < valid; i++) {
        v9_make_dynamic_card(ui, (int)i, &views[i]);
    }
    g_v9.card_count = (int)valid;
    g_v9.dynamic = 1;
    v9_refresh_rooms(ui);
    v9_reflow();
}

static void v9_poll_timer_cb(lv_timer_t *timer)
{
    smart_home_lvgl_v9_refresh_views(
        (smart_home_lvgl_t *)lv_timer_get_user_data(timer));
}

void smart_home_lvgl_build_home_screen_v9(smart_home_lvgl_t *ui)
{
    lv_obj_t *screen;
    lv_obj_t *badge;
    lv_obj_t *icon;

    if (!ui) {
        return;
    }
    memset(&g_v9, 0, sizeof(g_v9));
    g_v9_refresh_forced = 1;

    ui->home_weather_badge = NULL;
    ui->home_weather_temp_label = NULL;
    ui->home_weather_condition_label = NULL;
    ui->home_date_label = NULL;
    ui->home_time_label = NULL;

    screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    smart_home_lvgl_set_bg(screen, SMART_HOME_UI_COLOR_BG);
    ui->screen_home = screen;
    g_v9.screen = screen;

    smart_home_lvgl_build_top_bar(screen, ui, "OpenVela HOME");

    ui->home_date_label = v9_text(screen, "", 28, 84, 14,
                                  SMART_HOME_UI_COLOR_TEXT_SECONDARY);
    ui->home_time_label = v9_text(screen, SMART_HOME_UI_DEFAULT_CLOCK, 24, 106, 50,
                                  SMART_HOME_UI_COLOR_TEXT_PRIMARY);
    ui->home_weather_condition_label = v9_text(
        screen, SMART_HOME_UI_DEFAULT_WEATHER, 244, 84, 14, SMART_HOME_UI_COLOR_TEXT_SECONDARY);
    ui->home_weather_temp_label = v9_text(
        screen, "--°", 240, 106, 50, SMART_HOME_UI_COLOR_TEXT_PRIMARY);

    /* 天气图标 128px 紧随温度块右侧，作为页头的天气读数配图。 */
    badge = lv_obj_create(screen);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, V9_WEATHER_ICON_PX, V9_WEATHER_ICON_PX);
    lv_obj_set_pos(badge, V9_WEATHER_ICON_X, V9_WEATHER_ICON_Y);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    ui->home_weather_badge = badge;
    icon = lv_image_create(badge);
    lv_image_set_src(icon, smart_home_lvgl_png_icon_get("asset:sun",
                                                     V9_WEATHER_ICON_PX));
    lv_obj_set_size(icon, V9_WEATHER_ICON_PX, V9_WEATHER_ICON_PX);
    /* 默认 inner_align 为 AUTO_TRANSFORM，缩放时会按原图尺寸居中并裁切；
     * 天气图标需要铺满徽标，因此显式设为 STRETCH。
     */
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_center(icon);

    g_v9.room_strip = lv_obj_create(screen);
    lv_obj_remove_style_all(g_v9.room_strip);
    lv_obj_set_pos(g_v9.room_strip, 28, 200);
    lv_obj_set_size(g_v9.room_strip, 760, 48);
    lv_obj_set_scroll_dir(g_v9.room_strip, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(g_v9.room_strip, LV_SCROLLBAR_MODE_OFF);
    v9_make_scene_button(screen, ui);

    g_v9.view = lv_obj_create(screen);
    lv_obj_remove_style_all(g_v9.view);
    lv_obj_set_size(g_v9.view, V9_VIEW_W, V9_VIEW_H);
    lv_obj_set_pos(g_v9.view, V9_VIEW_X, V9_VIEW_Y);
    lv_obj_set_style_bg_opa(g_v9.view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_v9.view, 0, 0);
    lv_obj_set_style_pad_all(g_v9.view, 0, 0);
    lv_obj_set_scroll_dir(g_v9.view, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(g_v9.view, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_snap_x(g_v9.view, LV_SCROLL_SNAP_NONE);

    g_v9.empty = smart_home_lvgl_label_create(
        g_v9.view, "该房间暂无设备", SMART_HOME_UI_COLOR_TEXT_MUTED, 16);
    lv_obj_center(g_v9.empty);
    lv_obj_add_flag(g_v9.empty, LV_OBJ_FLAG_HIDDEN);

    v9_create_demo_cards(ui);
    v9_refresh_rooms(ui);
    v9_scene_apply_label();
    smart_home_lvgl_v9_refresh_views(ui);

    if (ui->home_time_timer) {
        lv_timer_delete(ui->home_time_timer);
    }
    ui->home_time_timer = lv_timer_create(v9_time_timer_cb, 30000, ui);
    if (ui->v9_poll_timer) {
        lv_timer_delete(ui->v9_poll_timer);
    }
    ui->v9_poll_timer = lv_timer_create(v9_poll_timer_cb, 1000, ui);

    v9_update_time(ui);
    smart_home_lvgl_refresh_home(ui);
    v9_reflow();
    smart_home_lvgl_build_nav_bar(screen, ui);

#ifdef CONFIG_SMART_HOME_ONBOARDING
    /* 首次配置引导：仅当存在未完成项且本次运行未跳过时弹出。
     * 首页已完整构建，浮层只是覆盖在上层，关掉即可看演示数据。
     * 关掉 CONFIG_SMART_HOME_ONBOARDING 时整段不编译。 */
    if (smart_home_lvgl_onboarding_should_present(ui)) {
        smart_home_lvgl_onboarding_present(ui);
    }
#endif
}

int smart_home_lvgl_v9_visible_card_count(void)
{
    return g_v9.visible;
}
