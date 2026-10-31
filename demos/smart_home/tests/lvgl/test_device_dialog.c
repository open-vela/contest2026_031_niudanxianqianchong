#include "smart_home_lvgl_device_dialog.c"
#include "../../src/miloco/smart_home_miloco_specscan.h"
#include <assert.h>
static smart_home_miloco_device_t remote;
static int online = 1, submit_result, completed, completion_result;
static uint32_t request_id;
static int32_t submitted_value;
void *smart_home_bulk_alloc(size_t n) {
    return malloc(n);
}
void smart_home_bulk_free(void *p) {
    free(p);
}
void smart_home_lvgl_v9_refresh_views(smart_home_lvgl_t *ui) {
    (void)ui;
}
bool smart_home_miloco_reachable(const smart_home_miloco_t *service) {
    (void)service;
    return online;
}
bool smart_home_miloco_copy_device(const smart_home_miloco_t *service, const char *did,
                                   smart_home_miloco_device_t *out) {
    (void)service;
    if (strcmp(did, remote.did))
        return false;
    *out = remote;
    return true;
}
int smart_home_miloco_submit_control_tracked(smart_home_miloco_t *service,
                                             const char *did, const char *iid,
                                             const char *op, int32_t value,
                                             uint32_t *id) {
    (void)service;
    assert(!strcmp(did, remote.did));
    assert(!strcmp(iid, remote.controls[0].iid));
    assert(!strcmp(op, remote.controls[0].type == SMART_HOME_MILOCO_CTRL_ACTION
                           ? "action"
                           : "set"));
    submitted_value = value;
    *id = ++request_id;
    return submit_result;
}
bool smart_home_miloco_control_result(const smart_home_miloco_t *service, uint32_t id,
                                      int *result) {
    (void)service;
    if (!completed || id != request_id)
        return false;
    *result = completion_result;
    return true;
}
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *p) {
    (void)a;
    (void)p;
    lv_display_flush_ready(d);
}
static void test_ranges(void) {
    const char *json =
        "{\"code\":0,\"data\":{\"did\":\"test\",\"spec\":{\"prop.2.3\":{\"writeable\":"
        "true,\"format\":\"uint8\",\"type_name\":\"brightness\",\"description\":"
        "\"亮度\",\"value_range\":[1,100,1]}}}}";
    smart_home_miloco_control_t controls[8];
    uint8_t count = 0;
    bool power = false;
    char did[24];
    smart_home_miloco_category_t cat;
    assert(!smart_home_miloco_specscan(json, did, sizeof did, &cat, controls, 8, &count,
                                       &power));
    assert(count == 1 && controls[0].type == SMART_HOME_MILOCO_CTRL_RANGE);
    assert(controls[0].range.minimum == 1 && controls[0].range.maximum == 100 &&
           controls[0].range.step == 1);
    puts("PASS: gateway integer range parsed without losing its bounds/step");
}
int main(void) {
    test_ranges();
    lv_init();
    lv_display_t *display = lv_display_create(1024, 600);
    static uint16_t pixels[1024 * 600];
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof pixels,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, flush);
    assert(!smart_home_lvgl_style_init());
    smart_home_agent_app_t app = {0};
    smart_home_lvgl_t ui = {.app = &app};
    ui.screen_home = lv_obj_create(NULL);
    lv_screen_load(ui.screen_home);
    smart_home_device_init(&app.device_state);
    assert(smart_home_device_service_init(&app.device_service, &app.device_state) ==
           AGENT_OK);
    int second;
    assert(smart_home_device_service_add(&app.device_service, "living_room",
                                         "Second light", SMART_HOME_DEVICE_LIGHT,
                                         &second) == AGENT_OK);
    int first = app.device_state.devices[0].brightness;
    smart_home_v9_device_view_t view = {.source = SMART_HOME_V9_SOURCE_LOCAL,
                                        .kind = SMART_HOME_V9_KIND_LIGHT};
    snprintf(view.key, sizeof view.key, "local:%d", second);
    strcpy(view.name, "第二盏灯");
    smart_home_lvgl_device_dialog_open(&ui, &view);
    assert(g_dialog && g_dialog->count == 2);
    dialog_control_t *c = &g_dialog->control[1];
    lv_slider_set_value(c->widget, 37, LV_ANIM_OFF);
    lv_obj_send_event(c->widget, LV_EVENT_RELEASED, NULL);
    assert(smart_home_device_find_by_id(&app.device_state, second)->brightness == 37);
    assert(app.device_state.devices[0].brightness == first);
    assert(smart_home_device_service_revision(&app.device_service) == 2);
    smart_home_lvgl_device_dialog_close();
    assert(!g_dialog);
    view.kind = SMART_HOME_V9_KIND_AC;
    snprintf(view.key, sizeof view.key, "local:3");
    smart_home_lvgl_device_dialog_open(&ui, &view);
    assert(g_dialog->count == 4);
    lv_dropdown_set_selected(g_dialog->control[2].widget, 4);
    lv_obj_send_event(g_dialog->control[2].widget, LV_EVENT_VALUE_CHANGED, NULL);
    assert(smart_home_device_find_by_id(&app.device_state, 3)->ac_mode == 4);
    smart_home_lvgl_device_dialog_close();
    puts("PASS: local dialogs address the selected device ID and preserve peer "
         "devices; AC automatic mode is selectable");
    app.miloco = (smart_home_miloco_t *)&remote;
    strcpy(remote.did, "test");
    remote.online = true;
    remote.control_count = 1;
    strcpy(remote.controls[0].iid, "prop.2.3");
    strcpy(remote.controls[0].desc, "亮度");
    remote.controls[0].type = SMART_HOME_MILOCO_CTRL_RANGE;
    remote.controls[0].range.minimum = 0;
    remote.controls[0].range.maximum = 100;
    remote.controls[0].range.step = 5;
    remote.controls[0].value = 20;
    view.source = SMART_HOME_V9_SOURCE_MILOCO;
    view.kind = SMART_HOME_V9_KIND_LIGHT;
    strcpy(view.key, "milo:test");
    smart_home_lvgl_device_dialog_open(&ui, &view);
    c = &g_dialog->control[0];
    lv_slider_set_value(c->widget, 53, LV_ANIM_OFF);
    lv_obj_send_event(c->widget, LV_EVENT_RELEASED, NULL);
    assert(submitted_value == 50 && c->pending);
    completed = 1;
    completion_result = AGENT_OK;
    poll(g_dialog->timer);
    assert(c->pending);
    remote.controls[0].value = 50;
    poll(g_dialog->timer);
    assert(!c->pending && c->confirmed == 50);
    completed = 0;
    lv_slider_set_value(c->widget, 70, LV_ANIM_OFF);
    lv_obj_send_event(c->widget, LV_EVENT_RELEASED, NULL);
    completed = 1;
    completion_result = AGENT_ERROR_IO;
    poll(g_dialog->timer);
    assert(!c->pending && lv_slider_get_value(c->widget) == 50);
    completed = 0;
    lv_slider_set_value(c->widget, 80, LV_ANIM_OFF);
    lv_obj_send_event(c->widget, LV_EVENT_RELEASED, NULL);
    lv_tick_inc(10001);
    poll(g_dialog->timer);
    assert(!c->pending && lv_slider_get_value(c->widget) == 50);
    submit_result = AGENT_ERROR_LIMIT;
    lv_slider_set_value(c->widget, 90, LV_ANIM_OFF);
    lv_obj_send_event(c->widget, LV_EVENT_RELEASED, NULL);
    assert(!c->pending && lv_slider_get_value(c->widget) == 50);
    online = 0;
    poll(g_dialog->timer);
    assert(lv_obj_has_state(c->widget, LV_STATE_DISABLED));
    smart_home_lvgl_device_dialog_close();
    online = 1;
    submit_result = 0;
    completed = 0;
    remote.controls[0].type = SMART_HOME_MILOCO_CTRL_ACTION;
    strcpy(remote.controls[0].iid, "action.2.1");
    view.kind = SMART_HOME_V9_KIND_ROBOT;
    smart_home_lvgl_device_dialog_open(&ui, &view);
    c = &g_dialog->control[0];
    lv_obj_send_event(c->widget, LV_EVENT_CLICKED, NULL);
    assert(c->pending);
    completed = 1;
    completion_result = AGENT_OK;
    poll(g_dialog->timer);
    assert(!c->pending);
    assert(strstr(lv_label_get_text(g_dialog->status), "已确认"));
    smart_home_lvgl_device_dialog_close();
    remote.controls[0].type = SMART_HOME_MILOCO_CTRL_ENUM;
    strcpy(remote.controls[0].iid, "prop.2.2");
    remote.controls[0].option_count = 2;
    strcpy(remote.controls[0].options[0].name, "标准");
    strcpy(remote.controls[0].options[1].name, "强力");
    remote.controls[0].options[0].value = 10;
    remote.controls[0].options[1].value = 20;
    remote.controls[0].value = 10;
    completed = 0;
    smart_home_lvgl_device_dialog_open(&ui, &view);
    c = &g_dialog->control[0];
    lv_dropdown_set_selected(c->widget, 1);
    lv_obj_send_event(c->widget, LV_EVENT_VALUE_CHANGED, NULL);
    assert(submitted_value == 20 && c->pending);
    remote.controls[0].value = 20;
    completed = 1;
    poll(g_dialog->timer);
    assert(!c->pending);
    lv_obj_delete(ui.screen_home);
    assert(!g_dialog);
    smart_home_device_service_deinit(&app.device_service);
    smart_home_lvgl_style_deinit();
    puts("PASS: remote range quantization, separate ACK/readback, execution error, "
         "timeout, queue-full rollback, offline and parent deletion");
    return 0;
}
