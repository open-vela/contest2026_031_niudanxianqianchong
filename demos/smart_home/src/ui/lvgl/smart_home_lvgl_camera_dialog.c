/* SPDX-License-Identifier: Apache-2.0 */
#include "smart_home_lvgl_internal.h"
#include "smart_home_lvgl_camera_dialog.h"
#include "smart_home_lvgl_room_dialog.h"
#include "../../camera/smart_home_camera_service.h"
#include "../../camera/smart_home_camera_control.h"
#include "../../smart_home_memory.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    smart_home_lvgl_t *ui;
    smart_home_v9_device_view_t view;
    lv_obj_t *mask, *image, *status, *metrics, *sw, *placeholder;
    lv_timer_t *timer;
    uint8_t *pixels;
    lv_image_dsc_t image_data;
    uint32_t sequence, waiting_since;
    int timeout;
    bool memory_error;
} camera_dialog_t;
static camera_dialog_t *g_camera_dialog;

static void deleted(lv_event_t *event) {
    camera_dialog_t *ctx = lv_event_get_user_data(event);
    if (ctx->timer)
        lv_timer_delete(ctx->timer);
    (void)smart_home_camera_request(false);
    smart_home_bulk_free(ctx->pixels);
    if (g_camera_dialog == ctx)
        g_camera_dialog = NULL;
    smart_home_bulk_free(ctx);
}
void smart_home_lvgl_camera_dialog_close(void) {
    if (g_camera_dialog)
        lv_obj_delete(g_camera_dialog->mask);
}
static void close_cb(lv_event_t *event) {
    (void)event;
    smart_home_lvgl_camera_dialog_close();
}
static void stop_cb(lv_event_t *event) {
    lv_event_stop_bubbling(event);
}
static void room_cb(lv_event_t *event) {
    camera_dialog_t *ctx = lv_event_get_user_data(event);
    smart_home_lvgl_t *ui = ctx->ui;
    smart_home_v9_device_view_t view = ctx->view;
    smart_home_lvgl_camera_dialog_close();
    smart_home_lvgl_room_assign_open(ui, &view);
}
static lv_obj_t *text(lv_obj_t *p, const char *s, int x, int y, int size) {
    lv_obj_t *label =
        smart_home_lvgl_label_create(p, s, SMART_HOME_UI_COLOR_TEXT_PRIMARY, size);
    lv_obj_set_pos(label, x, y);
    return label;
}
static void button(lv_obj_t *p, const char *s, int x, int y, lv_event_cb_t cb,
                   void *data) {
    lv_obj_t *b = lv_button_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 120, 40);
    lv_obj_set_pos(b, x, y);
    smart_home_lvgl_set_bg(b, SMART_HOME_UI_COLOR_SURFACE_ON);
    lv_obj_set_style_radius(b, 10, 0);
    smart_home_lvgl_press_feedback(b);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    lv_obj_center(text(b, s, 0, 0, 16));
}
static void refresh(lv_timer_t *timer) {
    camera_dialog_t *ctx = lv_timer_get_user_data(timer);
    struct smart_home_camera_status_s status;
    smart_home_camera_control_status_t control;
    smart_home_camera_control_status(&control);
    if (smart_home_camera_get_status(&status)) {
        lv_label_set_text(ctx->status, "摄像头状态读取失败");
        return;
    }
    char value[96];
    bool frames = status.state == SMART_HOME_CAMERA_RUNNING && status.preview_sequence;
    bool requested = control.busy ? control.desired_on
                                  : status.state == SMART_HOME_CAMERA_RUNNING ||
                                        status.state == SMART_HOME_CAMERA_STARTING;
    if (requested)
        lv_obj_add_state(ctx->sw, LV_STATE_CHECKED);
    else
        lv_obj_remove_state(ctx->sw, LV_STATE_CHECKED);
    /* First-frame confirmation, not merely accepting the start request. */
    if (requested && !frames && !ctx->timeout) {
        if (!ctx->waiting_since)
            ctx->waiting_since = lv_tick_get() + 1;
        if ((uint32_t)(lv_tick_get() + 1 - ctx->waiting_since) >= 10000u) {
            ctx->timeout = 1;
            (void)smart_home_camera_request(false);
        }
    } else
        ctx->waiting_since = 0;
    if (ctx->timeout)
        snprintf(value, sizeof value, "未获取画面，已请求停止；请重新开启");
    else if (ctx->memory_error)
        snprintf(value, sizeof value, "预览内存不足，已请求停止；请重新开启");
    else if (control.busy && !control.desired_on)
        snprintf(value, sizeof value, "正在停止采集…");
    else if (control.result || status.state == SMART_HOME_CAMERA_ERROR)
        snprintf(value, sizeof value, "摄像头操作失败 (%d)",
                 control.result ? control.result : status.last_error);
    else if (frames)
        snprintf(value, sizeof value, "实时预览");
    else if (requested)
        snprintf(value, sizeof value, "启动中，等待首帧…");
    else
        snprintf(value, sizeof value, "预览已关闭");
    lv_label_set_text(ctx->status, value);
    snprintf(value, sizeof value, "预览帧率 %u.%02u FPS\n采集帧率 %u.%02u FPS",
             (unsigned)(status.preview_fps_x100 / 100),
             (unsigned)(status.preview_fps_x100 % 100),
             (unsigned)(status.capture_fps_x100 / 100),
             (unsigned)(status.capture_fps_x100 % 100));
    lv_label_set_text(ctx->metrics, value);
    if (frames && !ctx->memory_error && status.preview_sequence != ctx->sequence) {
        if (!ctx->pixels) {
            ctx->pixels = smart_home_bulk_alloc(SMART_HOME_CAMERA_PREVIEW_BYTES);
            if (ctx->pixels) {
                ctx->image_data.data = ctx->pixels;
                lv_image_set_src(ctx->image, &ctx->image_data);
            } else {
                ctx->memory_error = true;
                lv_label_set_text(ctx->status, "预览内存不足，已请求停止");
                (void)smart_home_camera_request(false);
            }
        }
        if (ctx->pixels &&
            !smart_home_camera_copy_latest(ctx->pixels, SMART_HOME_CAMERA_PREVIEW_BYTES,
                                           &ctx->sequence)) {
            lv_obj_clear_flag(ctx->image, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ctx->placeholder, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ctx->image);
        }
    }
    if (!frames) {
        lv_obj_add_flag(ctx->image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ctx->placeholder, LV_OBJ_FLAG_HIDDEN);
    }
    smart_home_lvgl_set_camera_indicator(ctx->ui, frames);
}
static void switch_cb(lv_event_t *event) {
    camera_dialog_t *ctx = lv_event_get_user_data(event);
    ctx->timeout = 0;
    ctx->memory_error = false;
    ctx->waiting_since = 0;
    ctx->sequence = 0;
    if (smart_home_camera_request(lv_obj_has_state(ctx->sw, LV_STATE_CHECKED)))
        lv_label_set_text(ctx->status, "无法提交摄像头操作，请重试");
    refresh(ctx->timer);
}
int smart_home_lvgl_camera_dialog_open(smart_home_lvgl_t *ui,
                                       const smart_home_v9_device_view_t *view) {
    smart_home_lvgl_camera_dialog_close();
    if (!ui || !ui->screen_home || !view ||
        strcmp(view->key, SMART_HOME_BOARD_CAMERA_KEY))
        return AGENT_ERROR_INVALID;
    camera_dialog_t *ctx = smart_home_bulk_alloc(sizeof(*ctx));
    if (!ctx)
        return AGENT_ERROR_NOMEM;
    memset(ctx, 0, sizeof(*ctx));
    ctx->ui = ui;
    ctx->view = *view;
    g_camera_dialog = ctx;
    ctx->mask = lv_obj_create(ui->screen_home);
    lv_obj_remove_style_all(ctx->mask);
    lv_obj_set_size(ctx->mask, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(ctx->mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ctx->mask, LV_OPA_30, 0);
    lv_obj_add_event_cb(ctx->mask, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ctx->mask, deleted, LV_EVENT_DELETE, ctx);
    lv_obj_t *panel = lv_obj_create(ctx->mask);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 840, 480);
    lv_obj_center(panel);
    smart_home_lvgl_card_style(panel);
    lv_obj_set_style_pad_all(panel, 24, 0);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_add_event_cb(panel, stop_cb, LV_EVENT_CLICKED, NULL);
    text(panel, "本机摄像头", 0, 0, 24);
    text(panel, smart_home_lvgl_room_display(view->room_label), 0, 40, 16);
    lv_obj_t *preview = lv_obj_create(panel);
    lv_obj_remove_style_all(preview);
    lv_obj_set_size(preview, 512, 300);
    lv_obj_set_pos(preview, 0, 82);
    smart_home_lvgl_set_bg(preview, SMART_HOME_UI_COLOR_SURFACE_ON);
    lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    ctx->image_data.header.magic = LV_IMAGE_HEADER_MAGIC;
    ctx->image_data.header.cf = LV_COLOR_FORMAT_RGB565;
    ctx->image_data.header.w = SMART_HOME_CAMERA_PREVIEW_WIDTH;
    ctx->image_data.header.h = SMART_HOME_CAMERA_PREVIEW_HEIGHT;
    ctx->image_data.header.stride = SMART_HOME_CAMERA_PREVIEW_WIDTH * 2;
    ctx->image_data.data_size = SMART_HOME_CAMERA_PREVIEW_BYTES;
    ctx->image = lv_image_create(preview);
    lv_obj_center(ctx->image);
    lv_obj_add_flag(ctx->image, LV_OBJ_FLAG_HIDDEN);
    ctx->placeholder = text(preview, "开启预览后显示实时画面", 0, 0, 18);
    lv_obj_center(ctx->placeholder);
    text(panel, "实时预览", 548, 92, 18);
    ctx->sw = lv_switch_create(panel);
    lv_obj_set_pos(ctx->sw, 706, 88);
    lv_obj_set_size(ctx->sw, 54, 30);
    lv_obj_set_style_bg_color(ctx->sw, lv_color_hex(0xCCD6D6), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ctx->sw, SMART_HOME_UI_COLOR_PRIMARY,
                              LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(ctx->sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->sw, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ctx->sw, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(ctx->sw, -4, LV_PART_KNOB);
    lv_obj_set_style_anim_duration(ctx->sw, 0, 0);
    smart_home_lvgl_press_feedback(ctx->sw);
    lv_obj_add_event_cb(ctx->sw, switch_cb, LV_EVENT_VALUE_CHANGED, ctx);
    ctx->metrics = text(panel, "", 548, 156, 15);
    button(panel, "分配房间", 548, 302, room_cb, ctx);
    button(panel, "关闭", 672, 392, close_cb, NULL);
    ctx->status = text(panel, "", 0, 398, 15);
    lv_obj_set_width(ctx->status, 650);
    ctx->timer = lv_timer_create(refresh, 66, ctx);
    refresh(ctx->timer);
    return AGENT_OK;
}
