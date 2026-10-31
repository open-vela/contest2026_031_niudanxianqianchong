/* Room metadata is local to this panel. Gateway room configuration is untouched. */
#include "smart_home_lvgl_internal.h"
#include "smart_home_lvgl_room_dialog.h"
#include "../../config/smart_home_config_store.h"
#include "../../smart_home_memory.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
#include "../../miloco/smart_home_miloco.h"
#endif

typedef struct {
    smart_home_lvgl_t *ui;
    lv_obj_t *mask, *panel, *content, *status, *input, *choice, *keyboard;
    lv_timer_t *timer;
    smart_home_state_t state;
    int targets[SMART_HOME_MAX_ROOMS];
    char source[SMART_HOME_ROOM_NAME_SIZE];
    char key[SMART_HOME_V9_KEY_SIZE];
    int was_busy;
} room_dialog_t;
static room_dialog_t *g_room;
/* The detached writer owns only a state snapshot, never app/UI pointers.
 * Closing a popup must not join an I/O thread while holding the display lock. */
static pthread_mutex_t g_save_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_save_busy, g_save_result;
static int g_save_attempted;

const char *smart_home_lvgl_room_display(const char *room) {
    if (!room || !room[0])
        return "未分配";
    if (!strcmp(room, "living_room"))
        return "客厅";
    if (!strcmp(room, "bedroom"))
        return "卧室";
    return room;
}
static int save_status(int *result) {
    pthread_mutex_lock(&g_save_mutex);
    int busy = g_save_busy;
    if (result)
        *result = g_save_result;
    pthread_mutex_unlock(&g_save_mutex);
    return busy;
}
static void *save_worker(void *argument) {
    smart_home_state_t *state = argument;
    cJSON *json = smart_home_device_state_to_json(state);
    int result = json ? smart_home_config_state_save(json) : AGENT_ERROR_NOMEM;
    cJSON_Delete(json);
    smart_home_bulk_free(state);
    pthread_mutex_lock(&g_save_mutex);
    g_save_result = result;
    g_save_busy = 0;
    pthread_mutex_unlock(&g_save_mutex);
    return NULL;
}
static int save_async(room_dialog_t *ctx) {
    pthread_t thread;
    pthread_attr_t attr;
    smart_home_state_t *state = smart_home_bulk_alloc(sizeof(*state));
    if (!state)
        return AGENT_ERROR_NOMEM;
    if (smart_home_device_service_copy_state(&ctx->ui->app->device_service, state)) {
        smart_home_bulk_free(state);
        return AGENT_ERROR_STATE;
    }
    pthread_mutex_lock(&g_save_mutex);
    if (g_save_busy) {
        pthread_mutex_unlock(&g_save_mutex);
        smart_home_bulk_free(state);
        return AGENT_ERROR_STATE;
    }
    g_save_busy = 1;
    g_save_attempted = 1;
    pthread_mutex_unlock(&g_save_mutex);
    int ret = pthread_attr_init(&attr);
    if (!ret) {
        ret = pthread_attr_setstacksize(&attr, 32768);
        if (!ret)
            ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (!ret)
            ret = pthread_create(&thread, &attr, save_worker, state);
        pthread_attr_destroy(&attr);
    }
    if (ret) {
        smart_home_bulk_free(state);
        pthread_mutex_lock(&g_save_mutex);
        g_save_result = AGENT_ERROR;
        g_save_busy = 0;
        pthread_mutex_unlock(&g_save_mutex);
        return AGENT_ERROR;
    }
    ctx->was_busy = 1;
    return AGENT_OK;
}
static void status(room_dialog_t *ctx, const char *text, int error) {
    lv_label_set_text(ctx->status, text);
    lv_obj_set_style_text_color(
        ctx->status,
        error ? SMART_HOME_UI_COLOR_DANGER : SMART_HOME_UI_COLOR_TEXT_SECONDARY, 0);
}
static void deleted(lv_event_t *event) {
    room_dialog_t *ctx = lv_event_get_user_data(event);
    lv_timer_delete(ctx->timer);
    if (g_room == ctx)
        g_room = NULL;
    smart_home_bulk_free(ctx);
}
void smart_home_lvgl_room_dialog_close(void) {
    if (g_room)
        lv_obj_delete(g_room->mask);
}
static void close_cb(lv_event_t *event) {
    (void)event;
    smart_home_lvgl_room_dialog_close();
}
static void stop_cb(lv_event_t *event) {
    lv_event_stop_bubbling(event);
}
static lv_obj_t *text(lv_obj_t *parent, const char *value, int x, int y, int size) {
    lv_obj_t *label = smart_home_lvgl_label_create(
        parent, value, SMART_HOME_UI_COLOR_TEXT_PRIMARY, size);
    lv_obj_set_pos(label, x, y);
    return label;
}
static lv_obj_t *button(lv_obj_t *parent, const char *name, int x, int y,
                        lv_event_cb_t callback, void *data) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 120, 40);
    lv_obj_set_pos(btn, x, y);
    smart_home_lvgl_set_bg(btn, SMART_HOME_UI_COLOR_SURFACE_ON);
    lv_obj_set_style_radius(btn, 10, 0);
    smart_home_lvgl_press_feedback(btn);
    lv_obj_add_event_cb(btn, callback, LV_EVENT_CLICKED, data);
    lv_obj_center(text(btn, name, 0, 0, 16));
    return btn;
}
static void poll(lv_timer_t *timer) {
    room_dialog_t *ctx = lv_timer_get_user_data(timer);
    int result;
    int busy = save_status(&result);
    if (busy)
        status(ctx, "正在保存房间配置…", 0);
    else if (ctx->was_busy) {
        status(ctx,
               result == AGENT_OK ? "房间配置已保存"
                                  : "本次已生效，保存失败；可返回管理页重新保存",
               result != AGENT_OK);
        ctx->was_busy = 0;
        smart_home_lvgl_v9_refresh_views(ctx->ui);
    }
}
static int ready(room_dialog_t *ctx) {
    if (save_status(NULL)) {
        status(ctx, "正在保存，请稍候再操作", 0);
        return 0;
    }
    return 1;
}
static void applied(room_dialog_t *ctx, int result) {
    if (result != AGENT_OK) {
        status(ctx,
               result == AGENT_ERROR_LIMIT
                   ? "数量已达上限"
                   : "操作未完成：名称重复、房间不存在或参数无效",
               1);
        return;
    }
    smart_home_device_service_copy_state(&ctx->ui->app->device_service, &ctx->state);
    smart_home_lvgl_v9_refresh_views(ctx->ui);
    if (save_async(ctx) != AGENT_OK)
        status(ctx, "本次已生效，无法保存；请返回管理页重新保存", 1);
    else
        status(ctx, "正在保存房间配置…", 0);
}
static void clear_content(room_dialog_t *ctx) {
    lv_obj_clean(ctx->content);
    ctx->input = ctx->choice = ctx->keyboard = NULL;
    smart_home_device_service_copy_state(&ctx->ui->app->device_service, &ctx->state);
}
static void manager(room_dialog_t *ctx);
static void back_cb(lv_event_t *event) {
    manager(lv_event_get_user_data(event));
}
static void retry_cb(lv_event_t *event) {
    room_dialog_t *ctx = lv_event_get_user_data(event);
    if (ready(ctx))
        applied(ctx, AGENT_OK);
}
static void add_cb(lv_event_t *event) {
    room_dialog_t *ctx = lv_event_get_user_data(event);
    if (!ready(ctx))
        return;
    const char *name = lv_textarea_get_text(ctx->input);
    /* Preserve canonical IDs for the two built-in Chinese labels. */
    if (!strcmp(name, "客厅"))
        name = "living_room";
    if (!strcmp(name, "卧室"))
        name = "bedroom";
    int ret = smart_home_device_service_add_room(&ctx->ui->app->device_service, name);
    if (ret == AGENT_OK)
        manager(ctx);
    applied(ctx, ret);
}
static void preset_cb(lv_event_t *event) {
    room_dialog_t *ctx = lv_event_get_user_data(event);
    char name[32];
    lv_dropdown_get_selected_str(ctx->choice, name, sizeof(name));
    lv_textarea_set_text(ctx->input, name);
}
static void add_page_cb(lv_event_t *event) {
    room_dialog_t *ctx = lv_event_get_user_data(event);
    clear_content(ctx);
    text(ctx->content, "新增房间 · 最多 6 个，名称最多 7 个汉字或 23 字节", 0, 0, 16);
    ctx->input = lv_textarea_create(ctx->content);
    lv_obj_set_pos(ctx->input, 0, 34);
    lv_obj_set_size(ctx->input, 400, 46);
    lv_textarea_set_one_line(ctx->input, true);
    lv_obj_set_style_text_font(ctx->input, smart_home_lvgl_font(18), 0);
    lv_textarea_set_max_length(ctx->input, 23);
    lv_textarea_set_text(ctx->input, "书房");
    ctx->choice = lv_dropdown_create(ctx->content);
    smart_home_lvgl_style_dropdown(ctx->choice, 18);
    lv_obj_set_pos(ctx->choice, 430, 34);
    lv_obj_set_width(ctx->choice, 300);
    lv_dropdown_set_options(ctx->choice,
                            "书房\n阳台\n厨房\n餐厅\n儿童房\n客房\n客厅\n卧室");
    lv_obj_add_event_cb(ctx->choice, preset_cb, LV_EVENT_VALUE_CHANGED, ctx);
    ctx->keyboard = lv_keyboard_create(ctx->content);
    lv_obj_set_align(ctx->keyboard, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(ctx->keyboard, 0, 96);
    lv_obj_set_size(ctx->keyboard, 744, 202);
    lv_keyboard_set_textarea(ctx->keyboard, ctx->input);
    button(ctx->content, "返回", 0, 310, back_cb, ctx);
    button(ctx->content, "添加", 624, 310, add_cb, ctx);
}
static int targets(room_dialog_t *ctx, const char *exclude) {
    char options[SMART_HOME_MAX_ROOMS * (SMART_HOME_ROOM_NAME_SIZE + 1)] = "";
    int count = 0;
    for (int i = 0; i < ctx->state.room_count; i++) {
        if (exclude && !strcmp(exclude, ctx->state.rooms[i]))
            continue;
        if (count)
            strcat(options, "\n");
        strcat(options, smart_home_lvgl_room_display(ctx->state.rooms[i]));
        ctx->targets[count++] = i;
    }
    ctx->choice = lv_dropdown_create(ctx->content);
    smart_home_lvgl_style_dropdown(ctx->choice, 18);
    lv_obj_set_pos(ctx->choice, 0, 120);
    lv_obj_set_size(ctx->choice, 744, 48);
    lv_dropdown_set_options(ctx->choice, count ? options : "没有可用房间");
    if (!count)
        lv_obj_add_state(ctx->choice, LV_STATE_DISABLED);
    return count;
}
static void delete_confirm_cb(lv_event_t *event) {
    room_dialog_t *ctx = lv_event_get_user_data(event);
    if (!ready(ctx) || ctx->state.room_count <= 1)
        return;
    unsigned selected = lv_dropdown_get_selected(ctx->choice);
    if (selected >= (unsigned)(ctx->state.room_count - 1))
        return;
    char target[SMART_HOME_ROOM_NAME_SIZE];
    snprintf(target, sizeof target, "%s", ctx->state.rooms[ctx->targets[selected]]);
    char keys[SMART_HOME_ROOM_BINDINGS][SMART_HOME_ROOM_KEY_SIZE];
    size_t count = 0;
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (ctx->ui->app->miloco) {
        smart_home_miloco_device_t *devices =
            smart_home_bulk_alloc(sizeof(*devices) * SMART_HOME_MILOCO_MAX_DEVICES);
        if (!devices) {
            status(ctx, "内存不足，尚未删除房间", 1);
            return;
        }
        smart_home_device_service_copy_state(&ctx->ui->app->device_service,
                                             &ctx->state);
        size_t total = smart_home_miloco_list(ctx->ui->app->miloco, devices,
                                              SMART_HOME_MILOCO_MAX_DEVICES, NULL);
        for (size_t i = 0; i < total && i < SMART_HOME_MILOCO_MAX_DEVICES &&
                           count < SMART_HOME_ROOM_BINDINGS;
             i++) {
            char key[SMART_HOME_ROOM_KEY_SIZE];
            snprintf(key, sizeof key, "milo:%s", devices[i].did);
            const char *room = smart_home_device_room_override(&ctx->state, key);
            if (!room)
                room = devices[i].room;
            if (!strcmp(smart_home_lvgl_room_display(room),
                        smart_home_lvgl_room_display(ctx->source)))
                snprintf(keys[count++], SMART_HOME_ROOM_KEY_SIZE, "%s", key);
        }
        smart_home_bulk_free(devices);
    }
#endif
    int ret = smart_home_device_service_delete_room_with_keys(
        &ctx->ui->app->device_service, ctx->source, target, keys, count);
    if (ret == AGENT_OK)
        manager(ctx);
    applied(ctx, ret);
}
static void delete_page_cb(lv_event_t *event) {
    room_dialog_t *ctx = g_room;
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (!ctx || !ready(ctx) || index < 0 || index >= ctx->state.room_count)
        return;
    snprintf(ctx->source, sizeof ctx->source, "%s", ctx->state.rooms[index]);
    clear_content(ctx);
    char message[160];
    snprintf(message, sizeof message, "删除“%s”前，请选择设备迁往的房间。",
             smart_home_lvgl_room_display(ctx->source));
    text(ctx->content, message, 0, 8, 20);
    text(ctx->content, "本地设备和已发现的米家设备将一同迁移；米家云端房间不变。", 0,
         64, 16);
    int count = targets(ctx, ctx->source);
    button(ctx->content, "取消", 0, 310, back_cb, ctx);
    lv_obj_t *confirm =
        button(ctx->content, "确认删除", 624, 310, delete_confirm_cb, ctx);
    if (!count) {
        lv_obj_add_state(confirm, LV_STATE_DISABLED);
        status(ctx, "至少保留一个房间，无法删除最后一个房间", 1);
    }
}
static void manager(room_dialog_t *ctx) {
    clear_content(ctx);
    for (int i = 0; i < ctx->state.room_count; i++) {
        int count = 0;
        for (int j = 0; j < SMART_HOME_MAX_DEVICES; j++)
            if (ctx->state.devices[j].used &&
                !strcmp(ctx->state.devices[j].room, ctx->state.rooms[i]))
                count++;
        for (int j = 0; j < SMART_HOME_ROOM_BINDINGS; j++)
            if (ctx->state.room_bindings[j].key[0] &&
                !strcmp(ctx->state.room_bindings[j].room, ctx->state.rooms[i]))
                count++;
        char line[96];
        snprintf(line, sizeof line, "%s  ·  %d 台已分配设备",
                 smart_home_lvgl_room_display(ctx->state.rooms[i]), count);
        text(ctx->content, line, 0, i * 44 + 8, 18);
        button(ctx->content, "删除", 624, i * 44, delete_page_cb, (void *)(intptr_t)i);
    }
    lv_obj_t *add = button(ctx->content, "新增房间", 0, 310, add_page_cb, ctx);
    if (ctx->state.room_count >= SMART_HOME_MAX_ROOMS)
        lv_obj_add_state(add, LV_STATE_DISABLED);
    button(ctx->content, "重新保存", 624, 310, retry_cb, ctx);
}
static void assign_cb(lv_event_t *event) {
    room_dialog_t *ctx = lv_event_get_user_data(event);
    if (!ready(ctx))
        return;
    unsigned selected = lv_dropdown_get_selected(ctx->choice);
    if (selected >= (unsigned)ctx->state.room_count)
        return;
    applied(ctx, smart_home_device_service_assign_room(
                     &ctx->ui->app->device_service, ctx->key,
                     ctx->state.rooms[ctx->targets[selected]]));
}
static room_dialog_t *open_dialog(smart_home_lvgl_t *ui, const char *title) {
    smart_home_lvgl_room_dialog_close();
    if (!ui || !ui->app || !ui->screen_home ||
        !ui->app->device_service.mutex_initialized)
        return NULL;
    room_dialog_t *ctx = smart_home_bulk_alloc(sizeof(*ctx));
    if (!ctx)
        return NULL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->ui = ui;
    g_room = ctx;
    ctx->mask = lv_obj_create(ui->screen_home);
    lv_obj_remove_style_all(ctx->mask);
    lv_obj_set_size(ctx->mask, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(ctx->mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ctx->mask, LV_OPA_30, 0);
    lv_obj_add_event_cb(ctx->mask, close_cb, LV_EVENT_CLICKED, NULL);
    ctx->panel = lv_obj_create(ctx->mask);
    lv_obj_remove_style_all(ctx->panel);
    lv_obj_set_size(ctx->panel, 840, 480);
    lv_obj_center(ctx->panel);
    smart_home_lvgl_card_style(ctx->panel);
    lv_obj_set_style_radius(ctx->panel, 18, 0);
    lv_obj_set_style_pad_all(ctx->panel, 24, 0);
    lv_obj_add_event_cb(ctx->panel, stop_cb, LV_EVENT_CLICKED, NULL);
    text(ctx->panel, title, 0, 0, 24);
    button(ctx->panel, "关闭", 672, 0, close_cb, NULL);
    ctx->content = lv_obj_create(ctx->panel);
    lv_obj_remove_style_all(ctx->content);
    lv_obj_set_pos(ctx->content, 0, 58);
    lv_obj_set_size(ctx->content, 744, 350);
    lv_obj_clear_flag(ctx->content, LV_OBJ_FLAG_SCROLLABLE);
    ctx->status = text(ctx->panel, "", 0, 410, 14);
    lv_obj_set_width(ctx->status, 744);
    ctx->timer = lv_timer_create(poll, 200, ctx);
    lv_obj_add_event_cb(ctx->mask, deleted, LV_EVENT_DELETE, ctx);
    int result;
    ctx->was_busy = save_status(&result);
    if (g_save_attempted && !ctx->was_busy)
        status(ctx,
               result == AGENT_OK ? "房间配置已保存"
                                  : "此前保存失败，可在管理页重新保存",
               result != AGENT_OK);
    return ctx;
}
void smart_home_lvgl_room_dialog_open(smart_home_lvgl_t *ui) {
    room_dialog_t *ctx = open_dialog(ui, "房间管理");
    if (ctx)
        manager(ctx);
}
void smart_home_lvgl_room_assign_open(smart_home_lvgl_t *ui,
                                      const smart_home_v9_device_view_t *view) {
    if (!view || view->source == SMART_HOME_V9_SOURCE_DEMO)
        return;
    room_dialog_t *ctx = open_dialog(ui, "分配房间");
    if (!ctx)
        return;
    clear_content(ctx);
    snprintf(ctx->key, sizeof ctx->key, "%s", view->key);
    text(ctx->content, view->name, 0, 8, 20);
    text(ctx->content,
         view->source == SMART_HOME_V9_SOURCE_MILOCO
             ? "仅调整本屏房间，不修改米家云端房间。"
             : "将设备分配到以下房间：",
         0, 64, 16);
    targets(ctx, NULL);
    for (int i = 0; i < ctx->state.room_count; i++)
        if (!strcmp(ctx->state.rooms[i], view->room_id))
            lv_dropdown_set_selected(ctx->choice, i);
    button(ctx->content, "确认分配", 624, 310, assign_cb, ctx);
}
