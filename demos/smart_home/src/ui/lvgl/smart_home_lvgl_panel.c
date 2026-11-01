/**
 * smart_home LVGL panel screen.
 */

#include "smart_home_lvgl_internal.h"
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
#include "../../miloco/smart_home_miloco.h"
#endif
#include "images/smart_home_icons.h"
#include "../../smart_home_memory.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
#include "../../addons/smart_home_node_gateway.h"
#include "cagent_addons/limits.h"
#endif

enum {
    ENV_TEMP = 0,
    ENV_HUM,
    ENV_LIGHT,
};

static void rebuild_device_cards(smart_home_lvgl_t *ui);
static void update_popup_value(smart_home_lvgl_t *ui);
static void open_device_editor(smart_home_lvgl_t *ui,
                               const smart_home_device_t *device);
static void device_switch_refresh_cb(void *data);

#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
static void remote_node_timer_cb(lv_timer_t *timer);
#endif

static void layout_device_keyboard(smart_home_lvgl_t *ui, int visible)
{
    if (!ui || !ui->device_keyboard) {
        return;
    }

    lv_obj_set_size(ui->device_keyboard,
                    smart_home_lvgl_content_w(),
                    smart_home_lvgl_keyboard_h());
    lv_obj_align(ui->device_keyboard,
                 LV_ALIGN_BOTTOM_MID,
                 0,
                 -SMART_HOME_NAV_H - SMART_HOME_NAV_BOTTOM_PAD - 8);

    if (visible) {
        lv_obj_clear_flag(ui->device_keyboard, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->device_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void device_name_input_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_current_target(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (!ui) {
        return;
    }

    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        if (ui->device_keyboard && target) {
            lv_keyboard_set_textarea(ui->device_keyboard, target);
        }
        layout_device_keyboard(ui, 1);
    } else if (code == LV_EVENT_CANCEL || code == LV_EVENT_READY ||
               code == LV_EVENT_DEFOCUSED) {
        layout_device_keyboard(ui, 0);
    }
}

static void device_keyboard_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_CANCEL || code == LV_EVENT_READY) {
        layout_device_keyboard(ui, 0);
    }
}

#ifndef CONFIG_SMART_HOME_MILOCO_BRIDGE
static const char *device_icon(const smart_home_device_t *device)
{
    if (!device) {
        return ICON_DEVICE_GENERIC;
    }

    return device->type == SMART_HOME_DEVICE_AC ?
        ICON_DEVICE_AC : ICON_DEVICE_LIGHT;
}
#endif

static const char *device_room_name(const char *room)
{
    if (room && strcmp(room, "living_room") == 0) {
        return "客厅";
    }
    if (room && strcmp(room, "bedroom") == 0) {
        return "卧室";
    }
    if (room && strcmp(room, "kitchen") == 0) {
        return "厨房";
    }
    if (room && strcmp(room, "bathroom") == 0) {
        return "卫生间";
    }
    if (room && strcmp(room, "study") == 0) {
        return "书房";
    }
    if (room && strcmp(room, "balcony") == 0) {
        return "阳台";
    }
    return room && room[0] ? room : "家庭";
}

static void room_dropdown_options(const smart_home_state_t *state,
                                  char *buffer, size_t buffer_size,
                                  int include_all)
{
    size_t used = 0u;
    int i;

    if (!buffer || buffer_size == 0u) {
        return;
    }
    buffer[0] = '\0';
    if (include_all) {
        snprintf(buffer, buffer_size, "全部");
        used = strlen(buffer);
    }
    for (i = 0; state && i < smart_home_room_count(state); i++) {
        const char *room = smart_home_room_get(state, i);
        int written;

        written = snprintf(buffer + used, buffer_size - used,
                           "%s%s", used ? "\n" : "", device_room_name(room));
        if (written < 0 || (size_t)written >= buffer_size - used) {
            break;
        }
        used += (size_t)written;
    }
}

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
#define MILOCO_ROOM_OPTIONS_SIZE (32 * SMART_HOME_MILOCO_MAX_DEVICES + 32)

static const char *miloco_room_name(const char *room)
{
    return room && room[0] ? device_room_name(room) : "未分配房间";
}

/* 房间来自真实设备快照；同名房间只列一次，不混入本地演示房间。 */
static void miloco_room_options(smart_home_lvgl_t *ui, char *buffer, size_t size)
{
    smart_home_miloco_device_t devices[SMART_HOME_MILOCO_MAX_DEVICES];
    const char *rooms[SMART_HOME_MILOCO_MAX_DEVICES];
    size_t count = 0, room_count = 0, used;

    snprintf(buffer, size, "全部");
    used = strlen(buffer);
    if (ui->app && ui->app->miloco) {
        count = smart_home_miloco_list(ui->app->miloco, devices,
                                      SMART_HOME_MILOCO_MAX_DEVICES, NULL);
    }
    for (size_t i = 0; i < count; i++) {
        const char *room = miloco_room_name(devices[i].room);
        size_t j;
        for (j = 0; j < room_count; j++) {
            if (!strcmp(rooms[j], room)) {
                break;
            }
        }
        if (j != room_count) {
            continue;
        }
        int n = snprintf(buffer + used, size - used, "\n%s", room);
        if (n < 0 || (size_t)n >= size - used) {
            break;
        }
        used += (size_t)n;
        rooms[room_count++] = room;
    }
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
    if (ui->app && ui->app->node_gateway) {
        caddons_node_info_t nodes[CAGENT_ADDONS_MAX_NODES];
        size_t node_count = smart_home_node_gateway_list(ui->app->node_gateway,
            nodes, CAGENT_ADDONS_MAX_NODES, NULL);
        int has_unassigned = 0;
        for (size_t i = 0; i < room_count; i++)
            if (!strcmp(rooms[i], miloco_room_name(""))) has_unassigned = 1;
        if (node_count && !has_unassigned)
            snprintf(buffer + used, size - used, "\n%s", miloco_room_name(""));
    }
#endif
}

static int miloco_matches_room(smart_home_lvgl_t *ui, const char *room)
{
    char selected[32];
    if (!ui->panel_room_dd || !lv_dropdown_get_selected(ui->panel_room_dd)) {
        return 1;
    }
    lv_dropdown_get_selected_str(ui->panel_room_dd, selected, sizeof(selected));
    return !strcmp(selected, miloco_room_name(room));
}

/* 按房间名称保留选择，避免设备发现顺序变化后筛错房间。 */
static int miloco_sync_room_options(smart_home_lvgl_t *ui)
{
    char options[MILOCO_ROOM_OPTIONS_SIZE];
    char selected[32];
    uint32_t index = 0;
    miloco_room_options(ui, options, sizeof(options));
    if (!strcmp(options, lv_dropdown_get_options(ui->panel_room_dd))) {
        return 0;
    }
    lv_dropdown_get_selected_str(ui->panel_room_dd, selected, sizeof(selected));
    lv_dropdown_set_options(ui->panel_room_dd, options);
    const char *start = options;
    while (start && *start) {
        const char *end = strchr(start, '\n');
        size_t len = end ? (size_t)(end - start) : strlen(start);
        if (strlen(selected) == len && !strncmp(start, selected, len)) {
            lv_dropdown_set_selected(ui->panel_room_dd, index);
            break;
        }
        start = end ? end + 1 : NULL;
        index++;
    }
    return 1;
}
#endif

static const char *device_display_name(const smart_home_device_t *device)
{
    if (!device || !device->name[0]) {
        return "未命名设备";
    }
    /* Keep the seeded demo readable in the Chinese product shell while real
     * devices continue to use their service-provided names unchanged. */
    if (!strcmp(device->name, "Living Light")) {
        return "客厅主灯";
    }
    if (!strcmp(device->name, "Bedroom Light")) {
        return "床头灯";
    }
    if (!strcmp(device->name, "Bedroom AC")) {
        return "卧室空调";
    }
    return device->name;
}

#ifndef CONFIG_SMART_HOME_MILOCO_BRIDGE
static int device_matches_filter(smart_home_lvgl_t *ui,
                                 const smart_home_device_t *device)
{
    int filter;

    if (!ui || !ui->panel_room_dd || !device) {
        return 1;
    }

    filter = lv_dropdown_get_selected(ui->panel_room_dd);
    if (filter <= 0) {
        return 1;
    }
    {
        const char *room = smart_home_room_get(ui->device_state, filter - 1);

        return room && strcmp(device->room, room) == 0;
    }
}
#endif

static const char *device_ac_mode_text(int mode)
{
    static const char *const modes[] = {"制冷", "制热", "除湿", "送风", "自动"};
    return mode >= 0 && mode < (int)(sizeof(modes) / sizeof(modes[0])) ?
        modes[mode] : "未知模式";
}

#ifndef CONFIG_SMART_HOME_MILOCO_BRIDGE
static void format_device_card_text(const smart_home_device_t *device,
                                    char *buffer,
                                    size_t buffer_size)
{
    if (!device || !buffer || buffer_size == 0u) {
        return;
    }

    if (device->type == SMART_HOME_DEVICE_AC) {
        if (device->on) {
            snprintf(buffer,
                     buffer_size,
                     "%s · %d°C · %s",
                     device_room_name(device->room), device->temperature,
                     device_ac_mode_text(device->ac_mode));
        } else {
            snprintf(buffer, buffer_size, "%s · 已关闭",
                     device_room_name(device->room));
        }
    } else if (device->on) {
        snprintf(buffer,
                 buffer_size,
                 "%s · 亮度 %d%%",
                 device_room_name(device->room),
                 device->brightness);
    } else {
        snprintf(buffer, buffer_size, "%s · 已关闭",
                 device_room_name(device->room));
    }
}
#endif

static int device_card_width(void)
{
    int columns = smart_home_lvgl_compact() ? 2 : 4;
    int gap = smart_home_lvgl_compact() ? 8 : 12;

    return (smart_home_lvgl_content_w() - gap * (columns - 1)) / columns;
}

static void device_card_switch_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    lv_obj_t *sw = lv_event_get_current_target(event);
    smart_home_device_t *device;
    int slot;
    int value;
    int mode;
    int fan_speed;
    int on;

    if (!ui || !sw) {
        return;
    }

    /* A switch is an immediate control, never a request to open the card's
     * fine-control sheet.  Stop the event before it can reach the card. */
    lv_event_stop_bubbling(event);
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || !ui->app ||
        !ui->device_state) {
        return;
    }

    slot = (int)(intptr_t)lv_obj_get_user_data(sw);
    device = smart_home_device_get_by_slot(ui->device_state, slot);
    if (!device) {
        return;
    }

    on = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    value = device->type == SMART_HOME_DEVICE_AC ? device->temperature :
                                                   device->brightness;
    if (on && device->type != SMART_HOME_DEVICE_AC && value == 0) {
        /* The current state model stores an off light as brightness zero.
         * Give a fast re-enable a useful default rather than an invisible
         * "on at 0%" state. */
        value = 70;
    }
    mode = device->type == SMART_HOME_DEVICE_AC ? device->ac_mode : 0;
    fan_speed = device->type == SMART_HOME_DEVICE_AC ?
                device->ac_fan_speed : 3;
    if (smart_home_device_service_set_device_control(
            &ui->app->device_service, device->id, on, value, mode,
            fan_speed) != AGENT_OK) {
        if (device->on) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        }
        return;
    }

    /* Rebuilding the grid deletes this switch, therefore postpone it until
     * LVGL has finished dispatching the current value-change event. */
    lv_async_call(device_switch_refresh_cb, ui);
}

#ifndef CONFIG_SMART_HOME_MILOCO_BRIDGE
static void set_card_content(lv_obj_t *card,
                             const char *icon_name,
                             const char *title,
                             const char *detail,
                             int is_on,
                             smart_home_lvgl_t *ui,
                             int slot)
{
    lv_obj_t *badge;
    lv_obj_t *glyph;
    lv_obj_t *label;
    lv_obj_t *sw;

    lv_obj_clean(card);

    /* Horizontal layout: large icon badge on the left, name and status in
     * the middle, state switch on the right. */
    badge = lv_obj_create(card);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 48, 48);
    lv_obj_align(badge, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(badge, 13, 0);
    smart_home_lvgl_set_bg(badge, is_on ? lv_color_hex(0xFFF8EF) :
                           SMART_HOME_UI_COLOR_SURFACE_SOFT);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    glyph = smart_home_lvgl_icon_create(badge, icon_name, 32, 32);
    if (glyph) {
        lv_obj_set_style_text_color(glyph, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_image_recolor(glyph, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
        lv_obj_center(glyph);
    }

    if (ui && slot >= 0) {
        sw = lv_switch_create(card);
        smart_home_lvgl_press_feedback(sw);
        lv_obj_set_size(sw, 40, 22);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
        if (is_on) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        lv_obj_set_user_data(sw, (void *)(intptr_t)slot);
        lv_obj_add_event_cb(sw, device_card_switch_cb, LV_EVENT_ALL, ui);
    }

    label = smart_home_lvgl_label_create(card, title,
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 16);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 62, -12);
    label = smart_home_lvgl_label_create(card, detail,
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY, 12);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 62, 14);
}
#endif

static lv_obj_t *create_action_button(lv_obj_t *parent,
                                      const char *text,
                                      lv_color_t bg,
                                      lv_color_t fg)
{
    lv_obj_t *btn;
    lv_obj_t *label;

    btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn,
                    smart_home_lvgl_compact() ? 62 : 78,
                    smart_home_lvgl_compact() ? 30 : 34);
    lv_obj_set_style_radius(btn, 8, 0);
    smart_home_lvgl_set_bg(btn, bg);

    label = smart_home_lvgl_label_create(btn,
                                         text,
                                         fg,
                                         smart_home_lvgl_compact() ? 10 : 12);
    lv_obj_center(label);
    return btn;
}

static void ctrl_cancel_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    if (ui && ui->ctrl_popup) {
        lv_obj_add_flag(ui->ctrl_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ctrl_toggle_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    if (!ui) {
        return;
    }

    ui->ctrl_pending_on = !ui->ctrl_pending_on;
    smart_home_lvgl_set_bg(ui->ctrl_switch,
                           ui->ctrl_pending_on ?
                               SMART_HOME_UI_COLOR_PRIMARY :
                               SMART_HOME_UI_COLOR_SURFACE_SOFT);
}

static void ctrl_slider_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    if (!ui || !ui->ctrl_slider || !ui->ctrl_value_label) {
        return;
    }

    ui->ctrl_pending_val = lv_slider_get_value(ui->ctrl_slider);
    update_popup_value(ui);
}

static void ctrl_mode_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    if (!ui || !ui->ctrl_mode_dd) {
        return;
    }

    ui->ctrl_pending_mode = lv_dropdown_get_selected(ui->ctrl_mode_dd);
    if (ui->ctrl_slider) {
        if (ui->ctrl_pending_mode == 2 || ui->ctrl_pending_mode == 3) {
            lv_obj_add_flag(ui->ctrl_slider, LV_OBJ_FLAG_HIDDEN);
            if (ui->ctrl_value_label) {
                lv_label_set_text(ui->ctrl_value_label, "当前模式无需设定温度");
            }
        } else {
            lv_obj_clear_flag(ui->ctrl_slider, LV_OBJ_FLAG_HIDDEN);
            update_popup_value(ui);
        }
    }
}

static void ctrl_fan_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    if (!ui || !ui->ctrl_fan_dd) {
        return;
    }

    ui->ctrl_pending_fan_speed = lv_dropdown_get_selected(ui->ctrl_fan_dd);
}

static void ctrl_confirm_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    smart_home_device_t *device;
    const char *room;

    if (!ui || !ui->app || !ui->device_state || !ui->ctrl_room_dd) {
        return;
    }

    device = smart_home_device_find_by_id(ui->device_state, ui->ctrl_device_id);
    ui->ctrl_pending_room_index = lv_dropdown_get_selected(ui->ctrl_room_dd);
    room = smart_home_room_get(ui->device_state, ui->ctrl_pending_room_index);
    if (!device || !room) {
        return;
    }
    if (strcmp(device->room, room) != 0 &&
        smart_home_device_service_update_meta(&ui->app->device_service,
                                              device->id, room,
                                              device->name) != AGENT_OK) {
        return;
    }

    if (smart_home_device_service_set_device_control(
            &ui->app->device_service, ui->ctrl_device_id,
            ui->ctrl_pending_on, ui->ctrl_pending_val,
            ui->ctrl_pending_mode, ui->ctrl_pending_fan_speed) != AGENT_OK) {
        return;
    }

    smart_home_lvgl_refresh_cards(ui);
    if (ui->ctrl_popup) {
        lv_obj_add_flag(ui->ctrl_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ctrl_edit_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    smart_home_device_t *device;

    if (!ui || !ui->app) {
        return;
    }

    device = smart_home_device_find_by_id(ui->device_state, ui->ctrl_device_id);
    if (!device) {
        return;
    }

    if (ui->ctrl_popup) {
        lv_obj_add_flag(ui->ctrl_popup, LV_OBJ_FLAG_HIDDEN);
    }
    open_device_editor(ui, device);
}

static void ctrl_delete_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    if (!ui || !ui->app || !ui->device_state) {
        return;
    }

    smart_home_device_service_remove(&ui->app->device_service,
                                     ui->ctrl_device_id);
    if (ui->ctrl_popup) {
        lv_obj_add_flag(ui->ctrl_popup, LV_OBJ_FLAG_HIDDEN);
    }
    smart_home_lvgl_refresh_cards(ui);
}

static void env_slider_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    char text[32];
    int value;

    if (!ui || !ui->env_slider || !ui->env_value_label) {
        return;
    }

    value = lv_slider_get_value(ui->env_slider);
    if (ui->env_pending_type == ENV_LIGHT) {
        value = (value / 10) * 10;
        lv_slider_set_value(ui->env_slider, value, LV_ANIM_OFF);
    }
    ui->env_pending_value = value;

    if (ui->env_pending_type == ENV_TEMP) {
        snprintf(text, sizeof(text), "%d°C", value);
    } else if (ui->env_pending_type == ENV_HUM) {
        snprintf(text, sizeof(text), "%d%%", value);
    } else {
        snprintf(text, sizeof(text), "%dlx", value);
    }
    lv_label_set_text(ui->env_value_label, text);
}

static void env_cancel_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    if (ui && ui->env_popup) {
        lv_obj_add_flag(ui->env_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void env_confirm_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    smart_home_state_t *state;
    int temperature;
    int humidity;
    int ambient_light;

    if (!ui || !ui->app || !ui->device_state) {
        return;
    }

    state = ui->device_state;
    temperature = state->env_temperature;
    humidity = state->env_humidity;
    ambient_light = state->env_light;

    if (ui->env_pending_type == ENV_TEMP) {
        temperature = ui->env_pending_value;
    } else if (ui->env_pending_type == ENV_HUM) {
        humidity = ui->env_pending_value;
    } else {
        ambient_light = ui->env_pending_value;
    }

    smart_home_device_service_set_environment(&ui->app->device_service,
                                              temperature,
                                              humidity,
                                              ambient_light);
    smart_home_lvgl_refresh_cards(ui);
    if (ui->env_popup) {
        lv_obj_add_flag(ui->env_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void device_editor_cancel_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    if (ui && ui->device_popup) {
        layout_device_keyboard(ui, 0);
        lv_obj_add_flag(ui->device_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void device_editor_confirm_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    const char *name;
    const char *room;
    int ret;

    if (!ui || !ui->app || !ui->device_state || !ui->device_name_input ||
        !ui->device_room_dd) {
        return;
    }

    name = lv_textarea_get_text(ui->device_name_input);
    room = smart_home_room_get(ui->device_state,
                               lv_dropdown_get_selected(ui->device_room_dd));
    if (!room) {
        return;
    }

    if (ui->device_edit_id == 0) {
        smart_home_device_type_t type;

        if (!ui->device_type_dd) {
            return;
        }
        type = smart_home_device_type_from_index(lv_dropdown_get_selected(
            ui->device_type_dd));
        ret = smart_home_device_service_add(&ui->app->device_service,
                                            room, name, type, NULL);
    } else {
        ret = smart_home_device_service_update_meta(&ui->app->device_service,
                                                    ui->device_edit_id,
                                                    room,
                                                    name);
    }

    if (ret != AGENT_OK) {
        lv_label_set_text(ui->device_popup_title, "保存失败，请检查设备名称和房间");
        return;
    }

    layout_device_keyboard(ui, 0);
    lv_obj_add_flag(ui->device_popup, LV_OBJ_FLAG_HIDDEN);
    smart_home_lvgl_refresh_cards(ui);
}

/* 来源筛选：0=全部 1=米家 2=Node。 */
static void panel_source_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);

    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || !ui ||
        !ui->panel_source_dd) {
        return;
    }
    smart_home_lvgl_refresh_cards(ui);
}

/* 判断设备是否命中当前来源筛选。 */
static int device_matches_source(smart_home_lvgl_t *ui, int source)
{
    int selected;

    if (!ui || !ui->panel_source_dd) {
        return 1;
    }
    selected = lv_dropdown_get_selected(ui->panel_source_dd);
    if (selected == 0) {
        return 1;
    }
    return selected == source;
}

static void panel_room_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    rebuild_device_cards(ui);
}

static void room_chip_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    lv_obj_t *chip = lv_event_get_current_target(event);
    lv_obj_t *row;
    int selected;
    uint32_t i;

    if (!ui || !ui->panel_room_dd || !chip ||
        lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    selected = (int)(intptr_t)lv_obj_get_user_data(chip);
    lv_dropdown_set_selected(ui->panel_room_dd, selected);
    row = lv_obj_get_parent(chip);
    for (i = 0; row && i < lv_obj_get_child_count(row); i++) {
        lv_obj_t *item = lv_obj_get_child(row, i);
        lv_obj_t *label;
        int active = (int)(intptr_t)lv_obj_get_user_data(item) == selected;

        smart_home_lvgl_set_bg(item, active ? SMART_HOME_UI_COLOR_SURFACE_ON :
                               SMART_HOME_UI_COLOR_SURFACE);
        label = lv_obj_get_child(item, 0);
        if (label) {
            lv_obj_set_style_text_color(label,
                                        active ? SMART_HOME_UI_COLOR_PRIMARY_DARK :
                                        SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                        0);
        }
    }
    rebuild_device_cards(ui);
}

static void create_room_chip(lv_obj_t *row, smart_home_lvgl_t *ui,
                             const char *text, int index)
{
    lv_obj_t *chip = lv_btn_create(row);
    lv_obj_t *label;
    int selected = ui && ui->panel_room_dd &&
                   lv_dropdown_get_selected(ui->panel_room_dd) == index;

    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, smart_home_lvgl_compact() ? 62 : 82, 34);
    lv_obj_set_style_radius(chip, 12, 0);
    smart_home_lvgl_set_bg(chip, selected ? SMART_HOME_UI_COLOR_SURFACE_ON :
                           SMART_HOME_UI_COLOR_SURFACE);
    lv_obj_set_user_data(chip, (void *)(intptr_t)index);
    smart_home_lvgl_press_feedback(chip);
    lv_obj_add_event_cb(chip, room_chip_cb, LV_EVENT_CLICKED, ui);
    label = smart_home_lvgl_label_create(chip, text,
                                         selected ? SMART_HOME_UI_COLOR_PRIMARY_DARK :
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY, 14);
    lv_obj_center(label);
}

static void room_popup_cancel_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    if (ui && ui->room_popup) {
        lv_obj_add_flag(ui->room_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void rebuild_room_filters(smart_home_lvgl_t *ui);

static void room_popup_confirm_cb(lv_event_t *event)
{
    static const char *const presets[] = {
        "kitchen", "bathroom", "study", "balcony",
    };
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    int selected;
    int ret;

    if (!ui || !ui->app || !ui->room_preset_dd) {
        return;
    }
    selected = lv_dropdown_get_selected(ui->room_preset_dd);
    if (selected < 0 || selected >= (int)(sizeof(presets) / sizeof(presets[0]))) {
        return;
    }
    ret = smart_home_device_service_add_room(&ui->app->device_service,
                                             presets[selected]);
    if (ret != AGENT_OK) {
        if (ui->room_popup_title) {
            lv_label_set_text(ui->room_popup_title, "新增房间失败或数量已满");
        }
        return;
    }
    {
        char rooms[128];

        room_dropdown_options(ui->device_state, rooms, sizeof(rooms), 1);
        lv_dropdown_set_options(ui->panel_room_dd, rooms);
        lv_dropdown_set_selected(ui->panel_room_dd, 0);
    }
    rebuild_room_filters(ui);
    rebuild_device_cards(ui);
    if (ui->room_popup) {
        lv_obj_add_flag(ui->room_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void open_room_creator(smart_home_lvgl_t *ui)
{
    lv_obj_t *popup;
    lv_obj_t *btn;
    int compact = smart_home_lvgl_compact();

    if (!ui || !ui->screen_panel) {
        return;
    }
    if (!ui->room_popup) {
        popup = lv_obj_create(ui->screen_panel);
        lv_obj_remove_style_all(popup);
        lv_obj_set_size(popup, compact ? 240 : 320, compact ? 154 : 184);
        lv_obj_align(popup, LV_ALIGN_CENTER, 0, 0);
        smart_home_lvgl_card_style(popup);
        smart_home_lvgl_set_bg(popup, SMART_HOME_UI_COLOR_SURFACE);
        ui->room_popup = popup;

        ui->room_popup_title = smart_home_lvgl_label_create(
            popup, "新增房间", SMART_HOME_UI_COLOR_TEXT_PRIMARY,
            compact ? 14 : 16);
        lv_obj_align(ui->room_popup_title, LV_ALIGN_TOP_MID, 0,
                     compact ? 12 : 16);

        ui->room_preset_dd = lv_dropdown_create(popup);
        smart_home_lvgl_press_feedback(ui->room_preset_dd);
    lv_obj_set_style_text_font(ui->room_preset_dd, smart_home_lvgl_font(13), 0);
        lv_dropdown_set_options(ui->room_preset_dd, "厨房\n卫生间\n书房\n阳台");
        lv_obj_set_size(ui->room_preset_dd, lv_pct(74), compact ? 32 : 36);
        lv_obj_align(ui->room_preset_dd, LV_ALIGN_TOP_MID, 0,
                     compact ? 48 : 62);
        lv_obj_set_style_text_font(ui->room_preset_dd,
                                   smart_home_lvgl_font(12), 0);

        btn = create_action_button(popup, "取消",
                                   SMART_HOME_UI_COLOR_BTN_SECONDARY,
                                   SMART_HOME_UI_COLOR_TEXT_PRIMARY);
        lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, compact ? 22 : 30,
                     compact ? -10 : -16);
        smart_home_lvgl_press_feedback(btn);
        lv_obj_add_event_cb(btn, room_popup_cancel_cb, LV_EVENT_CLICKED, ui);
        btn = create_action_button(popup, "创建",
                                   SMART_HOME_UI_COLOR_PRIMARY, lv_color_white());
        lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, compact ? -22 : -30,
                     compact ? -10 : -16);
        smart_home_lvgl_press_feedback(btn);
        lv_obj_add_event_cb(btn, room_popup_confirm_cb, LV_EVENT_CLICKED, ui);
    }
    lv_label_set_text(ui->room_popup_title, "新增房间");
    lv_obj_clear_flag(ui->room_popup, LV_OBJ_FLAG_HIDDEN);
}

static void add_room_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    open_room_creator(ui);
}

static void rebuild_room_filters(smart_home_lvgl_t *ui)
{
    int i;

    if (!ui || !ui->panel_filter_row || !ui->panel_room_dd) {
        return;
    }
    lv_obj_clean(ui->panel_filter_row);
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    const char *start = lv_dropdown_get_options(ui->panel_room_dd);
    for (i = 0; start && *start; i++) {
        const char *end = strchr(start, '\n');
        char room[32];
        size_t len = end ? (size_t)(end - start) : strlen(start);
        snprintf(room, sizeof(room), "%.*s", (int)len, start);
        create_room_chip(ui->panel_filter_row, ui, room, i);
        start = end ? end + 1 : NULL;
    }
#else
    create_room_chip(ui->panel_filter_row, ui, "全部", 0);
    for (i = 0; i < smart_home_room_count(ui->device_state); i++) {
        create_room_chip(ui->panel_filter_row, ui,
                         device_room_name(smart_home_room_get(ui->device_state, i)),
                         i + 1);
    }
    {
        lv_obj_t *btn = lv_btn_create(ui->panel_filter_row);
        lv_obj_t *label;

        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, smart_home_lvgl_compact() ? 72 : 96, 34);
        lv_obj_set_style_radius(btn, 12, 0);
        smart_home_lvgl_set_bg(btn, SMART_HOME_UI_COLOR_SURFACE_SOFT);
        smart_home_lvgl_press_feedback(btn);
        lv_obj_add_event_cb(btn, add_room_cb, LV_EVENT_CLICKED, ui);
        label = smart_home_lvgl_label_create(btn, "+ 新增房间",
                                             SMART_HOME_UI_COLOR_PRIMARY_DARK,
                                             12);
        lv_obj_center(label);
    }
#endif
}

#ifndef CONFIG_SMART_HOME_MILOCO_BRIDGE
static void add_device_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    open_device_editor(ui, NULL);
}
#endif

static void update_popup_value(smart_home_lvgl_t *ui)
{
    smart_home_device_t *device;
    char text[32];

    if (!ui || !ui->ctrl_value_label || !ui->device_state) {
        return;
    }

    device = smart_home_device_find_by_id(ui->device_state, ui->ctrl_device_id);
    snprintf(text,
             sizeof(text),
             device && device->type == SMART_HOME_DEVICE_AC ? "%d°C" : "亮度 %d%%",
             ui->ctrl_pending_val);
    lv_label_set_text(ui->ctrl_value_label, text);
}

static void refresh_sensor_bar(smart_home_lvgl_t *ui)
{
    smart_home_state_t *state;
    smart_home_device_t *ac;
    char text[40];

    if (!ui) {
        return;
    }

    state = ui->device_state;
    if (!state) {
        return;
    }

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    /* 传感器栏优先显示 wttr.in 真实天气。 */
    if (ui->app && ui->app->miloco) {
        smart_home_miloco_weather_t wx;

        if (smart_home_miloco_get_weather(ui->app->miloco, &wx)) {
            if (ui->env_temp_label) {
                snprintf(text, sizeof(text), "室外 %d°C", wx.temperature);
                lv_label_set_text(ui->env_temp_label, text);
            }
            if (ui->env_hum_label) {
                snprintf(text, sizeof(text), "湿度 %d%%", wx.humidity);
                lv_label_set_text(ui->env_hum_label, text);
            }
            if (ui->env_light_label) {
                snprintf(text, sizeof(text), "%s · 风%dkm/h",
                         wx.condition_cn, wx.wind_kmph);
                lv_label_set_text(ui->env_light_label, text);
            }
            return;
        }
    }
#endif
    if (ui->env_temp_label) {
        snprintf(text, sizeof(text), "温度 %d°C", state->env_temperature);
        lv_label_set_text(ui->env_temp_label, text);
    }
    if (ui->env_hum_label) {
        snprintf(text, sizeof(text), "湿度 %d%%", state->env_humidity);
        lv_label_set_text(ui->env_hum_label, text);
    }
    if (ui->env_light_label) {
        snprintf(text, sizeof(text), "光照 %d lx", state->env_light);
        lv_label_set_text(ui->env_light_label, text);
    }
    if (ui->env_ac_label) {
        ac = smart_home_device_find_first(state,
                                          "bedroom",
                                          SMART_HOME_DEVICE_AC);
        if (ac) {
            snprintf(text,
                     sizeof(text),
                     "空调 %s %d°C",
                     ac->on ? device_ac_mode_text(ac->ac_mode) : "已关闭",
                     ac->temperature);
        } else {
            snprintf(text, sizeof(text), "暂无空调");
        }
        lv_label_set_text(ui->env_ac_label, text);
    }
}

void smart_home_lvgl_refresh_cards(smart_home_lvgl_t *ui)
{
    if (!ui) {
        return;
    }

    rebuild_device_cards(ui);
    refresh_sensor_bar(ui);
    smart_home_lvgl_refresh_home(ui);
}

static void device_switch_refresh_cb(void *data)
{
    smart_home_lvgl_refresh_cards((smart_home_lvgl_t *)data);
}

static void ensure_control_popup(smart_home_lvgl_t *ui)
{
    lv_obj_t *popup;
    lv_obj_t *label;
    lv_obj_t *row;
    lv_obj_t *btn;
    int compact = smart_home_lvgl_compact();
    int drawer_w;
    int drawer_h;

    if (!ui || ui->ctrl_popup) {
        return;
    }

    popup = lv_obj_create(ui->screen_panel);
    lv_obj_remove_style_all(popup);
    drawer_w = compact ? smart_home_lvgl_content_w() :
               smart_home_lvgl_disp_w() * 62 / 100;
    drawer_h = smart_home_lvgl_disp_h() - SMART_HOME_TOPBAR_H -
               SMART_HOME_NAV_H - 10;
    lv_obj_set_size(popup, drawer_w, drawer_h);
    lv_obj_set_pos(popup, smart_home_lvgl_disp_w() -
                   smart_home_lvgl_pad_x() - drawer_w,
                   SMART_HOME_TOPBAR_H + 6);
    smart_home_lvgl_card_style(popup);
    smart_home_lvgl_set_bg(popup, SMART_HOME_UI_COLOR_SURFACE);
    ui->ctrl_popup = popup;

    ui->ctrl_title = smart_home_lvgl_label_create(popup,
                                                  "",
                                                  SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                                  compact ? 16 : 22);
    lv_obj_align(ui->ctrl_title, LV_ALIGN_TOP_LEFT,
                 compact ? 16 : 28, compact ? 10 : 18);

    ui->ctrl_switch = lv_btn_create(popup);
    lv_obj_remove_style_all(ui->ctrl_switch);
    lv_obj_set_size(ui->ctrl_switch, compact ? 82 : 96, compact ? 32 : 38);
    lv_obj_align(ui->ctrl_switch, LV_ALIGN_TOP_RIGHT,
                 compact ? -16 : -28, compact ? 10 : 16);
    lv_obj_set_style_radius(ui->ctrl_switch, 8, 0);
    smart_home_lvgl_press_feedback(ui->ctrl_switch);
    lv_obj_add_event_cb(ui->ctrl_switch, ctrl_toggle_cb, LV_EVENT_CLICKED, ui);
    label = smart_home_lvgl_label_create(ui->ctrl_switch,
                                         "开关",
                                         lv_color_white(),
                                         12);
    lv_obj_center(label);

    label = smart_home_lvgl_label_create(popup, "所属房间",
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                         compact ? 11 : 13);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, compact ? 16 : 28,
                 compact ? 52 : 68);
    ui->ctrl_room_dd = lv_dropdown_create(popup);
    smart_home_lvgl_press_feedback(ui->ctrl_room_dd);
    lv_obj_set_style_text_font(ui->ctrl_room_dd, smart_home_lvgl_font(13), 0);
    lv_obj_set_size(ui->ctrl_room_dd, compact ? 112 : 142,
                    compact ? 30 : 34);
    lv_obj_align(ui->ctrl_room_dd, LV_ALIGN_TOP_LEFT,
                 compact ? 76 : 98, compact ? 46 : 60);
    lv_obj_set_style_text_font(ui->ctrl_room_dd, smart_home_lvgl_font(12), 0);

    ui->ctrl_value_label = smart_home_lvgl_label_create(popup,
                                                        "",
                                                        SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                                        compact ? 16 : 22);
    lv_obj_align(ui->ctrl_value_label,
                 LV_ALIGN_TOP_LEFT,
                 compact ? 16 : 28,
                 compact ? 90 : 114);

    ui->ctrl_mode_dd = lv_dropdown_create(popup);
    smart_home_lvgl_press_feedback(ui->ctrl_mode_dd);
    lv_obj_set_style_text_font(ui->ctrl_mode_dd, smart_home_lvgl_font(13), 0);
    lv_dropdown_set_options(ui->ctrl_mode_dd, "制冷\n制热\n除湿\n送风\n自动");
    lv_obj_set_size(ui->ctrl_mode_dd, compact ? 104 : 116, 32);
    lv_obj_align(ui->ctrl_mode_dd,
                 LV_ALIGN_TOP_LEFT,
                 compact ? 16 : 28,
                 compact ? 126 : 164);
    lv_obj_set_style_text_font(ui->ctrl_mode_dd, smart_home_lvgl_font(12), 0);
    lv_obj_add_event_cb(ui->ctrl_mode_dd,
                        ctrl_mode_cb,
                        LV_EVENT_VALUE_CHANGED,
                        ui);

    ui->ctrl_fan_dd = lv_dropdown_create(popup);
    smart_home_lvgl_press_feedback(ui->ctrl_fan_dd);
    lv_obj_set_style_text_font(ui->ctrl_fan_dd, smart_home_lvgl_font(13), 0);
    lv_dropdown_set_options(ui->ctrl_fan_dd, "低速\n中速\n高速\n自动");
    lv_obj_set_size(ui->ctrl_fan_dd, compact ? 104 : 116, 32);
    lv_obj_align(ui->ctrl_fan_dd,
                 LV_ALIGN_TOP_RIGHT,
                 compact ? -16 : -28,
                 compact ? 126 : 164);
    lv_obj_set_style_text_font(ui->ctrl_fan_dd, smart_home_lvgl_font(12), 0);
    lv_obj_add_event_cb(ui->ctrl_fan_dd,
                        ctrl_fan_cb,
                        LV_EVENT_VALUE_CHANGED,
                        ui);

    ui->ctrl_slider = lv_slider_create(popup);
    lv_obj_set_size(ui->ctrl_slider, lv_pct(72), 8);
    lv_obj_align(ui->ctrl_slider, LV_ALIGN_TOP_MID, 0, compact ? 182 : 226);
    lv_obj_add_event_cb(ui->ctrl_slider,
                        ctrl_slider_cb,
                        LV_EVENT_VALUE_CHANGED,
                        ui);

    row = lv_obj_create(popup);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row,
                    drawer_w - (compact ? 18 : 36),
                    compact ? 32 : 38);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, compact ? -10 : -18);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);

    btn = create_action_button(row,
                               "删除",
                               SMART_HOME_UI_COLOR_DANGER,
                               lv_color_white());
    smart_home_lvgl_press_feedback(btn);
    lv_obj_add_event_cb(btn, ctrl_delete_cb, LV_EVENT_CLICKED, ui);

    btn = create_action_button(row,
                               "关闭",
                               SMART_HOME_UI_COLOR_BTN_SECONDARY,
                               SMART_HOME_UI_COLOR_TEXT_PRIMARY);
    smart_home_lvgl_press_feedback(btn);
    lv_obj_add_event_cb(btn, ctrl_cancel_cb, LV_EVENT_CLICKED, ui);

    btn = create_action_button(row,
                               "重命名",
                               SMART_HOME_UI_COLOR_SURFACE_SOFT,
                               SMART_HOME_UI_COLOR_TEXT_PRIMARY);
    smart_home_lvgl_press_feedback(btn);
    lv_obj_add_event_cb(btn, ctrl_edit_cb, LV_EVENT_CLICKED, ui);

    btn = create_action_button(row,
                               "保存",
                               SMART_HOME_UI_COLOR_PRIMARY,
                               lv_color_white());
    smart_home_lvgl_press_feedback(btn);
    lv_obj_add_event_cb(btn, ctrl_confirm_cb, LV_EVENT_CLICKED, ui);
}

#ifndef CONFIG_SMART_HOME_MILOCO_BRIDGE
static void card_click_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    int slot =
        (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(event));
    smart_home_device_t *device;
    int is_ac;
    char rooms[128];

    if (!ui || !ui->device_state) {
        return;
    }

    device = smart_home_device_get_by_slot(ui->device_state, slot);
    if (!device) {
        return;
    }

    is_ac = device->type == SMART_HOME_DEVICE_AC;
    ui->ctrl_device_id = device->id;
    ui->ctrl_pending_on = device->on;
    ui->ctrl_pending_val = is_ac ? device->temperature : device->brightness;
    ui->ctrl_pending_mode = is_ac ? device->ac_mode : 0;
    ui->ctrl_pending_fan_speed = is_ac ? device->ac_fan_speed : 3;

    ensure_control_popup(ui);
    if (!ui->ctrl_popup) {
        return;
    }

    lv_label_set_text(ui->ctrl_title, device_display_name(device));
    room_dropdown_options(ui->device_state, rooms, sizeof(rooms), 0);
    lv_dropdown_set_options(ui->ctrl_room_dd, rooms);
    ui->ctrl_pending_room_index = smart_home_room_index(ui->device_state,
                                                         device->room);
    if (ui->ctrl_pending_room_index < 0) {
        ui->ctrl_pending_room_index = 0;
    }
    lv_dropdown_set_selected(ui->ctrl_room_dd, ui->ctrl_pending_room_index);
    smart_home_lvgl_set_bg(ui->ctrl_switch,
                           device->on ? SMART_HOME_UI_COLOR_PRIMARY :
                                        SMART_HOME_UI_COLOR_SURFACE_SOFT);
    lv_slider_set_range(ui->ctrl_slider, is_ac ? 16 : 0, is_ac ? 30 : 100);
    lv_slider_set_value(ui->ctrl_slider, ui->ctrl_pending_val, LV_ANIM_OFF);

    if (is_ac) {
        lv_dropdown_set_selected(ui->ctrl_mode_dd, ui->ctrl_pending_mode);
        lv_dropdown_set_selected(ui->ctrl_fan_dd, ui->ctrl_pending_fan_speed);
        lv_obj_clear_flag(ui->ctrl_mode_dd, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui->ctrl_fan_dd, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->ctrl_mode_dd, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui->ctrl_fan_dd, LV_OBJ_FLAG_HIDDEN);
    }

    if (is_ac && (ui->ctrl_pending_mode == 2 || ui->ctrl_pending_mode == 3)) {
        lv_obj_add_flag(ui->ctrl_slider, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ui->ctrl_value_label, "当前模式无需设定温度");
    } else {
        lv_obj_clear_flag(ui->ctrl_slider, LV_OBJ_FLAG_HIDDEN);
        update_popup_value(ui);
    }
    lv_obj_clear_flag(ui->ctrl_popup, LV_OBJ_FLAG_HIDDEN);
}
#endif

static void env_click_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    int type =
        (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(event));
    smart_home_state_t *state;
    int min = 0;
    int max = 100;
    const char *title = "环境";

    if (!ui || !ui->device_state) {
        return;
    }

    state = ui->device_state;
    ui->env_pending_type = type;
    if (type == ENV_TEMP) {
        title = "温度";
        max = 45;
        ui->env_pending_value = state->env_temperature;
    } else if (type == ENV_HUM) {
        title = "湿度";
        max = 100;
        ui->env_pending_value = state->env_humidity;
    } else {
        title = "光照";
        max = 1000;
        ui->env_pending_value = state->env_light;
    }

    if (!ui->env_popup) {
        lv_obj_t *popup;
        lv_obj_t *btn;
        lv_obj_t *label;
        int compact = smart_home_lvgl_compact();

        popup = lv_obj_create(ui->screen_panel);
        lv_obj_remove_style_all(popup);
        lv_obj_set_size(popup, smart_home_lvgl_content_w(), compact ? 188 : 216);
        lv_obj_align(popup, LV_ALIGN_CENTER, 0, 0);
        smart_home_lvgl_card_style(popup);
        smart_home_lvgl_set_bg(popup, SMART_HOME_UI_COLOR_SURFACE);
        ui->env_popup = popup;

        ui->env_title = smart_home_lvgl_label_create(popup,
                                                     "",
                                                     SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                                     compact ? 14 : 16);
        lv_obj_align(ui->env_title, LV_ALIGN_TOP_MID, 0, compact ? 12 : 16);

        ui->env_value_label = smart_home_lvgl_label_create(popup,
                                                           "",
                                                           SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                                           compact ? 14 : 16);
        lv_obj_align(ui->env_value_label,
                     LV_ALIGN_TOP_MID,
                     0,
                     compact ? 48 : 58);

        ui->env_slider = lv_slider_create(popup);
        lv_obj_set_size(ui->env_slider, lv_pct(78), 8);
        lv_obj_align(ui->env_slider, LV_ALIGN_TOP_MID, 0, compact ? 86 : 102);
        lv_obj_add_event_cb(ui->env_slider,
                            env_slider_cb,
                            LV_EVENT_VALUE_CHANGED,
                            ui);

        btn = create_action_button(popup,
                                   "取消",
                                   SMART_HOME_UI_COLOR_BTN_SECONDARY,
                                   SMART_HOME_UI_COLOR_TEXT_PRIMARY);
        lv_obj_align(btn,
                     LV_ALIGN_BOTTOM_LEFT,
                     compact ? 24 : 30,
                     compact ? -10 : -18);
        smart_home_lvgl_press_feedback(btn);
        lv_obj_add_event_cb(btn, env_cancel_cb, LV_EVENT_CLICKED, ui);

        btn = create_action_button(popup,
                                   "应用",
                                   SMART_HOME_UI_COLOR_PRIMARY,
                                   lv_color_white());
        lv_obj_align(btn,
                     LV_ALIGN_BOTTOM_RIGHT,
                     compact ? -24 : -30,
                     compact ? -10 : -18);
        smart_home_lvgl_press_feedback(btn);
        lv_obj_add_event_cb(btn, env_confirm_cb, LV_EVENT_CLICKED, ui);

        (void)label;
    }

    lv_label_set_text(ui->env_title, title);
    lv_slider_set_range(ui->env_slider, min, max);
    lv_slider_set_value(ui->env_slider, ui->env_pending_value, LV_ANIM_OFF);
    env_slider_cb(event);
    lv_obj_clear_flag(ui->env_popup, LV_OBJ_FLAG_HIDDEN);
}

static void open_device_editor(smart_home_lvgl_t *ui,
                               const smart_home_device_t *device)
{
    lv_obj_t *popup;
    lv_obj_t *btn;
    int is_edit = device != NULL;
    int compact = smart_home_lvgl_compact();
    char rooms[128];

    if (!ui || !ui->screen_panel) {
        return;
    }

    if (!ui->device_popup) {
        popup = lv_obj_create(ui->screen_panel);
        lv_obj_remove_style_all(popup);
        lv_obj_set_size(popup, smart_home_lvgl_content_w(), compact ? 218 : 274);
        lv_obj_align(popup, LV_ALIGN_CENTER, 0, 0);
        smart_home_lvgl_card_style(popup);
        smart_home_lvgl_set_bg(popup, SMART_HOME_UI_COLOR_SURFACE);
        ui->device_popup = popup;

        ui->device_popup_title = smart_home_lvgl_label_create(
            popup,
            "",
            SMART_HOME_UI_COLOR_TEXT_PRIMARY,
            compact ? 14 : 16);
        lv_obj_align(ui->device_popup_title,
                     LV_ALIGN_TOP_MID,
                     0,
                     compact ? 8 : 14);

        ui->device_name_input = lv_textarea_create(popup);
        lv_textarea_set_one_line(ui->device_name_input, true);
        lv_textarea_set_max_length(ui->device_name_input,
                                   SMART_HOME_DEVICE_NAME_SIZE - 1);
        lv_obj_set_size(ui->device_name_input,
                        lv_pct(78),
                        compact ? 32 : 36);
        lv_obj_align(ui->device_name_input,
                     LV_ALIGN_TOP_MID,
                     0,
                     compact ? 38 : 52);
        lv_obj_set_style_text_font(ui->device_name_input,
                                   smart_home_lvgl_font(12),
                                   0);
        lv_obj_add_event_cb(ui->device_name_input,
                            device_name_input_cb,
                            LV_EVENT_ALL,
                            ui);

        ui->device_room_dd = lv_dropdown_create(popup);
        smart_home_lvgl_press_feedback(ui->device_room_dd);
    lv_obj_set_style_text_font(ui->device_room_dd, smart_home_lvgl_font(13), 0);
        lv_dropdown_set_options(ui->device_room_dd, "客厅\n卧室");
        lv_obj_set_size(ui->device_room_dd, lv_pct(78), compact ? 32 : 34);
        lv_obj_align(ui->device_room_dd,
                     LV_ALIGN_TOP_MID,
                     0,
                     compact ? 82 : 102);
        lv_obj_set_style_text_font(ui->device_room_dd,
                                   smart_home_lvgl_font(12),
                                   0);

        ui->device_type_dd = lv_dropdown_create(popup);
        smart_home_lvgl_press_feedback(ui->device_type_dd);
    lv_obj_set_style_text_font(ui->device_type_dd, smart_home_lvgl_font(13), 0);
        lv_dropdown_set_options(ui->device_type_dd, "灯光\n空调");
        lv_obj_set_size(ui->device_type_dd, lv_pct(78), compact ? 32 : 34);
        lv_obj_align(ui->device_type_dd,
                     LV_ALIGN_TOP_MID,
                     0,
                     compact ? 126 : 152);
        lv_obj_set_style_text_font(ui->device_type_dd,
                                   smart_home_lvgl_font(12),
                                   0);

        btn = create_action_button(popup,
                                   "取消",
                                   SMART_HOME_UI_COLOR_BTN_SECONDARY,
                                   SMART_HOME_UI_COLOR_TEXT_PRIMARY);
        lv_obj_align(btn,
                     LV_ALIGN_BOTTOM_LEFT,
                     compact ? 24 : 30,
                     compact ? -10 : -18);
        smart_home_lvgl_press_feedback(btn);
        lv_obj_add_event_cb(btn,
                            device_editor_cancel_cb,
                            LV_EVENT_CLICKED,
                            ui);

        btn = create_action_button(popup,
                                   "保存",
                                   SMART_HOME_UI_COLOR_PRIMARY,
                                   lv_color_white());
        lv_obj_align(btn,
                     LV_ALIGN_BOTTOM_RIGHT,
                     compact ? -24 : -30,
                     compact ? -10 : -18);
        smart_home_lvgl_press_feedback(btn);
        lv_obj_add_event_cb(btn,
                            device_editor_confirm_cb,
                            LV_EVENT_CLICKED,
                            ui);

        ui->device_keyboard = lv_keyboard_create(ui->screen_panel);
        smart_home_lvgl_style_keyboard(ui->device_keyboard);
        lv_obj_add_event_cb(ui->device_keyboard,
                            device_keyboard_cb,
                            LV_EVENT_ALL,
                            ui);
        layout_device_keyboard(ui, 0);
    }

    ui->device_edit_id = is_edit ? device->id : 0;
    room_dropdown_options(ui->device_state, rooms, sizeof(rooms), 0);
    lv_dropdown_set_options(ui->device_room_dd, rooms);
    lv_label_set_text(ui->device_popup_title,
                      is_edit ? "编辑设备" : "添加设备");
    lv_textarea_set_text(ui->device_name_input,
                         is_edit ? device->name : "");
    {
        int room_index = is_edit ?
            smart_home_room_index(ui->device_state, device->room) : 0;

        lv_dropdown_set_selected(ui->device_room_dd,
                                 room_index >= 0 ? room_index : 0);
    }
    lv_dropdown_set_selected(
        ui->device_type_dd,
        is_edit ? smart_home_device_type_to_index(device->type) : 0);

    if (is_edit) {
        lv_obj_add_flag(ui->device_type_dd, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(ui->device_type_dd, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(ui->device_popup, LV_OBJ_FLAG_HIDDEN);
}

#ifndef CONFIG_SMART_HOME_MILOCO_BRIDGE
static lv_obj_t *create_device_card(lv_obj_t *grid,
                                    smart_home_lvgl_t *ui,
                                    int slot,
                                    const smart_home_device_t *device)
{
    int card_w = device_card_width();
    int card_h = smart_home_lvgl_compact() ? 92 : 112;
    lv_obj_t *card = lv_obj_create(grid);
    char text[128];

    lv_obj_set_size(card, card_w, card_h);
    smart_home_lvgl_card_style(card);
    lv_obj_set_user_data(card, (void *)(intptr_t)slot);
    smart_home_lvgl_press_feedback(card);
    lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED, ui);

    format_device_card_text(device, text, sizeof(text));
    set_card_content(card, device_icon(device), device_display_name(device), text,
                     device && device->on, ui, slot);

    return card;
}
#endif

#ifndef CONFIG_SMART_HOME_MILOCO_BRIDGE
static lv_obj_t *create_add_card(lv_obj_t *grid, smart_home_lvgl_t *ui)
{
    int card_w = device_card_width();
    int card_h = smart_home_lvgl_compact() ? 92 : 112;
    lv_obj_t *card = lv_obj_create(grid);

    lv_obj_set_size(card, card_w, card_h);
    smart_home_lvgl_card_style(card);
    smart_home_lvgl_press_feedback(card);
    lv_obj_add_event_cb(card, add_device_cb, LV_EVENT_CLICKED, ui);
    set_card_content(card, ICON_ADD, "添加设备", "接入新的家庭设备", 0,
                     NULL, -1);
    smart_home_lvgl_set_bg(card, SMART_HOME_UI_COLOR_SURFACE_SOFT);
    return card;
}
#endif

#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
static void format_remote_command_text(const caddons_node_info_t *node,
                                       char *buffer,
                                       size_t buffer_size)
{
    const char *separator = "";
    size_t i;
    size_t used;

    if (!buffer || buffer_size == 0u) {
        return;
    }

    snprintf(buffer, buffer_size, "支持操作：");
    used = strlen(buffer);
    if (!node || node->command_count == 0u) {
        snprintf(buffer, buffer_size, "暂无可用操作");
        return;
    }

    for (i = 0; i < node->command_count &&
                i < CAGENT_ADDONS_MAX_NODE_COMMANDS; i++) {
        int written;

        if (!node->commands[i][0]) {
            continue;
        }
        written = snprintf(buffer + used, buffer_size - used, "%s%s",
                           separator, node->commands[i]);
        if (written < 0 || (size_t)written >= buffer_size - used) {
            if (buffer_size >= 4u) {
                memcpy(buffer + buffer_size - 4u, "...", 4u);
            }
            return;
        }
        used += (size_t)written;
        separator = ", ";
    }

    if (used == strlen("支持操作：")) {
        snprintf(buffer, buffer_size, "暂无可用操作");
    }
}

static void remote_node_card_deleted(lv_event_t *event)
{
    smart_home_lvgl_node_card_t *binding = lv_event_get_user_data(event);
    memset(binding, 0, sizeof(*binding));
}

static lv_obj_t *create_remote_node_card(lv_obj_t *grid,
                                         const caddons_node_info_t *node,
                                         smart_home_lvgl_node_card_t *binding)
{
    int card_w = device_card_width();
    int card_h = smart_home_lvgl_compact() ? 104 : 112;
    int font_size = smart_home_lvgl_compact() ? 10 : 11;
    lv_obj_t *card;
    lv_obj_t *content;
    lv_obj_t *label;
    char commands[192];
    const char *name;

    card = lv_obj_create(grid);
    lv_obj_set_size(card, card_w, card_h);
    smart_home_lvgl_card_style(card);
    smart_home_lvgl_set_bg(card, SMART_HOME_UI_COLOR_SURFACE_SOFT);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

    binding->card = card;
    snprintf(binding->id, sizeof(binding->id), "%s", node->node_id);
    lv_obj_add_event_cb(card, remote_node_card_deleted, LV_EVENT_DELETE, binding);

    content = lv_obj_create(card);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(content, smart_home_lvgl_compact() ? 2 : 4, 0);
    lv_obj_set_style_pad_left(content, 6, 0);
    lv_obj_set_style_pad_right(content, 6, 0);

    smart_home_lvgl_icon_create(content,
                                ICON_DEVICE_GENERIC,
                                smart_home_lvgl_compact() ? 18 : 22,
                                smart_home_lvgl_compact() ? 18 : 22);
    name = node->display_name[0] ? node->display_name : node->node_id;
    label = smart_home_lvgl_label_create(content, name,
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                         font_size + 1);
    binding->name = label;
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    label = smart_home_lvgl_label_create(content, "在线",
                                         SMART_HOME_UI_COLOR_SUCCESS,
                                         font_size);
    label = smart_home_lvgl_label_create(content, "来源：节点设备",
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                         font_size);
    format_remote_command_text(node, commands, sizeof(commands));
    label = smart_home_lvgl_label_create(content, commands,
                                         SMART_HOME_UI_COLOR_TEXT_MUTED,
                                         font_size);
    lv_obj_set_width(label, lv_pct(100));
    binding->commands = label;
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return card;
}

static void append_online_remote_node_cards(smart_home_lvgl_t *ui)
{
    caddons_node_info_t nodes[CAGENT_ADDONS_MAX_NODES];
    size_t count = 0;
    int show = device_matches_source(ui, 2);
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    show = show && miloco_matches_room(ui, "");
#else
    show = show && (!ui->panel_room_dd || !lv_dropdown_get_selected(ui->panel_room_dd));
#endif
    if (ui->app && ui->app->node_gateway) {
        count = smart_home_node_gateway_list(ui->app->node_gateway, nodes,
                                             CAGENT_ADDONS_MAX_NODES, &ui->remote_node_revision);
    }
    for (size_t i = 0; i < CAGENT_ADDONS_MAX_NODES; i++) {
        smart_home_lvgl_node_card_t *binding = &ui->node_cards[i];
        if (!binding->card) continue;
        size_t j;
        for (j = 0; j < count; j++) {
            if (show && nodes[j].online && !strcmp(binding->id, nodes[j].node_id)) break;
        }
        if (j == count) lv_obj_delete(binding->card);
    }
    for (size_t i = 0; show && i < count; i++) {
        if (!nodes[i].online) continue;
        smart_home_lvgl_node_card_t *binding = NULL;
        for (size_t j = 0; j < CAGENT_ADDONS_MAX_NODES; j++) {
            if (ui->node_cards[j].card && !strcmp(ui->node_cards[j].id, nodes[i].node_id)) {
                binding = &ui->node_cards[j]; break;
            }
        }
        if (!binding) {
            for (size_t j = 0; j < CAGENT_ADDONS_MAX_NODES; j++) {
                if (!ui->node_cards[j].card) { binding = &ui->node_cards[j]; break; }
            }
            if (!binding) break;
            create_remote_node_card(ui->panel_grid, &nodes[i], binding);
        }
        const char *name = nodes[i].display_name[0] ? nodes[i].display_name : nodes[i].node_id;
        char commands[192];
        format_remote_command_text(&nodes[i], commands, sizeof(commands));
        if (strcmp(lv_label_get_text(binding->name), name)) lv_label_set_text(binding->name, name);
        if (strcmp(lv_label_get_text(binding->commands), commands))
            lv_label_set_text(binding->commands, commands);
    }
}

static void remote_node_timer_cb(lv_timer_t *timer)
{
    smart_home_lvgl_t *ui = lv_timer_get_user_data(timer);
    uint32_t revision = 0;

    if (!ui || !ui->app || !ui->app->node_gateway) {
        return;
    }

    (void)smart_home_node_gateway_list(ui->app->node_gateway, NULL, 0,
                                        &revision);
    if (revision != ui->remote_node_revision) {
        smart_home_lvgl_refresh_cards(ui);
    }
}
#endif

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
/* 绑定记录归 UI 所有，did 地址在卡片存活期内稳定。 */
#define MILOCO_CONFIRM_WAIT_MS 15000u

static void miloco_label_update(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

static void miloco_sheet_deleted(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    smart_home_bulk_free(ui->miloco_sheet_context);
    ui->miloco_sheet_context = NULL;
    ui->miloco_sheet = NULL;
    ui->miloco_sheet_feedback = NULL;
    ui->miloco_sheet_did[0] = '\0';
}

/* ── 米家通用控制抽屉：按 controls 的值类型渲染 ──
 *   BOOL→开关  ENUM→分段选择  ACTION→按钮
 * 状态以 worker 回读为准；revision 变化时刷新控件。 */
static void miloco_sheet_close_impl(smart_home_lvgl_t *ui)
{
    if (ui && ui->miloco_sheet) {
        lv_obj_delete(ui->miloco_sheet);
        ui->miloco_sheet = NULL;
        ui->miloco_sheet_feedback = NULL;
        ui->miloco_sheet_did[0] = '\0';
    }
}

static void miloco_sheet_close(lv_event_t *event)
{
    miloco_sheet_close_impl((smart_home_lvgl_t *)lv_event_get_user_data(event));
}

static int miloco_find_device(smart_home_lvgl_t *ui, const char *did,
                               smart_home_miloco_device_t *out)
{
    if (!ui || !ui->app || !ui->app->miloco || !did) {
        return 0;
    }

    /* 按 did 单条拷贝，避免为了找一个设备把整份 19 KB 快照搬上栈。 */
    return smart_home_miloco_copy_device(ui->app->miloco, did, out) ? 1 : 0;
}

static void miloco_feedback(smart_home_lvgl_t *ui, const char *text, int error)
{
    lv_obj_t *labels[] = {ui->panel_feedback_label, ui->miloco_sheet_feedback};
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        if (labels[i]) {
            lv_label_set_text(labels[i], text);
            lv_obj_set_style_text_color(labels[i], error ?
                SMART_HOME_UI_COLOR_DANGER : SMART_HOME_UI_COLOR_PRIMARY_DARK, 0);
        }
    }
}

static void miloco_restore_switch(lv_obj_t *sw, int on)
{
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
}

/* 控件私有数据不引用临时设备快照。容量涵盖每项的全部枚举选项。 */
typedef struct {
    smart_home_lvgl_t *ui;
    char iid[14];
    const char *operation;
    int32_t value;
    int32_t confirmed_value;
    lv_obj_t *widget;
    int pending;
    uint32_t submitted_at;
} miloco_sheet_req_t;

typedef struct {
    smart_home_miloco_device_t snapshot;
    lv_obj_t *title;
    lv_obj_t *status;
    size_t request_count;
    miloco_sheet_req_t requests[
        SMART_HOME_MILOCO_MAX_CONTROLS * (SMART_HOME_MILOCO_MAX_OPTIONS + 1)];
} miloco_sheet_context_t;

static void miloco_sheet_submit(miloco_sheet_req_t *req, int32_t value,
                                lv_obj_t *sw)
{
    smart_home_miloco_device_t dev;
    smart_home_lvgl_t *ui = req->ui;
    int found = miloco_find_device(ui, ui->miloco_sheet_did, &dev);
    int32_t confirmed = req->confirmed_value;
    req->pending = 0;
    if (found) {
        for (size_t i = 0; i < dev.control_count &&
             i < SMART_HOME_MILOCO_MAX_CONTROLS; i++) {
            if (!strcmp(dev.controls[i].iid, req->iid)) {
                confirmed = dev.controls[i].value;
                break;
            }
        }
    }
    if (!found || !dev.online || !smart_home_miloco_reachable(ui->app->miloco)) {
        if (sw) miloco_restore_switch(sw, confirmed);
        miloco_feedback(ui, "设备当前不可用，请连接后重试", 1);
        return;
    }
    int ret = smart_home_miloco_submit_control(ui->app->miloco,
        ui->miloco_sheet_did, req->iid, req->operation, value);
    if (ret != AGENT_OK) {
        if (sw) miloco_restore_switch(sw, confirmed);
        miloco_feedback(ui, "操作未能提交，请稍后重试", 1);
        return;
    }
    if (sw) {
        req->pending = 1;
        req->value = value;
        req->submitted_at = lv_tick_get();
    }
    miloco_feedback(ui, "已提交，等待设备状态更新", 0);
}

static void miloco_sheet_set_cb(lv_event_t *event)
{
    miloco_sheet_req_t *req = lv_event_get_user_data(event);
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || !req || !req->ui) {
        return;
    }
    lv_obj_t *sw = lv_event_get_current_target(event);
    miloco_sheet_submit(req, lv_obj_has_state(sw, LV_STATE_CHECKED), sw);
}

static void miloco_sheet_pick_cb(lv_event_t *event)
{
    miloco_sheet_req_t *req = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_CLICKED && req && req->ui) {
        miloco_sheet_submit(req, req->value, NULL);
    }
}

static void miloco_sheet_action_cb(lv_event_t *event)
{
    miloco_sheet_req_t *req = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_CLICKED && req && req->ui) {
        miloco_sheet_submit(req, 0, NULL);
    }
}

static void miloco_card_click_cb(lv_event_t *event);

static void miloco_sheet_build(smart_home_lvgl_t *ui, const char *did)
{
    smart_home_miloco_device_t devices[SMART_HOME_MILOCO_MAX_DEVICES];
    smart_home_miloco_device_t *dev = NULL;
    miloco_sheet_context_t *context;
    lv_obj_t *sheet;
    lv_obj_t *row;
    lv_obj_t *label;
    lv_obj_t *btn;
    lv_obj_t *sw;
    int drawer_w;
    int drawer_h;
    int y = 0;
    uint8_t k;
    uint8_t req_slot = 0;
    size_t count;
    size_t i;

    count = smart_home_miloco_list(ui->app->miloco, devices,
                                   SMART_HOME_MILOCO_MAX_DEVICES, NULL);
    for (i = 0; i < count; i++) {
        if (strcmp(devices[i].did, did) == 0) {
            dev = &devices[i];
            break;
        }
    }
    if (!dev) {
        return;
    }

    context = smart_home_bulk_alloc(sizeof(*context));
    if (!context) {
        miloco_feedback(ui, "暂时无法打开设备，请稍后重试", 1);
        return;
    }
    memset(context, 0, sizeof(*context));
    context->snapshot = *dev;
    int available = dev->online && smart_home_miloco_reachable(ui->app->miloco);
    ui->miloco_sheet_context = context;
    drawer_w = smart_home_lvgl_compact() ? smart_home_lvgl_content_w() :
              smart_home_lvgl_disp_w() * 56 / 100;
    drawer_h = smart_home_lvgl_disp_h() - SMART_HOME_TOPBAR_H -
               SMART_HOME_NAV_H - 40;
    sheet = lv_obj_create(ui->screen_panel);
    lv_obj_remove_style_all(sheet);
    lv_obj_set_size(sheet, drawer_w, drawer_h);
    lv_obj_align(sheet, LV_ALIGN_TOP_RIGHT, -12, SMART_HOME_TOPBAR_H + 20);
    smart_home_lvgl_card_style(sheet);
    lv_obj_set_style_pad_all(sheet, 16, 0);
    ui->miloco_sheet = sheet;
    lv_obj_add_event_cb(sheet, miloco_sheet_deleted, LV_EVENT_DELETE, ui);
    snprintf(ui->miloco_sheet_did, sizeof(ui->miloco_sheet_did), "%s", did);

    label = smart_home_lvgl_label_create(sheet, dev->name,
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                         16);
    context->title = label;
    lv_obj_set_width(label, lv_pct(65));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
    label = smart_home_lvgl_label_create(sheet,
                                         available ? "● 在线 · 米家" :
                                                       "○ 离线 · 米家",
                                         available ?
                                             SMART_HOME_UI_COLOR_SUCCESS :
                                             SMART_HOME_UI_COLOR_TEXT_MUTED,
                                         12);
    context->status = label;
    lv_obj_align(label, LV_ALIGN_TOP_RIGHT, 0, 4);
    btn = lv_btn_create(sheet);
    lv_obj_set_size(btn, 64, 32);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, 0, 24);
    label = smart_home_lvgl_label_create(btn, "关闭", lv_color_white(), 12);
    lv_obj_center(label);
    smart_home_lvgl_press_feedback(btn);
    lv_obj_add_event_cb(btn, miloco_sheet_close, LV_EVENT_CLICKED, ui);
    ui->miloco_sheet_feedback = smart_home_lvgl_label_create(
        sheet, "状态以设备回读为准", SMART_HOME_UI_COLOR_TEXT_MUTED, 12);
    lv_obj_set_width(ui->miloco_sheet_feedback, lv_pct(100));
    lv_label_set_long_mode(ui->miloco_sheet_feedback, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(ui->miloco_sheet_feedback, 0, 60);
    y = 88;

    if (dev->control_count == 0) {
        /* spec 拉取中或失败（服务侧每轮重试，见 spec_attempt_take）：
         * 给出明确占位而不是空白抽屉。 */
        label = smart_home_lvgl_label_create(
            sheet, "暂无可控项：设备规格拉取中或失败，将自动重试。",
            SMART_HOME_UI_COLOR_TEXT_MUTED, 12);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(label, 0, y);
        return;
    }

    for (k = 0; k < dev->control_count && k <
         SMART_HOME_MILOCO_MAX_CONTROLS; k++) {
        const smart_home_miloco_control_t *ctrl = &dev->controls[k];
        miloco_sheet_req_t *req = &context->requests[req_slot++];

        req->ui = ui;
        req->confirmed_value = ctrl->value;
        snprintf(req->iid, sizeof(req->iid), "%s", ctrl->iid);
        req->operation = ctrl->type == SMART_HOME_MILOCO_CTRL_ACTION ?
                         "action" : "set";

        row = lv_obj_create(sheet);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 40);
        lv_obj_set_pos(row, 0, y);
        label = smart_home_lvgl_label_create(row, ctrl->desc,
                                             SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                             13);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

        if (ctrl->type == SMART_HOME_MILOCO_CTRL_BOOL) {
            req->value = 0;
            sw = lv_switch_create(row);
            smart_home_lvgl_press_feedback(sw);
            req->widget = sw;
            lv_obj_set_size(sw, 44, 24);
            lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
            if (ctrl->value) {
                lv_obj_add_state(sw, LV_STATE_CHECKED);
            }
            if (!available) lv_obj_add_state(sw, LV_STATE_DISABLED);
            lv_obj_add_event_cb(sw, miloco_sheet_set_cb,
                                LV_EVENT_VALUE_CHANGED, req);
        } else if (ctrl->type == SMART_HOME_MILOCO_CTRL_ENUM &&
                   ctrl->option_count > 0) {
            uint8_t j;

            for (j = 0; j < ctrl->option_count &&
                 j < SMART_HOME_MILOCO_MAX_OPTIONS; j++) {
                miloco_sheet_req_t *oreq = &context->requests[req_slot++];
                lv_obj_t *opt;

                oreq->ui = ui;
                oreq->confirmed_value = ctrl->value;
                snprintf(oreq->iid, sizeof(oreq->iid), "%s", ctrl->iid);
                oreq->operation = "set";
                oreq->value = ctrl->options[j].value;
                opt = lv_btn_create(row);
                oreq->widget = opt;
                lv_obj_set_size(opt, 56, 30);
                lv_obj_align(opt, LV_ALIGN_RIGHT_MID,
                             -(int)(j * 62), 0);
                label = smart_home_lvgl_label_create(
                    opt, ctrl->options[j].name,
                    ctrl->value == ctrl->options[j].value ?
                        lv_color_white() :
                        SMART_HOME_UI_COLOR_TEXT_PRIMARY, 11);
                lv_obj_center(label);
                if (ctrl->value == ctrl->options[j].value) {
                    smart_home_lvgl_set_bg(opt, SMART_HOME_UI_COLOR_PRIMARY);
                } else {
                    smart_home_lvgl_set_bg(opt,
                        SMART_HOME_UI_COLOR_SURFACE_SOFT);
                }
                if (!available) lv_obj_add_state(opt, LV_STATE_DISABLED);
                smart_home_lvgl_press_feedback(opt);
                lv_obj_add_event_cb(opt, miloco_sheet_pick_cb,
                                    LV_EVENT_CLICKED, oreq);
            }
        } else if (ctrl->type == SMART_HOME_MILOCO_CTRL_RANGE) {
            char value_text[48];
            snprintf(value_text, sizeof(value_text), "%ld · 请在首页调节", (long)ctrl->value);
            label = smart_home_lvgl_label_create(row, value_text,
                        SMART_HOME_UI_COLOR_TEXT_SECONDARY, 12);
            lv_obj_align(label, LV_ALIGN_RIGHT_MID, 0, 0);
        } else {
            req->value = 0;
            btn = lv_btn_create(row);
            req->widget = btn;
            lv_obj_set_size(btn, 88, 32);
            lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
            label = smart_home_lvgl_label_create(btn, "执行", lv_color_white(),
                                                 12);
            lv_obj_center(label);
            smart_home_lvgl_set_bg(btn, SMART_HOME_UI_COLOR_PRIMARY);
            if (!available) lv_obj_add_state(btn, LV_STATE_DISABLED);
            smart_home_lvgl_press_feedback(btn);
            lv_obj_add_event_cb(btn, miloco_sheet_action_cb,
                                LV_EVENT_CLICKED, req);
        }
        y += 48;
    }
    context->request_count = req_slot;
}

static void miloco_card_click_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    const char *did = lv_obj_get_user_data(lv_event_get_current_target(event));

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui ||
        !ui->app || !ui->app->miloco || !did) {
        return;
    }
    miloco_sheet_close_impl(ui);
    miloco_sheet_build(ui, did);
}

static int miloco_same_controls(const smart_home_miloco_device_t *a,
                                 const smart_home_miloco_device_t *b)
{
    if (a->control_count != b->control_count) return 0;
    for (size_t i = 0; i < a->control_count && i < SMART_HOME_MILOCO_MAX_CONTROLS; i++) {
        const smart_home_miloco_control_t *x = &a->controls[i], *y = &b->controls[i];
        if (strcmp(x->iid, y->iid) || strcmp(x->desc, y->desc) ||
            x->type != y->type || x->option_count != y->option_count) return 0;
        for (size_t j = 0; j < x->option_count && j < SMART_HOME_MILOCO_MAX_OPTIONS; j++) {
            if (x->options[j].value != y->options[j].value ||
                strcmp(x->options[j].name, y->options[j].name)) return 0;
        }
    }
    return 1;
}

void smart_home_lvgl_miloco_sheet_refresh(smart_home_lvgl_t *ui)
{
    smart_home_miloco_device_t dev;
    if (!ui || !ui->miloco_sheet || !ui->miloco_sheet_context) return;
    if (!miloco_find_device(ui, ui->miloco_sheet_did, &dev)) {
        miloco_sheet_close_impl(ui);
        miloco_feedback(ui, "设备已移除，请重新选择", 1);
        return;
    }
    miloco_sheet_context_t *context = ui->miloco_sheet_context;
    if (!miloco_same_controls(&context->snapshot, &dev)) {
        char did[24];
        int32_t scroll_y = lv_obj_get_scroll_y(ui->miloco_sheet);
        snprintf(did, sizeof(did), "%s", dev.did);
        miloco_sheet_close_impl(ui);
        miloco_sheet_build(ui, did);
        if (ui->miloco_sheet) {
            lv_obj_update_layout(ui->miloco_sheet);
            lv_obj_scroll_to_y(ui->miloco_sheet, scroll_y, LV_ANIM_OFF);
        }
        return;
    }
    int online = dev.online && smart_home_miloco_reachable(ui->app->miloco);
    miloco_label_update(context->title, dev.name);
    miloco_label_update(context->status, online ? "● 在线 · 米家" : "○ 不可用 · 米家");
    lv_obj_set_style_text_color(context->status, online ?
        SMART_HOME_UI_COLOR_SUCCESS : SMART_HOME_UI_COLOR_TEXT_MUTED, 0);
    for (size_t i = 0; i < context->request_count; i++) {
        miloco_sheet_req_t *req = &context->requests[i];
        if (!req->widget) continue; /* 枚举项共用的描述记录。 */
        const smart_home_miloco_control_t *ctrl = NULL;
        for (size_t j = 0; j < dev.control_count && j < SMART_HOME_MILOCO_MAX_CONTROLS; j++) {
            if (!strcmp(req->iid, dev.controls[j].iid)) { ctrl = &dev.controls[j]; break; }
        }
        if (!ctrl) continue;
        if (online) lv_obj_remove_state(req->widget, LV_STATE_DISABLED);
        else lv_obj_add_state(req->widget, LV_STATE_DISABLED);
        if (ctrl->type == SMART_HOME_MILOCO_CTRL_BOOL) {
            if (req->pending && online && ctrl->value != req->value &&
                lv_tick_elaps(req->submitted_at) < MILOCO_CONFIRM_WAIT_MS) continue;
            if (req->pending && ctrl->value != req->value)
                miloco_feedback(ui, "设备状态尚未确认，请稍后重试", 1);
            if (req->pending && ctrl->value == req->value)
                miloco_feedback(ui, "设备状态已更新", 0);
            req->pending = 0;
            miloco_restore_switch(req->widget, ctrl->value);
        } else if (ctrl->type == SMART_HOME_MILOCO_CTRL_ENUM &&
                   req->confirmed_value != ctrl->value) {
            int selected = ctrl->value == req->value;
            smart_home_lvgl_set_bg(req->widget, selected ?
                SMART_HOME_UI_COLOR_PRIMARY : SMART_HOME_UI_COLOR_SURFACE_SOFT);
            lv_obj_set_style_text_color(lv_obj_get_child(req->widget, 0), selected ?
                lv_color_white() : SMART_HOME_UI_COLOR_TEXT_PRIMARY, 0);
        }
        req->confirmed_value = ctrl->value;
    }
    context->snapshot = dev;
}

static void miloco_switch_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    lv_obj_t *sw = lv_event_get_current_target(event);
    const char *did = lv_obj_get_user_data(sw);

    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || !ui ||
        !ui->app || !ui->app->miloco || !did) {
        return;
    }
    smart_home_miloco_device_t dev;
    int requested = lv_obj_has_state(sw, LV_STATE_CHECKED);
    for (size_t i = 0; i < SMART_HOME_MILOCO_MAX_DEVICES; i++) {
        if (ui->miloco_cards[i].card && !strcmp(ui->miloco_cards[i].did, did))
            ui->miloco_cards[i].pending = 0;
    }
    int found = miloco_find_device(ui, did, &dev);
    if (!found || !dev.online || !smart_home_miloco_reachable(ui->app->miloco)) {
        miloco_restore_switch(sw, found ? dev.power_on : !requested);
        miloco_feedback(ui, "设备当前不可用，请连接后重试", 1);
        return;
    }
    if (smart_home_miloco_submit_power(ui->app->miloco, did, requested) != AGENT_OK) {
        miloco_restore_switch(sw, dev.power_on);
        miloco_feedback(ui, "操作未能提交，请稍后重试", 1);
        return;
    }
    for (size_t i = 0; i < SMART_HOME_MILOCO_MAX_DEVICES; i++) {
        smart_home_lvgl_miloco_card_t *binding = &ui->miloco_cards[i];
        if (binding->card && !strcmp(binding->did, did)) {
            binding->pending = 1;
            binding->submitted_at = lv_tick_get();
            break;
        }
    }
    miloco_feedback(ui, "已提交，等待设备状态更新", 0);
}

static void miloco_card_deleted(lv_event_t *event)
{
    smart_home_lvgl_miloco_card_t *binding = lv_event_get_user_data(event);
    memset(binding, 0, sizeof(*binding));
}

static lv_obj_t *create_miloco_card(smart_home_lvgl_t *ui,
                                    const smart_home_miloco_device_t *device,
                                    smart_home_lvgl_miloco_card_t *binding)
{
    int compact = smart_home_lvgl_compact();
    int card_w = device_card_width();
    int card_h = compact ? 104 : 112;
    int font_size = compact ? 10 : 11;
    const char *icon = ICON_DEVICE_GENERIC;
    lv_obj_t *card;
    lv_obj_t *content;
    lv_obj_t *label;
    lv_obj_t *sw;
    char room_text[48];

    switch (device->category) {
    case SMART_HOME_MILOCO_CATEGORY_LIGHT:
        icon = ICON_DEVICE_LIGHT;
        break;
    case SMART_HOME_MILOCO_CATEGORY_AC:
        icon = ICON_DEVICE_AC;
        break;
    case SMART_HOME_MILOCO_CATEGORY_CAMERA:
        icon = ICON_STATUS_CAMERA;
        break;
    case SMART_HOME_MILOCO_CATEGORY_FAN:
        icon = ICON_DEVICE_FAN;
        break;
    default:
        icon = ICON_DEVICE_GENERIC;
        break;
    }

    card = lv_obj_create(ui->panel_grid);
    lv_obj_set_size(card, card_w, card_h);
    smart_home_lvgl_card_style(card);
    smart_home_lvgl_set_bg(card, SMART_HOME_UI_COLOR_SURFACE_SOFT);

    binding->card = card;
    binding->category = device->category;
    binding->controllable = device->controllable;
    snprintf(binding->did, sizeof(binding->did), "%s", device->did);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card, binding->did);
    smart_home_lvgl_press_feedback(card);
    lv_obj_add_event_cb(card, miloco_card_click_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(card, miloco_card_deleted, LV_EVENT_DELETE, binding);

    content = lv_obj_create(card);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(content, compact ? 2 : 4, 0);
    lv_obj_set_style_pad_left(content, 6, 0);
    lv_obj_set_style_pad_right(content, 6, 0);

    smart_home_lvgl_icon_create(content, icon, compact ? 18 : 22,
                                compact ? 18 : 22);
    label = smart_home_lvgl_label_create(content, device->name,
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                         font_size + 1);
    binding->name = label;
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    if (device->room[0]) {
        snprintf(room_text, sizeof(room_text), "%s · 米家",
                 device->room);
    } else {
        snprintf(room_text, sizeof(room_text), "米家");
    }
    label = smart_home_lvgl_label_create(content, room_text,
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                         font_size);
    binding->room = label;
    label = smart_home_lvgl_label_create(
        content, device->online ? "在线" : "离线",
        device->online ? SMART_HOME_UI_COLOR_SUCCESS :
                         SMART_HOME_UI_COLOR_TEXT_MUTED,
        font_size);

    binding->status = label;
    if (device->controllable) {
        sw = lv_switch_create(content);
        smart_home_lvgl_press_feedback(sw);
        binding->power = sw;
        lv_obj_set_size(sw, 40, 22);
        if (device->power_on) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        lv_obj_set_user_data(sw, lv_obj_get_user_data(card));
        if (!device->online) lv_obj_add_state(sw, LV_STATE_DISABLED);
        lv_obj_add_event_cb(sw, miloco_switch_cb, LV_EVENT_VALUE_CHANGED, ui);
    }
    return card;
}

/* 网关未连接时设备页的主体提示：真实设备在网关连接后出现。 */
static void create_miloco_placeholder_card(smart_home_lvgl_t *ui)
{
    lv_obj_t *card;
    lv_obj_t *label;
    int card_w = device_card_width();
    int card_h = smart_home_lvgl_compact() ? 104 : 112;

    card = lv_obj_create(ui->panel_grid);
    lv_obj_set_size(card, card_w, card_h);
    smart_home_lvgl_card_style(card);
    smart_home_lvgl_set_bg(card, SMART_HOME_UI_COLOR_SURFACE_SOFT);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    ui->miloco_placeholder = card;

    label = smart_home_lvgl_label_create(card, "米家设备",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                         smart_home_lvgl_compact() ? 11 : 12);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
    label = smart_home_lvgl_label_create(
        card, "网关未连接\n在更多页配置\n米家网关后显示",
        SMART_HOME_UI_COLOR_TEXT_SECONDARY,
        smart_home_lvgl_compact() ? 10 : 11);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 24);
}

static void append_miloco_cards(smart_home_lvgl_t *ui)
{
    smart_home_miloco_device_t devices[SMART_HOME_MILOCO_MAX_DEVICES];
    size_t count = 0;
    int show = device_matches_source(ui, 1);
    int reachable = ui->app && ui->app->miloco &&
                    smart_home_miloco_reachable(ui->app->miloco);
    if (ui->app && ui->app->miloco) {
        count = smart_home_miloco_list(ui->app->miloco, devices,
                                      SMART_HOME_MILOCO_MAX_DEVICES, &ui->miloco_revision);
    }
    for (size_t i = 0; i < SMART_HOME_MILOCO_MAX_DEVICES; i++) {
        smart_home_lvgl_miloco_card_t *binding = &ui->miloco_cards[i];
        if (!binding->card) continue;
        size_t j;
        for (j = 0; j < count; j++) {
            if (show && miloco_matches_room(ui, devices[j].room) &&
                !strcmp(binding->did, devices[j].did) &&
                binding->category == (int)devices[j].category &&
                binding->controllable == devices[j].controllable) break;
        }
        if (j == count) lv_obj_delete(binding->card);
    }
    if (show && !reachable && !ui->miloco_placeholder) create_miloco_placeholder_card(ui);
    else if ((!show || reachable) && ui->miloco_placeholder) {
        lv_obj_delete(ui->miloco_placeholder);
        ui->miloco_placeholder = NULL;
    }
    for (size_t i = 0; show && i < count; i++) {
        smart_home_miloco_device_t *dev = &devices[i];
        if (!miloco_matches_room(ui, dev->room)) continue;
        smart_home_lvgl_miloco_card_t *binding = NULL;
        for (size_t j = 0; j < SMART_HOME_MILOCO_MAX_DEVICES; j++) {
            if (ui->miloco_cards[j].card && !strcmp(ui->miloco_cards[j].did, dev->did)) {
                binding = &ui->miloco_cards[j]; break;
            }
        }
        if (!binding) {
            for (size_t j = 0; j < SMART_HOME_MILOCO_MAX_DEVICES; j++) {
                if (!ui->miloco_cards[j].card) { binding = &ui->miloco_cards[j]; break; }
            }
            if (!binding) break;
            create_miloco_card(ui, dev, binding);
        }
        char room[48];
        snprintf(room, sizeof(room), "%s · 米家", miloco_room_name(dev->room));
        miloco_label_update(binding->name, dev->name);
        miloco_label_update(binding->room, room);
        miloco_label_update(binding->status, !reachable ? "状态未知" :
                            dev->online ? "在线" : "离线");
        lv_obj_set_style_text_color(binding->status, reachable && dev->online ?
            SMART_HOME_UI_COLOR_SUCCESS : SMART_HOME_UI_COLOR_TEXT_MUTED, 0);
        if (binding->power) {
            int desired = lv_obj_has_state(binding->power, LV_STATE_CHECKED);
            if (reachable && dev->online) lv_obj_remove_state(binding->power, LV_STATE_DISABLED);
            else lv_obj_add_state(binding->power, LV_STATE_DISABLED);
            if (binding->pending && reachable && dev->online && desired != dev->power_on &&
                lv_tick_elaps(binding->submitted_at) < MILOCO_CONFIRM_WAIT_MS) continue;
            if (binding->pending && desired != dev->power_on)
                miloco_feedback(ui, "设备状态尚未确认，请稍后重试", 1);
            if (binding->pending && desired == dev->power_on)
                miloco_feedback(ui, "设备状态已更新", 0);
            binding->pending = 0;
            miloco_restore_switch(binding->power, dev->power_on);
        }
    }
}

static int miloco_pending_expired(smart_home_lvgl_t *ui)
{
    for (size_t i = 0; i < SMART_HOME_MILOCO_MAX_DEVICES; i++) {
        if (ui->miloco_cards[i].pending &&
            lv_tick_elaps(ui->miloco_cards[i].submitted_at) >= MILOCO_CONFIRM_WAIT_MS) return 1;
    }
    miloco_sheet_context_t *context = ui->miloco_sheet_context;
    for (size_t i = 0; context && i < context->request_count; i++) {
        if (context->requests[i].pending &&
            lv_tick_elaps(context->requests[i].submitted_at) >= MILOCO_CONFIRM_WAIT_MS) return 1;
    }
    return 0;
}

static void miloco_timer_cb(lv_timer_t *timer)
{
    smart_home_lvgl_t *ui = lv_timer_get_user_data(timer);
    uint32_t revision = 0;

    if (!ui || !ui->app || !ui->app->miloco) {
        return;
    }
    (void)smart_home_miloco_list(ui->app->miloco, NULL, 0, &revision);
    if (revision != ui->miloco_polled_revision ||
        (ui->active_tab == SMART_HOME_TAB_DEVICES && miloco_pending_expired(ui))) {
        ui->miloco_polled_revision = revision;
        if (ui->miloco_sheet) {
            smart_home_lvgl_miloco_sheet_refresh(ui);
        }
        if (ui->active_tab == SMART_HOME_TAB_HOME) {
            smart_home_lvgl_refresh_home(ui);
        } else {
            smart_home_lvgl_refresh_cards(ui);
        }
    }
}

/* 设备页可见时启动轮询、离开时停止（与安防页摄像头同一门控模式）。 */
void smart_home_lvgl_miloco_poll_set_enabled(smart_home_lvgl_t *ui, int enable)
{
    if (!ui) {
        return;
    }
    if (enable && !ui->miloco_timer) {
        ui->miloco_timer = lv_timer_create(miloco_timer_cb, 1000, ui);
    } else if (!enable && ui->miloco_timer) {
        lv_timer_delete(ui->miloco_timer);
        ui->miloco_timer = NULL;
    }
}
#endif

static void rebuild_device_cards(smart_home_lvgl_t *ui)
{
    int slot;

    if (!ui || !ui->panel_grid || !ui->device_state) {
        return;
    }

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (miloco_sync_room_options(ui)) {
        rebuild_room_filters(ui);
    }
    /* 只对新增/移除设备调整对象，状态值在原控件上更新。 */
    (void)slot;
    append_miloco_cards(ui);
#else
    lv_obj_clean(ui->panel_grid);
    for (slot = 0; slot < SMART_HOME_MAX_DEVICES; slot++) {
        smart_home_device_t *device;

        ui->device_cards[slot] = NULL;
        device = smart_home_device_get_by_slot(ui->device_state, slot);
        if (!device) {
            continue;
        }
        if (!device_matches_filter(ui, device)) {
            continue;
        }

        ui->device_cards[slot] =
            create_device_card(ui->panel_grid, ui, slot, device);
    }

    ui->panel_add_btn = create_add_card(ui->panel_grid, ui);
#endif

#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
    append_online_remote_node_cards(ui);
#endif
}

static lv_obj_t *create_sensor_entry(lv_obj_t *row,
                                     smart_home_lvgl_t *ui,
                                     int type,
                                     lv_obj_t **label_out)
{
    int entry_w = smart_home_lvgl_content_w() / 4 - 4;
    lv_obj_t *entry = lv_btn_create(row);
    lv_obj_t *content;
    lv_obj_t *icon;
    lv_obj_t *label;
    const char *icon_name = ICON_TEMP;

    lv_obj_remove_style_all(entry);
    lv_obj_set_size(entry, entry_w, smart_home_lvgl_compact() ? 24 : 28);
    lv_obj_set_style_bg_opa(entry, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(entry, 8, 0);
    lv_obj_set_user_data(entry, (void *)(intptr_t)type);
    smart_home_lvgl_press_feedback(entry);
    lv_obj_add_event_cb(entry, env_click_cb, LV_EVENT_CLICKED, ui);

    if (type == ENV_HUM) {
        icon_name = ICON_HUMIDITY;
    } else if (type == ENV_LIGHT) {
        icon_name = ICON_SUN;
    }

    content = lv_obj_create(entry);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(content,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(content, smart_home_lvgl_compact() ? 3 : 4, 0);
    lv_obj_center(content);

    icon = smart_home_lvgl_icon_create(content,
                                       icon_name,
                                       smart_home_lvgl_compact() ? 14 : 16,
                                       smart_home_lvgl_compact() ? 14 : 16);
    if (icon) {
        lv_obj_set_style_text_color(icon,
                                    SMART_HOME_UI_COLOR_TEXT_MUTED,
                                    0);
        lv_obj_set_style_image_recolor(icon,
                                        SMART_HOME_UI_COLOR_TEXT_MUTED,
                                        0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    }

    label = smart_home_lvgl_label_create(content,
                                         "",
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                         smart_home_lvgl_compact() ? 11 : 12);
    if (label_out) {
        *label_out = label;
    }

    return entry;
}

void smart_home_lvgl_build_panel_screen(smart_home_lvgl_t *ui)
{
    lv_obj_t *screen;
    lv_obj_t *grid;
    lv_obj_t *sensor_bar;
    lv_obj_t *filter_row;
    lv_obj_t *label;
    int content_w = smart_home_lvgl_content_w();
    int pad_x = smart_home_lvgl_pad_x();
    int compact = smart_home_lvgl_compact();
    int grid_y = SMART_HOME_TOPBAR_H + (compact ? 108 : 146);
    int sensor_h = compact ? 26 : 30;
    int sensor_bottom =
        SMART_HOME_NAV_H + SMART_HOME_NAV_BOTTOM_PAD + (compact ? 8 : 16);
    int grid_h = smart_home_lvgl_disp_h() -
                 grid_y -
                 sensor_bottom -
                 sensor_h -
                 (compact ? 6 : 12);
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    char room_options[MILOCO_ROOM_OPTIONS_SIZE];
#else
    char room_options[128];
#endif

    if (grid_h < (compact ? 84 : 120)) {
        grid_h = compact ? 84 : 120;
    }

    screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    smart_home_lvgl_set_bg(screen, SMART_HOME_UI_COLOR_BG);
    ui->screen_panel = screen;

    smart_home_lvgl_build_top_bar(screen, ui, "OpenVela HOME");

    ui->panel_title = smart_home_lvgl_label_create(screen,
                                                   "我的设备",
                                                   SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                                   compact ? 20 : 28);
    lv_obj_align(ui->panel_title,
                 LV_ALIGN_TOP_LEFT,
                 pad_x,
                 SMART_HOME_TOPBAR_H + (compact ? 10 : 26));

    ui->panel_room_dd = lv_dropdown_create(screen);
    smart_home_lvgl_press_feedback(ui->panel_room_dd);
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    miloco_room_options(ui, room_options, sizeof(room_options));
#else
    room_dropdown_options(ui->device_state, room_options,
                          sizeof(room_options), 1);
#endif
    lv_dropdown_set_options(ui->panel_room_dd, room_options);
    /* Keep the existing selection contract as the source of truth while the
     * product page exposes it as touch-friendly room chips below. */
    lv_obj_set_style_text_font(ui->panel_room_dd, smart_home_lvgl_font(13), 0);
    {
        lv_obj_t *room_list = lv_dropdown_get_list(ui->panel_room_dd);
        if (room_list) {
            lv_obj_set_style_text_font(room_list, smart_home_lvgl_font(13), 0);
        }
    }
    lv_obj_set_size(ui->panel_room_dd, 1, 1);
    lv_obj_add_flag(ui->panel_room_dd, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(ui->panel_room_dd,
                        panel_room_cb,
                        LV_EVENT_VALUE_CHANGED,
                        ui);

    /* 来源筛选：全部/米家/Node，与房间筛选互补。 */
    ui->panel_source_dd = lv_dropdown_create(screen);
    smart_home_lvgl_press_feedback(ui->panel_source_dd);
    lv_dropdown_set_options(ui->panel_source_dd, "全部来源\n米家\n节点");
    lv_obj_set_style_text_font(ui->panel_source_dd, smart_home_lvgl_font(13), 0);
    lv_obj_t *source_list = lv_dropdown_get_list(ui->panel_source_dd);
    if (source_list) {
        lv_obj_set_style_text_font(source_list, smart_home_lvgl_font(13), 0);
    }
    lv_obj_set_size(ui->panel_source_dd, 96, 32);
    lv_obj_align(ui->panel_source_dd, LV_ALIGN_TOP_RIGHT,
                 -smart_home_lvgl_pad_x(), SMART_HOME_TOPBAR_H + 42);
    lv_obj_add_event_cb(ui->panel_source_dd, panel_source_cb,
                        LV_EVENT_VALUE_CHANGED, ui);

    filter_row = lv_obj_create(screen);
    lv_obj_remove_style_all(filter_row);
    lv_obj_set_size(filter_row, content_w, 38);
    lv_obj_align(filter_row, LV_ALIGN_TOP_LEFT, pad_x,
                 SMART_HOME_TOPBAR_H + (compact ? 44 : 70));
    lv_obj_set_style_bg_opa(filter_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(filter_row, 0, 0);
    lv_obj_set_style_pad_column(filter_row, compact ? 6 : 10, 0);
    lv_obj_set_flex_flow(filter_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(filter_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(filter_row, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(filter_row, LV_SCROLLBAR_MODE_OFF);
    ui->panel_filter_row = filter_row;
    rebuild_room_filters(ui);

    ui->panel_feedback_label = smart_home_lvgl_label_create(
        screen, "", SMART_HOME_UI_COLOR_TEXT_MUTED, 12);
    lv_obj_set_width(ui->panel_feedback_label, content_w);
    lv_label_set_long_mode(ui->panel_feedback_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(ui->panel_feedback_label, pad_x, grid_y - 22);

    grid = lv_obj_create(screen);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, content_w, grid_h);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, grid_y);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, compact ? 8 : 12, 0);
    lv_obj_set_style_pad_column(grid, compact ? 8 : 12, 0);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
    ui->panel_grid = grid;

    sensor_bar = lv_obj_create(screen);
    lv_obj_remove_style_all(sensor_bar);
    lv_obj_set_size(sensor_bar, content_w, sensor_h);
    lv_obj_align(sensor_bar,
                 LV_ALIGN_BOTTOM_MID,
                 0,
                 -sensor_bottom);
    lv_obj_set_style_bg_opa(sensor_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(sensor_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sensor_bar,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sensor_bar, 6, 0);
    ui->panel_sensor_bar = sensor_bar;

    create_sensor_entry(sensor_bar, ui, ENV_TEMP, &ui->env_temp_label);
    label = smart_home_lvgl_label_create(sensor_bar,
                                         "|",
                                         SMART_HOME_UI_COLOR_TEXT_MUTED,
                                         12);
    lv_obj_center(label);
    create_sensor_entry(sensor_bar, ui, ENV_HUM, &ui->env_hum_label);
    label = smart_home_lvgl_label_create(sensor_bar,
                                         "|",
                                         SMART_HOME_UI_COLOR_TEXT_MUTED,
                                         12);
    lv_obj_center(label);
    create_sensor_entry(sensor_bar, ui, ENV_LIGHT, &ui->env_light_label);
    label = smart_home_lvgl_label_create(sensor_bar,
                                         "|",
                                         SMART_HOME_UI_COLOR_TEXT_MUTED,
                                         12);
    lv_obj_center(label);
    ui->env_ac_label = smart_home_lvgl_label_create(sensor_bar,
                                                   "",
                                                   SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                                   12);

    smart_home_lvgl_build_nav_bar(screen, ui);
    smart_home_lvgl_refresh_cards(ui);
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
    ui->remote_node_timer = lv_timer_create(remote_node_timer_cb, 500, ui);
#endif
}
