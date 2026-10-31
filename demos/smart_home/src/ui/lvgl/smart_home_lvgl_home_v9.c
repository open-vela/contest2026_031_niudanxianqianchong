/** V9 merged Home: weather header, room filter and horizontal device cards. */

#include "smart_home_lvgl_internal.h"
#include "images/smart_home_icons.h"
#include "icons/smart_home_lvgl_png_icons.h"
#include "smart_home_v9_device_view.h"
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
#define V9_CARD_W       176
#define V9_CARD_H       244
#define V9_CARD_GAP     16
#define V9_VIEW_X       24
#define V9_VIEW_Y       260
#define V9_VIEW_W       976
#define V9_VIEW_H       256

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
    { "light", "吸顶灯", "客厅", "已开启", "已关闭", "80%", "0%", "亮度",
      103, 89, 61, 143, 1 },
    { "ac", "立式空调", "客厅", "制冷中", "已关闭", "24°", "--", "目标温度",
      98, 138, 66, 94, 1 },
    { "washer", "洗衣机", "阳台", "洗涤中", "待机", "32分", "--", "剩余时间",
      99, 114, 65, 118, 1 },
    { "robot", "扫地机器人", "客厅", "清扫中", "待机", "75%", "--", "电量",
      101, 58, 63, 174, 1 },
    { "camera", "摄像头", "客厅", "预览中", "预览关闭", "在线", "在线", "设备状态",
      92, 110, 72, 122, 0 },
    { "fan", "落地风扇", "卧室", "运行中", "已关闭", "2档", "--", "风速",
      88, 124, 76, 108, 1 },
};

static const char *const g_v9_rooms[V9_ROOM_COUNT] = {
    "客厅", "卧室", "书房", "阳台", "厨房"
};

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *view;
    lv_obj_t *empty;
    lv_obj_t *room_btn[V9_ROOM_COUNT + 1];
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
    lv_obj_t *modal;
} v9_home_state_t;

static v9_home_state_t g_v9;

#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
static void v9_camera_preview_stop(void);
#endif

static void v9_close_modal(void)
{
#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
    v9_camera_preview_stop();
#endif
    if (g_v9.modal) {
        lv_obj_delete(g_v9.modal);
        g_v9.modal = NULL;
    }
}

#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
static lv_timer_t *g_cam_timer;
static void *g_cam_buffer;
static lv_image_dsc_t g_cam_image;
static lv_obj_t *g_cam_view;
static int g_cam_started;
static uint32_t g_cam_sequence;

static void v9_camera_timer_cb(lv_timer_t *timer)
{
    uint32_t sequence = 0;

    (void)timer;
    if (!g_cam_buffer || !g_cam_view) {
        return;
    }
    if (smart_home_camera_copy_latest(g_cam_buffer,
                                      SMART_HOME_CAMERA_PREVIEW_BYTES,
                                      &sequence) == AGENT_OK &&
        sequence != g_cam_sequence) {
        g_cam_sequence = sequence;
        lv_obj_invalidate(g_cam_view);
    }
}

static void v9_camera_preview_stop(void)
{
    if (g_cam_timer) {
        lv_timer_delete(g_cam_timer);
        g_cam_timer = NULL;
    }
    g_cam_view = NULL;
    if (g_cam_buffer) {
        smart_home_bulk_free(g_cam_buffer);
        g_cam_buffer = NULL;
    }
    if (g_cam_started) {
        (void)smart_home_camera_stop();
        g_cam_started = 0;
    }
}

static int v9_camera_preview_start(smart_home_lvgl_t *ui, lv_obj_t *panel)
{
    if (!ui || !panel || g_cam_timer) {
        return 0;
    }
    g_cam_buffer = smart_home_bulk_alloc(SMART_HOME_CAMERA_PREVIEW_BYTES);
    if (!g_cam_buffer) {
        lv_obj_t *label = smart_home_lvgl_label_create(
            panel, "预览缓冲区分配失败", SMART_HOME_UI_COLOR_DANGER, 14);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 60);
        return -1;
    }
    memset(g_cam_buffer, 0, SMART_HOME_CAMERA_PREVIEW_BYTES);
    memset(&g_cam_image, 0, sizeof(g_cam_image));
    g_cam_image.header.magic = LV_IMAGE_HEADER_MAGIC;
    g_cam_image.header.cf = LV_COLOR_FORMAT_RGB565;
    g_cam_image.header.w = SMART_HOME_CAMERA_PREVIEW_WIDTH;
    g_cam_image.header.h = SMART_HOME_CAMERA_PREVIEW_HEIGHT;
    g_cam_image.header.stride = SMART_HOME_CAMERA_PREVIEW_WIDTH * 2u;
    g_cam_image.data_size = SMART_HOME_CAMERA_PREVIEW_BYTES;
    g_cam_image.data = g_cam_buffer;

    g_cam_view = lv_image_create(panel);
    lv_image_set_src(g_cam_view, &g_cam_image);
    lv_obj_set_size(g_cam_view, SMART_HOME_CAMERA_PREVIEW_WIDTH,
                    SMART_HOME_CAMERA_PREVIEW_HEIGHT);
    lv_obj_align(g_cam_view, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_radius(g_cam_view, 10, 0);
    lv_obj_clear_flag(g_cam_view, LV_OBJ_FLAG_CLICKABLE);

    if (smart_home_camera_start() == AGENT_OK) {
        g_cam_started = 1;
    }
    g_cam_timer = lv_timer_create(v9_camera_timer_cb, 66, ui);
    return 0;
}
#endif

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
    const char *state = on ? "on" : "off";

    if (g_v9.status[index]) {
        lv_label_set_text(g_v9.status[index], on ? def->status_on : def->status_off);
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
                            g_v9_rooms[g_v9.room_filter - 1]);

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
    if (index < 0 || index > V9_ROOM_COUNT) {
        return -1;
    }
    g_v9.room_filter = index;
    for (int i = 0; i <= V9_ROOM_COUNT; i++) {
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
    int index = (int)(intptr_t)lv_event_get_user_data(event);

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
    if (!view->state_valid) return "状态待获取";
    switch (view->kind) {
    case SMART_HOME_V9_KIND_WASHER:
        return view->on ? "洗涤中" : "待机";
    case SMART_HOME_V9_KIND_ROBOT:
        return view->on ? "清扫中" : "待机";
    case SMART_HOME_V9_KIND_CAMERA:
        return view->on ? "预览中" : "预览关闭";
    default:
        return view->on ? "已开启" : "已关闭";
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
    state = view->on ? "on" : "off";
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
        if (view->kind == SMART_HOME_V9_KIND_CAMERA) {
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

static void v9_route_cb(lv_event_t *event);
static int kind_def_index(smart_home_v9_kind_t kind);

static void v9_card_click_cb(lv_event_t *event)
{
    lv_obj_t *card = lv_event_get_current_target(event);
    int index = (int)(intptr_t)lv_obj_get_user_data(card);
    lv_obj_t *panel;
    lv_obj_t *label;
    char text[160];

    if (lv_event_get_code(event) != LV_EVENT_CLICKED ||
        lv_event_get_target(event) != lv_event_get_current_target(event) ||
        index < 0 || index >= g_v9.card_count) {
        return;
    }

    panel = v9_modal_panel(g_v9.name_text[index]);
    snprintf(text, sizeof(text), "%s · %s", g_v9.room_text[index],
             g_v9.status[index] ? lv_label_get_text(g_v9.status[index]) :
                                  (g_v9.on[index] ? "已开启" : "已关闭"));
    label = smart_home_lvgl_label_create(panel, text,
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY, 16);
    lv_obj_set_pos(label, 0, 48);

    label = smart_home_lvgl_label_create(
        panel, g_v9.metric[index] ? lv_label_get_text(g_v9.metric[index]) :
                                    "--",
        SMART_HOME_UI_COLOR_TEXT_PRIMARY, 32);
    lv_obj_set_pos(label, 0, 96);

    label = smart_home_lvgl_label_create(panel, "设备详情控制将在 M3 接入",
                                         SMART_HOME_UI_COLOR_TEXT_MUTED, 14);
    lv_obj_set_pos(label, 0, 152);
    if (g_v9.def_index[index] == kind_def_index(SMART_HOME_V9_KIND_CAMERA) &&
        !g_v9.real[index]) {
        smart_home_lvgl_t *ui = lv_event_get_user_data(event);
#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
        (void)v9_camera_preview_start(ui, panel);
#endif
        lv_obj_t *button = lv_button_create(panel);
        lv_obj_t *button_label;

        if (ui && ui->screen_security) {
            lv_obj_remove_style_all(button);
            lv_obj_set_size(button, 180, 40);
            lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, 0, 0);
            lv_obj_set_style_radius(button, 10, 0);
            lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE_ON,
                                      0);
            lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
            smart_home_lvgl_press_feedback(button);
            lv_obj_add_event_cb(button, v9_route_cb, LV_EVENT_CLICKED,
                                ui->screen_security);
            button_label = smart_home_lvgl_label_create(
                button, "打开摄像头预览", SMART_HOME_UI_COLOR_TEXT_PRIMARY, 14);
            lv_obj_center(button_label);
        }
    }
    v9_modal_close_button(panel);
}

static void v9_route_cb(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_user_data(event);

    if (lv_event_get_code(event) == LV_EVENT_CLICKED && target) {
        v9_close_modal();
        lv_scr_load(target);
    }
}

static void v9_family_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);

    if (lv_event_get_code(event) == LV_EVENT_CLICKED && ui) {
        smart_home_lvgl_open_scene_popup(ui);
    }
}

static void v9_add_room_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        v9_info_modal("房间管理", "房间新增、删除与设备迁移将在 M3 接入。");
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

static lv_obj_t *v9_make_room_button(lv_obj_t *screen, int index,
                                     const char *text)
{
    lv_obj_t *button = lv_button_create(screen);
    lv_obj_t *label;

    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 78, 40);
    lv_obj_set_pos(button, 28 + index * 84, 204);
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
    lv_obj_center(label);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return button;
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
    g_v9.status[index] = v9_text(card, def->status_on, 14, 79, 12,
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
    for (int i = 0; i < V9_DEVICE_COUNT; i++) {
        v9_make_card(ui, i);
    }
    g_v9.card_count = V9_DEVICE_COUNT;
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

void smart_home_lvgl_v9_refresh_views(smart_home_lvgl_t *ui)
{
    smart_home_v9_device_view_t raw[V9_MAX_CARDS];
    smart_home_v9_device_view_t views[V9_MAX_CARDS];
    size_t count = 0;
    size_t valid = 0;

    if (!ui || !g_v9.screen) {
        return;
    }
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (ui->app && ui->app->miloco &&
        smart_home_miloco_reachable(ui->app->miloco) &&
        smart_home_miloco_bound(ui->app->miloco)) {
        count = smart_home_v9_collect_views(NULL, ui->app->miloco, raw,
                                            V9_MAX_CARDS);
    }
#endif
    if (count == 0 && ui->app && ui->device_state &&
        ui->app->device_service.mutex_initialized) {
        count = smart_home_v9_collect_views(ui->device_state, NULL, raw,
                                            V9_MAX_CARDS);
    }
    for (size_t i = 0; i < count; i++) {
        if (kind_def_index(raw[i].kind) < 0) {
            continue;
        }
        if (raw[i].source == SMART_HOME_V9_SOURCE_MILOCO &&
            raw[i].name[0] == '\0') {
            continue;
        }
        views[valid++] = raw[i];
    }
    if (valid == 0) {
        if (g_v9.dynamic || g_v9.card_count == 0) {
            v9_create_demo_cards(ui);
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
            return;
        }
    }

    v9_destroy_cards();
    for (size_t i = 0; i < valid; i++) {
        v9_make_dynamic_card(ui, (int)i, &views[i]);
    }
    g_v9.card_count = (int)valid;
    g_v9.dynamic = 1;
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
    lv_obj_t *button;
    lv_obj_t *label;

    if (!ui) {
        return;
    }
    memset(&g_v9, 0, sizeof(g_v9));

    /* Drop pointers owned by a previous (legacy) home build. */
    ui->home_env_label = NULL;
    ui->home_miloco_sub_label = NULL;
    ui->home_ac_label = NULL;
    ui->home_status_label = NULL;
    ui->home_weather_city_label = NULL;
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

    badge = lv_obj_create(screen);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 32, 32);
    lv_obj_set_pos(badge, 348, 124);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    ui->home_weather_badge = badge;
    icon = lv_image_create(badge);
    lv_image_set_src(icon, smart_home_lvgl_png_icon_get("asset:sun", 32));
    lv_obj_set_size(icon, 32, 32);
    lv_obj_center(icon);

    button = lv_button_create(screen);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 158, 40);
    lv_obj_set_pos(button, 838, 96);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, SMART_HOME_UI_COLOR_BORDER, 0);
    lv_obj_add_event_cb(button, v9_family_cb, LV_EVENT_CLICKED, ui);
    smart_home_lvgl_press_feedback(button);
    label = smart_home_lvgl_label_create(button, "家庭模式",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 18);
    lv_obj_center(label);

    label = smart_home_lvgl_label_create(screen, "演示数据",
                                         SMART_HOME_UI_COLOR_TEXT_MUTED, 11);
    lv_obj_set_pos(label, 838, 142);

    g_v9.room_btn[0] = v9_make_room_button(screen, 0, "全部");
    for (int i = 0; i < V9_ROOM_COUNT; i++) {
        g_v9.room_btn[i + 1] = v9_make_room_button(screen, i + 1,
                                                    g_v9_rooms[i]);
    }
    button = lv_button_create(screen);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 48, 40);
    lv_obj_set_pos(button, 532, 204);
    lv_obj_set_style_radius(button, 16, 0);
    lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_ext_click_area(button, 4);
    lv_obj_add_event_cb(button, v9_add_room_cb, LV_EVENT_CLICKED, NULL);
    smart_home_lvgl_press_feedback(button);
    label = smart_home_lvgl_label_create(button, "+",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 24);
    lv_obj_center(label);

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
}

int smart_home_lvgl_v9_visible_card_count(void)
{
    return g_v9.visible;
}
