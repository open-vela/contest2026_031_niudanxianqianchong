#include "smart_home_lvgl_room_dialog.c"
#include <assert.h>
#include <unistd.h>
#include <stdatomic.h>
static int refreshes;
static atomic_int fail_save, hold_save;
int __real_smart_home_config_state_save(const cJSON *root);
int __wrap_smart_home_config_state_save(const cJSON *root) {
    while (atomic_load(&hold_save))
        usleep(1000);
    return atomic_load(&fail_save) ? AGENT_ERROR
                                   : __real_smart_home_config_state_save(root);
}
void *smart_home_bulk_alloc(size_t n) {
    return malloc(n);
}
void smart_home_bulk_free(void *p) {
    free(p);
}
void smart_home_lvgl_v9_refresh_views(smart_home_lvgl_t *ui) {
    (void)ui;
    refreshes++;
}
size_t smart_home_miloco_list(const smart_home_miloco_t *service,
                              smart_home_miloco_device_t *out, size_t capacity,
                              uint32_t *revision) {
    (void)service;
    (void)revision;
    if (!out || !capacity)
        return 0;
    memset(out, 0, sizeof(*out));
    strcpy(out->did, "2222");
    strcpy(out->room, "书房");
    return 1;
}
static uint16_t pixels[1024 * 600];
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *p) {
    (void)a;
    (void)p;
    lv_display_flush_ready(d);
}
static lv_obj_t *find_button(lv_obj_t *root, const char *value) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (lv_obj_check_type(child, &lv_label_class) &&
            !strcmp(lv_label_get_text(child), value))
            return lv_obj_get_parent(child);
        lv_obj_t *found = find_button(child, value);
        if (found)
            return found;
    }
    return NULL;
}
static void click(const char *name) {
    lv_obj_t *btn = find_button(g_room->content, name);
    assert(btn && !lv_obj_has_state(btn, LV_STATE_DISABLED));
    lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
}
static void wait_saved(void) {
    for (int i = 0; i < 3000 && save_status(NULL); i++)
        usleep(1000);
    assert(!save_status(NULL));
    if (g_room)
        poll(g_room->timer);
}
static void capture(const char *key) {
    const char *path = getenv(key);
    if (!path)
        return;
    lv_refr_now(NULL);
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n1024 600\n255\n");
    for (size_t i = 0; i < 1024 * 600; i++) {
        unsigned p = pixels[i];
        uint8_t rgb[] = {((p >> 11) & 31) * 255 / 31, ((p >> 5) & 63) * 255 / 63,
                         (p & 31) * 255 / 31};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}
int main(void) {
    unlink(SMART_HOME_CONFIG_STATE_PATH);
    lv_init();
    lv_display_t *d = lv_display_create(1024, 600);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, pixels, NULL, sizeof pixels,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush);
    assert(!smart_home_lvgl_style_init());
    smart_home_agent_app_t app = {0};
    smart_home_lvgl_t ui = {.app = &app};
    ui.screen_home = lv_obj_create(NULL);
    lv_screen_load(ui.screen_home);
    smart_home_device_init(&app.device_state);
    assert(!smart_home_device_service_init(&app.device_service, &app.device_state));
    smart_home_lvgl_room_dialog_open(&ui);
    assert(g_room);
    click("新增房间");
    capture("SMART_HOME_ROOM_ADD_CAPTURE");
    assert(!strcmp(lv_textarea_get_text(g_room->input), "书房"));
    click("添加");
    wait_saved();
    assert(app.device_state.room_count == 3);
    assert(strstr(lv_label_get_text(g_room->status), "已保存"));
    cJSON *json = NULL;
    assert(!smart_home_config_state_load(&json, NULL, 0));
    smart_home_state_t restored;
    assert(!smart_home_device_state_from_json(&restored, json));
    cJSON_Delete(json);
    assert(smart_home_room_index(&restored, "书房") == 2);
    click("新增房间");
    uint32_t revision = smart_home_device_service_revision(&app.device_service);
    click("添加");
    assert(revision == smart_home_device_service_revision(&app.device_service));
    assert(!save_status(NULL));
    assert(smart_home_room_add(&restored, "bad\"name") == AGENT_ERROR_INVALID);
    assert(smart_home_room_add(&restored, "\xe4\xb8") == AGENT_ERROR_INVALID);
    assert(!smart_home_room_add(&restored, "儿童房"));
    assert(!smart_home_room_add(&restored, "餐厅"));
    assert(!smart_home_room_add(&restored, "客房"));
    assert(smart_home_room_add(&restored, "车库") == AGENT_ERROR_LIMIT);
    smart_home_v9_device_view_t view = {.source = SMART_HOME_V9_SOURCE_LOCAL};
    strcpy(view.key, "local:1");
    strcpy(view.name, "吸顶灯");
    smart_home_lvgl_room_assign_open(&ui, &view);
    lv_dropdown_set_selected(g_room->choice, 2);
    click("确认分配");
    wait_saved();
    assert(!strcmp(app.device_state.devices[0].room, "书房"));
    view.source = SMART_HOME_V9_SOURCE_MILOCO;
    strcpy(view.key, "milo:123456789");
    strcpy(view.name, "米家风扇");
    smart_home_lvgl_room_assign_open(&ui, &view);
    lv_dropdown_set_selected(g_room->choice, 2);
    capture("SMART_HOME_ROOM_ASSIGN_CAPTURE");
    click("确认分配");
    wait_saved();
    assert(
        !strcmp(smart_home_device_room_override(&app.device_state, view.key), "书房"));
    smart_home_lvgl_room_dialog_open(&ui);
    capture("SMART_HOME_ROOM_MANAGER_CAPTURE");
    lv_obj_t *del = lv_obj_get_child(g_room->content, 5); /* third row delete */
    app.miloco = (smart_home_miloco_t *)1;
    lv_obj_send_event(del, LV_EVENT_CLICKED, NULL);
    assert(!strcmp(g_room->source, "书房"));
    lv_dropdown_set_selected(g_room->choice, 1); /* bedroom */
    capture("SMART_HOME_ROOM_DELETE_CAPTURE");
    click("确认删除");
    wait_saved();
    assert(app.device_state.room_count == 2);
    assert(!strcmp(smart_home_device_room_override(&app.device_state, "milo:2222"),
                   "bedroom"));
    assert(!strcmp(app.device_state.devices[0].room, "bedroom"));
    assert(!strcmp(smart_home_device_room_override(&app.device_state, view.key),
                   "bedroom"));
    smart_home_state_t transaction_before = app.device_state;
    uint32_t transaction_revision =
        smart_home_device_service_revision(&app.device_service);
    const char bad_keys[2][SMART_HOME_ROOM_KEY_SIZE] = {"milo:new", "invalid"};
    assert(smart_home_device_service_delete_room_with_keys(
               &app.device_service, "living_room", "bedroom", bad_keys, 2) != AGENT_OK);
    assert(!memcmp(&transaction_before, &app.device_state, sizeof transaction_before));
    assert(transaction_revision ==
           smart_home_device_service_revision(&app.device_service));
    /* Save failure remains visible, retry writes the current complete state. */
    atomic_store(&fail_save, 1);
    click("重新保存");
    wait_saved();
    assert(strstr(lv_label_get_text(g_room->status), "保存失败"));
    atomic_store(&fail_save, 0);
    click("重新保存");
    wait_saved();
    assert(strstr(lv_label_get_text(g_room->status), "已保存"));
    /* Closing/reopening during I/O never leaves UI/app pointers in the writer. */
    atomic_store(&hold_save, 1);
    click("重新保存");
    smart_home_lvgl_room_dialog_close();
    assert(!g_room && save_status(NULL));
    smart_home_lvgl_room_dialog_open(&ui);
    click("新增房间");
    click("添加");
    assert(app.device_state.room_count == 2); /* busy rejects mutation */
    lv_obj_delete(ui.screen_home);
    assert(!g_room);
    atomic_store(&hold_save, 0);
    wait_saved();
    /* Stable non-contiguous device IDs survive persistence, including light values. */
    app.device_state.devices[0].id = 7;
    app.device_state.next_device_id = 8;
    json = smart_home_device_state_to_json(&app.device_state);
    assert(!smart_home_device_state_from_json(&restored, json));
    assert(smart_home_device_find_by_id(&restored, 7));
    cJSON_Delete(json);
    json = smart_home_device_state_to_json(&restored);
    cJSON *binding = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "room_bindings"), 0);
    cJSON_SetValuestring(cJSON_GetObjectItem(binding, "room"), "不存在");
    smart_home_state_t before = restored;
    assert(smart_home_device_state_from_json(&restored, json) != AGENT_OK);
    assert(!memcmp(&before, &restored, sizeof before));
    cJSON_Delete(json);
    assert(!smart_home_room_delete_and_reassign(&restored, "living_room", "bedroom"));
    assert(smart_home_room_delete_and_reassign(&restored, "bedroom", "bedroom") !=
           AGENT_OK);
    assert(restored.room_count == 1 && refreshes > 0);
    smart_home_device_service_deinit(&app.device_service);
    smart_home_lvgl_style_deinit();
    lv_deinit();
    puts("PASS: room widgets, UTF-8, capacity, migration, stable-key roundtrip, "
         "background save/retry/close");
    return 0;
}
