/* Capability-driven device controls. All network I/O stays in the gateway worker. */
#include "smart_home_lvgl_internal.h"
#include "smart_home_lvgl_device_dialog.h"
#include "smart_home_lvgl_room_dialog.h"
#include "../../smart_home_memory.h"
#include "../../miloco/smart_home_miloco.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define LOCAL_POWER 64
#define LOCAL_VALUE 65
#define LOCAL_MODE 66
#define LOCAL_FAN 67
#define DIALOG_CONTROL_MAX 9

typedef struct device_dialog device_dialog_t;
typedef struct {
    device_dialog_t *owner;
    smart_home_miloco_control_t spec;
    lv_obj_t *widget;
    lv_obj_t *value_label;
    int kind;
    int32_t confirmed;
    int32_t desired;
    uint32_t request_id;
    uint32_t started;
    int pending;
} dialog_control_t;
struct device_dialog {
    smart_home_lvgl_t *ui;
    smart_home_v9_device_view_t view;
    lv_obj_t *mask;
    lv_obj_t *status;
    lv_obj_t *body;
    lv_timer_t *timer;
    int count;
    char art_path[256];
    dialog_control_t control[DIALOG_CONTROL_MAX];
};
static device_dialog_t *g_dialog;

static void feedback(device_dialog_t *ctx, const char *text, int error) {
    lv_label_set_text(ctx->status, text);
    lv_obj_set_style_text_color(
        ctx->status,
        error ? SMART_HOME_UI_COLOR_DANGER : SMART_HOME_UI_COLOR_TEXT_SECONDARY, 0);
}
static void deleted(lv_event_t *event) {
    device_dialog_t *ctx = lv_event_get_user_data(event);
    if (ctx->timer)
        lv_timer_delete(ctx->timer);
    if (g_dialog == ctx)
        g_dialog = NULL;
    smart_home_bulk_free(ctx);
}
void smart_home_lvgl_device_dialog_close(void) {
    if (g_dialog)
        lv_obj_delete(g_dialog->mask);
}
static void close_cb(lv_event_t *event) {
    (void)event;
    smart_home_lvgl_device_dialog_close();
}
static void room_cb(lv_event_t *event) {
    device_dialog_t *ctx = lv_event_get_user_data(event);
    smart_home_lvgl_t *ui = ctx->ui;
    smart_home_v9_device_view_t view = ctx->view;
    smart_home_lvgl_device_dialog_close();
    smart_home_lvgl_room_assign_open(ui, &view);
}
static void stop_cb(lv_event_t *event) {
    lv_event_stop_bubbling(event);
}

static int local_device(device_dialog_t *ctx, smart_home_device_t *device) {
    if (!ctx->ui || !ctx->ui->app || strncmp(ctx->view.key, "local:", 6))
        return AGENT_ERROR_NOT_FOUND;
    return smart_home_device_service_get_device(&ctx->ui->app->device_service,
                                                atoi(ctx->view.key + 6), device);
}
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
static smart_home_miloco_t *gateway(device_dialog_t *ctx) {
    return ctx->ui && ctx->ui->app ? ctx->ui->app->miloco : NULL;
}
static const smart_home_miloco_control_t *
find_control(const smart_home_miloco_device_t *device, const char *iid) {
    for (unsigned i = 0;
         i < device->control_count && i < SMART_HOME_MILOCO_MAX_CONTROLS; i++)
        if (!strcmp(device->controls[i].iid, iid))
            return &device->controls[i];
    return NULL;
}
#endif
static void show_value(dialog_control_t *control, int32_t value) {
    char text[32];
    if (control->kind == LOCAL_POWER || control->kind == SMART_HOME_MILOCO_CTRL_BOOL) {
        if (value)
            lv_obj_add_state(control->widget, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(control->widget, LV_STATE_CHECKED);
    } else if (control->kind == LOCAL_MODE) {
        lv_dropdown_set_selected(control->widget, value >= 0 && value <= 4 ? value : 0);
    } else if (control->kind == SMART_HOME_MILOCO_CTRL_ENUM ||
               control->kind == LOCAL_FAN) {
        for (unsigned i = 0; i < control->spec.option_count; i++)
            if (control->spec.options[i].value == value)
                lv_dropdown_set_selected(control->widget, i);
    } else if (control->kind == SMART_HOME_MILOCO_CTRL_RANGE ||
               control->kind == LOCAL_VALUE) {
        lv_slider_set_value(control->widget, value, LV_ANIM_OFF);
        snprintf(text, sizeof(text), "%ld", (long)value);
        lv_label_set_text(control->value_label, text);
    }
}
static int32_t widget_value(dialog_control_t *control) {
    if (control->kind == LOCAL_POWER || control->kind == SMART_HOME_MILOCO_CTRL_BOOL)
        return lv_obj_has_state(control->widget, LV_STATE_CHECKED);
    if (control->kind == LOCAL_MODE)
        return lv_dropdown_get_selected(control->widget);
    if (control->kind == SMART_HOME_MILOCO_CTRL_ENUM || control->kind == LOCAL_FAN) {
        unsigned index = lv_dropdown_get_selected(control->widget);
        return index < control->spec.option_count ? control->spec.options[index].value
                                                  : control->confirmed;
    }
    if (control->kind == SMART_HOME_MILOCO_CTRL_RANGE || control->kind == LOCAL_VALUE) {
        int32_t value = lv_slider_get_value(control->widget);
        int32_t minimum = control->spec.range.minimum;
        int32_t step = control->spec.range.step;
        return minimum + ((value - minimum) / step) * step;
    }
    return 0;
}
static void submit_cb(lv_event_t *event) {
    dialog_control_t *control = lv_event_get_user_data(event);
    device_dialog_t *ctx = control->owner;
    lv_event_code_t code = lv_event_get_code(event);
    int is_range =
        control->kind == LOCAL_VALUE || control->kind == SMART_HOME_MILOCO_CTRL_RANGE;
    int is_action = control->kind == SMART_HOME_MILOCO_CTRL_ACTION;
    int32_t value;
    if (control->pending)
        return;
    if (is_range && code == LV_EVENT_VALUE_CHANGED) {
        char text[32];
        snprintf(text, sizeof(text), "%ld", (long)widget_value(control));
        lv_label_set_text(control->value_label, text);
        return;
    }
    if (code != (is_range    ? LV_EVENT_RELEASED
                 : is_action ? LV_EVENT_CLICKED
                             : LV_EVENT_VALUE_CHANGED))
        return;
    value = widget_value(control);
    if (ctx->view.source == SMART_HOME_V9_SOURCE_LOCAL) {
        smart_home_device_t device;
        int result = local_device(ctx, &device);
        if (result == AGENT_OK) {
            int on = device.on;
            int primary = device.type == SMART_HOME_DEVICE_LIGHT ? device.brightness
                                                                 : device.temperature;
            int mode = device.ac_mode, speed = device.ac_fan_speed;
            if (control->kind == LOCAL_POWER)
                on = value;
            else if (control->kind == LOCAL_VALUE)
                primary = value;
            else if (control->kind == LOCAL_MODE)
                mode = value;
            else if (control->kind == LOCAL_FAN)
                speed = value;
            result = smart_home_device_service_set_device_control(
                &ctx->ui->app->device_service, device.id, on, primary, mode, speed);
        }
        if (result == AGENT_OK) {
            control->confirmed = value;
            show_value(control, value);
            feedback(ctx, "设备状态已更新", 0);
            smart_home_lvgl_v9_refresh_views(ctx->ui);
        } else {
            show_value(control, control->confirmed);
            feedback(ctx, "操作失败，设备可能已移除", 1);
        }
        return;
    }
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (ctx->view.source == SMART_HOME_V9_SOURCE_MILOCO) {
        smart_home_miloco_device_t device;
        smart_home_miloco_t *service = gateway(ctx);
        if (!service || !smart_home_miloco_reachable(service) ||
            !smart_home_miloco_copy_device(service, ctx->view.key + 5, &device) ||
            !device.online || !find_control(&device, control->spec.iid)) {
            show_value(control, control->confirmed);
            feedback(ctx, "设备当前离线或控制能力已变化", 1);
            return;
        }
        int result = smart_home_miloco_submit_control_tracked(
            service, device.did, control->spec.iid, is_action ? "action" : "set", value,
            &control->request_id);
        if (result == AGENT_OK) {
            control->pending = 1;
            control->desired = value;
            control->started = lv_tick_get();
            lv_obj_add_state(control->widget, LV_STATE_DISABLED);
            feedback(ctx, "已提交，等待设备确认…", 0);
        } else {
            show_value(control, control->confirmed);
            feedback(ctx, "提交失败，请稍后重试", 1);
        }
        return;
    }
#endif
    show_value(control, control->confirmed);
    feedback(ctx, "演示设备不执行真实控制", 0);
}
static void poll(lv_timer_t *timer) {
    device_dialog_t *ctx = lv_timer_get_user_data(timer);
    smart_home_device_t local;
    int local_ok = ctx->view.source == SMART_HOME_V9_SOURCE_LOCAL &&
                   local_device(ctx, &local) == AGENT_OK;
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    smart_home_miloco_device_t remote;
    smart_home_miloco_t *service = gateway(ctx);
    int remote_ok =
        ctx->view.source == SMART_HOME_V9_SOURCE_MILOCO && service &&
        smart_home_miloco_reachable(service) &&
        smart_home_miloco_copy_device(service, ctx->view.key + 5, &remote) &&
        remote.online;
#endif
    for (int i = 0; i < ctx->count; i++) {
        dialog_control_t *c = &ctx->control[i];
        int available = local_ok;
        int32_t current = c->confirmed;
        if (local_ok) {
            current = c->kind == LOCAL_POWER                  ? local.on
                      : c->kind == LOCAL_MODE                 ? local.ac_mode
                      : c->kind == LOCAL_FAN                  ? local.ac_fan_speed
                      : local.type == SMART_HOME_DEVICE_LIGHT ? local.brightness
                                                              : local.temperature;
        }
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
        const smart_home_miloco_control_t *actual =
            remote_ok ? find_control(&remote, c->spec.iid) : NULL;
        if (actual && actual->type == c->kind) {
            available = 1;
            current = actual->value;
        }
        if (c->pending) {
            int result = AGENT_ERROR_STATE;
            int done = service && smart_home_miloco_control_result(
                                      service, c->request_id, &result);
            if (done && result != AGENT_OK) {
                c->pending = 0;
                feedback(ctx, "设备执行失败，已恢复确认状态", 1);
            } else if (done && (c->kind == SMART_HOME_MILOCO_CTRL_ACTION ||
                                (actual && current == c->desired))) {
                c->pending = 0;
                feedback(ctx, c->kind == SMART_HOME_MILOCO_CTRL_ACTION ?
                        "网关已确认动作" : "设备已确认本次操作", 0);
            } else if ((uint32_t)(lv_tick_get() - c->started) >= 10000u) {
                c->pending = 0;
                feedback(ctx, "未收到设备确认，请检查连接后重试", 1);
            }
        }
#endif
        if (available && !c->pending)
            lv_obj_remove_state(c->widget, LV_STATE_DISABLED);
        else
            lv_obj_add_state(c->widget, LV_STATE_DISABLED);
        if (available)
            c->confirmed = current;
        if (!c->pending && !lv_obj_has_state(c->widget, LV_STATE_PRESSED))
            show_value(c, c->confirmed);
    }
}
static dialog_control_t *add_control(device_dialog_t *ctx, const char *name, int kind,
                                     int value,
                                     const smart_home_miloco_control_t *spec) {
    if (ctx->count >= DIALOG_CONTROL_MAX)
        return NULL;
    dialog_control_t *c = &ctx->control[ctx->count];
    int y = ctx->count++ * 76;
    c->owner = ctx;
    c->kind = kind;
    c->confirmed = value;
    if (spec)
        c->spec = *spec;
    lv_obj_t *label = smart_home_lvgl_label_create(
        ctx->body, name, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 17);
    lv_obj_set_pos(label, 4, y + 2);
    if (kind == LOCAL_POWER || kind == SMART_HOME_MILOCO_CTRL_BOOL) {
        c->widget = lv_switch_create(ctx->body);
        lv_obj_set_size(c->widget, 54, 30);
        lv_obj_set_pos(c->widget, 398, y + 8);
        lv_obj_set_style_bg_color(c->widget, lv_color_hex(0xCCD6D6), LV_PART_MAIN);
        lv_obj_set_style_bg_color(c->widget, lv_color_white(), LV_PART_KNOB);
        lv_obj_set_style_pad_all(c->widget, -4, LV_PART_KNOB);
    } else if (kind == SMART_HOME_MILOCO_CTRL_ACTION) {
        c->widget = lv_button_create(ctx->body);
        lv_obj_remove_style_all(c->widget);
        smart_home_lvgl_set_bg(c->widget, SMART_HOME_UI_COLOR_SURFACE_ON);
        lv_obj_set_style_radius(c->widget, 10, 0);
        lv_obj_set_size(c->widget, 120, 40);
        lv_obj_set_pos(c->widget, 332, y + 4);
        lv_obj_t *text = smart_home_lvgl_label_create(
            c->widget, "执行", SMART_HOME_UI_COLOR_TEXT_PRIMARY, 16);
        lv_obj_center(text);
        smart_home_lvgl_press_feedback(c->widget);
    } else if (kind == LOCAL_MODE || kind == LOCAL_FAN ||
               kind == SMART_HOME_MILOCO_CTRL_ENUM) {
        char options[128] = "";
        for (unsigned i = 0; i < c->spec.option_count; i++) {
            size_t n = strlen(options);
            snprintf(options + n, sizeof(options) - n, "%s%s", i ? "\n" : "",
                     c->spec.options[i].name);
        }
        if (kind == LOCAL_MODE)
            snprintf(options, sizeof(options), "制冷\n制热\n除湿\n送风\n自动");
        c->widget = lv_dropdown_create(ctx->body);
        lv_obj_set_size(c->widget, 256, 40);
        lv_obj_set_pos(c->widget, 196, y + 22);
        lv_dropdown_set_options(c->widget, options);
        smart_home_lvgl_style_dropdown(c->widget, 16);
    } else {
        c->value_label = smart_home_lvgl_label_create(
            ctx->body, "", SMART_HOME_UI_COLOR_TEXT_SECONDARY, 17);
        lv_obj_set_pos(c->value_label, 398, y + 2);
        c->widget = lv_slider_create(ctx->body);
        lv_obj_set_size(c->widget, 420, 12);
        lv_obj_set_pos(c->widget, 18, y + 42);
        lv_slider_set_range(c->widget, c->spec.range.minimum, c->spec.range.maximum);
        lv_obj_add_event_cb(c->widget, submit_cb, LV_EVENT_RELEASED, c);
    }
    lv_obj_set_style_bg_color(c->widget, SMART_HOME_UI_COLOR_PRIMARY,
                              LV_PART_INDICATOR);
    show_value(c, value);
    if (ctx->view.source == SMART_HOME_V9_SOURCE_DEMO)
        lv_obj_add_state(c->widget, LV_STATE_DISABLED);
    lv_obj_add_event_cb(c->widget, submit_cb,
                        kind == SMART_HOME_MILOCO_CTRL_ACTION ? LV_EVENT_CLICKED
                                                              : LV_EVENT_VALUE_CHANGED,
                        c);
    return c;
}
void smart_home_lvgl_device_dialog_open(smart_home_lvgl_t *ui,
                                        const smart_home_v9_device_view_t *view) {
    if (!ui || !ui->screen_home || !view)
        return;
    smart_home_lvgl_device_dialog_close();
    device_dialog_t *ctx = smart_home_bulk_alloc(sizeof(*ctx));
    if (!ctx)
        return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->ui = ui;
    ctx->view = *view;
    g_dialog = ctx;
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
    lv_obj_t *label = smart_home_lvgl_label_create(
        panel, view->name, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 24);
    lv_obj_set_pos(label, 0, 0);
    label = smart_home_lvgl_label_create(panel, smart_home_lvgl_room_display(view->room_label),
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY, 16);
    lv_obj_set_pos(label, 0, 42);
    if (view->source == SMART_HOME_V9_SOURCE_LOCAL || view->source == SMART_HOME_V9_SOURCE_MILOCO) {
        lv_obj_t *room = lv_button_create(panel);
        lv_obj_remove_style_all(room);
        lv_obj_set_pos(room, 180, 36);
        lv_obj_set_size(room, 108, 32);
        smart_home_lvgl_set_bg(room, SMART_HOME_UI_COLOR_SURFACE_ON);
        lv_obj_set_style_radius(room, 8, 0);
        smart_home_lvgl_press_feedback(room);
        lv_obj_add_event_cb(room, room_cb, LV_EVENT_CLICKED, ctx);
        label = smart_home_lvgl_label_create(room, "分配房间", SMART_HOME_UI_COLOR_TEXT_PRIMARY, 14);
        lv_obj_center(label);
    }
    lv_obj_t *art_area = lv_obj_create(panel);
    lv_obj_remove_style_all(art_area);
    lv_obj_set_size(art_area, 264, 280);
    lv_obj_set_pos(art_area, 0, 84);
    lv_obj_clear_flag(art_area, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    const char *ids[] = {"light", "ac", "washer", "robot", "camera", "fan"};
    if (view->kind >= 0 && view->kind < 6) {
        snprintf(ctx->art_path, sizeof(ctx->art_path), "A:%s/res/devices/%s-%s.png",
                 CONFIG_SMART_HOME_DEMO_DATA_ROOT, ids[view->kind],
                 view->on ? "on" : "off");
        lv_obj_t *art = lv_image_create(art_area);
        lv_image_set_src(art, ctx->art_path);
        lv_image_set_antialias(art, false);
        lv_image_set_scale(art, 512);
        lv_obj_center(art);
    }
    ctx->body = lv_obj_create(panel);
    lv_obj_remove_style_all(ctx->body);
    lv_obj_set_size(ctx->body, 472, 304);
    lv_obj_set_pos(ctx->body, 304, 66);
    lv_obj_set_scroll_dir(ctx->body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ctx->body, LV_SCROLLBAR_MODE_AUTO);
    ctx->status =
        smart_home_lvgl_label_create(panel, "", SMART_HOME_UI_COLOR_TEXT_SECONDARY, 14);
    lv_obj_set_width(ctx->status, 620);
    lv_label_set_long_mode(ctx->status, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(ctx->status, 0, 386);
    lv_obj_t *close = lv_button_create(panel);
    lv_obj_remove_style_all(close);
    smart_home_lvgl_set_bg(close, SMART_HOME_UI_COLOR_SURFACE_ON);
    lv_obj_set_style_radius(close, 10, 0);
    lv_obj_set_size(close, 120, 40);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    smart_home_lvgl_press_feedback(close);
    lv_obj_add_event_cb(close, close_cb, LV_EVENT_CLICKED, NULL);
    label = smart_home_lvgl_label_create(close, "关闭",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 16);
    lv_obj_center(label);
    if (view->source == SMART_HOME_V9_SOURCE_LOCAL ||
        view->source == SMART_HOME_V9_SOURCE_DEMO) {
        smart_home_device_t device = {.on = view->on,
                                      .brightness = view->primary_value,
                                      .temperature = view->primary_value,
                                      .ac_fan_speed = 3};
        if (view->source == SMART_HOME_V9_SOURCE_LOCAL &&
            local_device(ctx, &device) != AGENT_OK) {
            feedback(ctx, "设备已移除，请关闭后重试", 1);
            return;
        }
        if (view->kind == SMART_HOME_V9_KIND_LIGHT ||
            view->kind == SMART_HOME_V9_KIND_AC) {
            add_control(ctx, "设备开关", LOCAL_POWER, device.on, NULL);
            smart_home_miloco_control_t spec = {0};
            spec.range.step = 1;
            spec.range.minimum = view->kind == SMART_HOME_V9_KIND_LIGHT ? 0 : 16;
            spec.range.maximum = view->kind == SMART_HOME_V9_KIND_LIGHT ? 100 : 30;
            add_control(ctx,
                        view->kind == SMART_HOME_V9_KIND_LIGHT ? "亮度 / %"
                                                               : "目标温度 / °C",
                        LOCAL_VALUE,
                        view->kind == SMART_HOME_V9_KIND_LIGHT ? device.brightness
                                                               : device.temperature,
                        &spec);
            if (view->kind == SMART_HOME_V9_KIND_AC) {
                add_control(ctx, "运行模式", LOCAL_MODE, device.ac_mode, NULL);
                memset(&spec, 0, sizeof(spec));
                spec.option_count = 4;
                const char *speeds[] = {"低", "中", "高", "自动"};
                for (int i = 0; i < 4; i++) {
                    snprintf(spec.options[i].name, 12, "%s", speeds[i]);
                    spec.options[i].value = i;
                }
                add_control(ctx, "风速", LOCAL_FAN, device.ac_fan_speed, &spec);
            }
        }
        if (view->source == SMART_HOME_V9_SOURCE_DEMO && ctx->count == 0) {
            smart_home_miloco_control_t spec = {0};
            spec.option_count = 3;
            const char *const washer[] = {"标准洗", "快洗", "脱水"};
            const char *const robot[] = {"扫地", "拖地", "先扫后拖"};
            const char *const fan[] = {"标准风", "自然风", "睡眠风"};
            const char *const *names = view->kind == SMART_HOME_V9_KIND_WASHER  ? washer
                                       : view->kind == SMART_HOME_V9_KIND_ROBOT ? robot
                                                                                : fan;
            for (int i = 0; i < 3; i++) {
                snprintf(spec.options[i].name, 12, "%s", names[i]);
                spec.options[i].value = i;
            }
            add_control(ctx,
                        view->kind == SMART_HOME_V9_KIND_WASHER  ? "洗涤程序"
                        : view->kind == SMART_HOME_V9_KIND_ROBOT ? "清扫方式"
                                                                 : "送风方式",
                        SMART_HOME_MILOCO_CTRL_ENUM, 0, &spec);
            if (view->kind == SMART_HOME_V9_KIND_FAN) {
                memset(&spec, 0, sizeof(spec));
                spec.range.minimum = 1;
                spec.range.maximum = 3;
                spec.range.step = 1;
                add_control(ctx, "风速 / 档", SMART_HOME_MILOCO_CTRL_RANGE,
                            view->primary_value, &spec);
                add_control(ctx, "左右摇头", SMART_HOME_MILOCO_CTRL_BOOL, 0, NULL);
            } else {
                add_control(ctx, "运行 / 暂停", SMART_HOME_MILOCO_CTRL_BOOL, view->on,
                            NULL);
                add_control(ctx,
                            view->kind == SMART_HOME_V9_KIND_ROBOT ? "返回基站"
                                                                   : "结束洗涤",
                            SMART_HOME_MILOCO_CTRL_ACTION, 0, NULL);
            }
        }
        feedback(ctx,
                 view->source == SMART_HOME_V9_SOURCE_DEMO
                     ? "演示设备 · 不执行真实控制"
                     : "本地设备 · 状态由设备服务确认",
                 0);
    }
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    else if (view->source == SMART_HOME_V9_SOURCE_MILOCO) {
        smart_home_miloco_device_t remote;
        if (smart_home_miloco_copy_device(gateway(ctx), view->key + 5, &remote)) {
            for (unsigned i = 0;
                 i < remote.control_count && i < SMART_HOME_MILOCO_MAX_CONTROLS; i++) {
                const smart_home_miloco_control_t *c = &remote.controls[i];
                if (c->type == SMART_HOME_MILOCO_CTRL_ENUM && !c->option_count)
                    continue;
                add_control(ctx, c->desc[0] ? c->desc : c->type_name, c->type, c->value,
                            c);
            }
        }
        feedback(ctx,
                 ctx->count ? "米家设备 · 按设备实际能力显示"
                            : "设备未提供可控属性或动作",
                 0);
    }
#endif
    if (!ctx->count) {
        label = smart_home_lvgl_label_create(ctx->body, "此设备暂无可用控制能力",
                                             SMART_HOME_UI_COLOR_TEXT_MUTED, 17);
        lv_obj_set_pos(label, 4, 24);
    }
    ctx->timer = lv_timer_create(poll, 200, ctx);
    poll(ctx->timer);
}
