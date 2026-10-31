/* Exercise the actual panel helpers against LVGL, with a snapshot-only service. */
#include "smart_home_lvgl_panel.c"
#include "smart_home_v9_device_view.h"
#include <assert.h>
#include <stdlib.h>

static uint16_t pixels[1024 * 600];
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{ (void)area; (void)data; lv_display_flush_ready(display); }
static void capture_screen(lv_obj_t *screen, const char *env)
{
    const char *path = getenv(env);
    if (!path || !screen) return;
    lv_screen_load(screen);
    lv_refr_now(NULL);
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n1024 600\n255\n");
    for (size_t i = 0; i < sizeof(pixels) / sizeof(pixels[0]); i++) {
        unsigned p = pixels[i];
        uint8_t rgb[] = {((p >> 11) & 31) * 255 / 31,
                         ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}

static void capture_to(smart_home_lvgl_t *ui, const char *env)
{
    capture_screen(ui->screen_home, env);
}

static void capture_home(smart_home_lvgl_t *ui)
{
    capture_to(ui, "SMART_HOME_LVGL_CAPTURE");
}

static smart_home_miloco_device_t snapshot[SMART_HOME_MILOCO_MAX_DEVICES];
static size_t snapshot_count;
static uint32_t snapshot_revision = 1;
static unsigned snapshot_reads;
size_t smart_home_miloco_list(const smart_home_miloco_t *service,
                             smart_home_miloco_device_t *out,
                             size_t capacity, uint32_t *revision)
{
    (void)service;
    if (revision) *revision = snapshot_revision;
    if (out) snapshot_reads++;
    size_t n = capacity < snapshot_count ? capacity : snapshot_count;
    if (out) memcpy(out, snapshot, n * sizeof(*out));
    return out ? n : snapshot_count;
}

#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
static caddons_node_info_t node_snapshot[CAGENT_ADDONS_MAX_NODES];
static size_t node_count;
size_t smart_home_node_gateway_list(const smart_home_node_gateway_t *gateway,
    caddons_node_info_t *out, size_t capacity, uint32_t *revision)
{
    (void)gateway;
    if (revision) *revision = 1;
    size_t n = capacity < node_count ? capacity : node_count;
    if (out) memcpy(out, node_snapshot, n * sizeof(*out));
    return out ? n : node_count;
}
#endif

static int submit_result;
static int submissions;
static bool gateway_online = true;
void *smart_home_bulk_alloc(size_t size) { return malloc(size); }
void smart_home_bulk_free(void *ptr) { free(ptr); }
bool smart_home_miloco_reachable(const smart_home_miloco_t *service)
{ (void)service; return gateway_online; }
int smart_home_miloco_submit_power(smart_home_miloco_t *service,
                                   const char *did, bool on)
{ (void)service; (void)did; (void)on; submissions++; return submit_result; }
int smart_home_miloco_submit_control(smart_home_miloco_t *service,
    const char *did, const char *iid, const char *operation, int32_t value)
{
    (void)service; (void)did; (void)iid; (void)operation; (void)value;
    submissions++;
    return submit_result;
}

static smart_home_miloco_weather_t weather;
bool smart_home_miloco_get_weather(const smart_home_miloco_t *service,
                                   smart_home_miloco_weather_t *out)
{ (void)service; *out = weather; return weather.valid; }
bool smart_home_miloco_bound(const smart_home_miloco_t *service)
{ (void)service; return true; }
void smart_home_lvgl_security_camera_stop(smart_home_lvgl_t *ui) { (void)ui; }
void smart_home_lvgl_refresh_miloco_screen(smart_home_lvgl_t *ui) { (void)ui; }
void smart_home_lvgl_chat_send_text(smart_home_lvgl_t *ui, const char *text)
{ (void)ui; (void)text; }

static lv_point_t pointer_point;
static lv_indev_state_t pointer_state;
static void read_pointer(lv_indev_t *input, lv_indev_data_t *data)
{ (void)input; data->point = pointer_point; data->state = pointer_state; }
static void touch(lv_indev_t *input, lv_obj_t *target, lv_indev_state_t state)
{
    lv_area_t bounds;
    lv_obj_get_coords(target, &bounds);
    pointer_point.x = (bounds.x1 + bounds.x2) / 2;
    pointer_point.y = (bounds.y1 + bounds.y2) / 2;
    pointer_state = state;
    lv_tick_inc(40);
    lv_indev_read(input);
}

static void test_press(smart_home_lvgl_t *ui)
{
    lv_obj_t *probe = lv_button_create(ui->screen_home);
    lv_obj_remove_style_all(probe);
    smart_home_lvgl_set_bg(probe, SMART_HOME_UI_COLOR_SURFACE);
    smart_home_lvgl_press_feedback(probe);
    lv_obj_add_state(probe, LV_STATE_PRESSED);
    assert(lv_obj_get_style_recolor_opa(probe, 0) == LV_OPA_20);
    smart_home_lvgl_set_bg(probe, SMART_HOME_UI_COLOR_PRIMARY);
    lv_obj_remove_state(probe, LV_STATE_PRESSED);
    assert(lv_color_eq(lv_obj_get_style_bg_color(probe, 0),
                       SMART_HOME_UI_COLOR_PRIMARY));
    lv_obj_add_state(probe, LV_STATE_DISABLED | LV_STATE_PRESSED);
    assert(lv_obj_get_style_recolor_opa(probe, 0) == LV_OPA_TRANSP);
    assert(lv_obj_get_style_opa(probe, 0) == LV_OPA_50);
    lv_obj_delete(probe);

    ui->screen_chat = lv_obj_create(NULL);
    ui->active_tab = SMART_HOME_TAB_HOME;
    lv_screen_load(ui->screen_home);
    lv_obj_update_layout(ui->screen_home);
    lv_obj_t *bar = lv_obj_get_child(ui->screen_home, -1);
    lv_obj_t *nav = lv_obj_get_child(bar, 0);
    lv_obj_t *button = lv_obj_get_child(nav, SMART_HOME_TAB_CHAT);
    lv_obj_t *icon_content = lv_obj_get_child(button, 0);
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, read_pointer);
    int before = submissions;
    touch(input, icon_content, LV_INDEV_STATE_PRESSED);
    assert(lv_obj_has_state(button, LV_STATE_PRESSED));
    assert(lv_obj_get_style_bg_opa(button, 0) == LV_OPA_COVER);
    assert(ui->active_tab == SMART_HOME_TAB_HOME && submissions == before);
    touch(input, icon_content, LV_INDEV_STATE_RELEASED);
    assert(!lv_obj_has_state(button, LV_STATE_PRESSED));
    assert(ui->active_tab == SMART_HOME_TAB_CHAT);
    assert(lv_screen_active() == ui->screen_chat && submissions == before);

    lv_screen_load(ui->screen_home);
    ui->active_tab = SMART_HOME_TAB_HOME;
    lv_obj_t *scene = lv_obj_get_child(ui->screen_home, 2);
    lv_refr_now(NULL);
    lv_area_t bounds;
    lv_obj_get_coords(scene, &bounds);
    size_t sample = (bounds.y1 + 90) * 1024 + bounds.x1 + 30;
    uint16_t normal = pixels[sample];
    touch(input, scene, LV_INDEV_STATE_PRESSED);
    lv_refr_now(NULL);
    assert(lv_obj_has_state(scene, LV_STATE_PRESSED));
    assert(pixels[sample] != normal && submissions == before);
    touch(input, scene, LV_INDEV_STATE_RELEASED);
    assert(!lv_obj_has_state(scene, LV_STATE_PRESSED));
    assert(ui->scene_popup && !lv_obj_has_flag(ui->scene_popup, LV_OBJ_FLAG_HIDDEN));
    lv_obj_add_flag(ui->scene_popup, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);
    assert(pixels[sample] == normal && submissions == before);
    /* Gesture cancellation restores the navigation parent as well. */
    lv_obj_send_event(icon_content, LV_EVENT_PRESSED, NULL);
    assert(lv_obj_has_state(button, LV_STATE_PRESSED));
    lv_obj_send_event(icon_content, LV_EVENT_PRESS_LOST, NULL);
    assert(!lv_obj_has_state(button, LV_STATE_PRESSED));
    lv_indev_delete(input);
    lv_obj_delete(ui->screen_chat);
    ui->screen_chat = NULL;
    puts("PASS: pointer press/release, RGB565 feedback, nav hit target/cancel, disabled and selected states");
}

static void test_home(smart_home_lvgl_t *ui)
{
    snapshot_count = 2;
    snapshot[0].online = true;
    snapshot[1].online = false;
    ui->device_state = &ui->app->device_state;
    smart_home_lvgl_build_home_screen(ui);
    smart_home_lvgl_build_screensaver_screen(ui);
    assert(!strcmp(lv_label_get_text(ui->home_status_label),
                   "在线 1 / 共 2 台\n米家设备状态"));
    assert(!strcmp(lv_label_get_text(ui->home_env_label), "天气数据暂不可用"));
    assert(!strcmp(lv_label_get_text(ui->home_weather_temp_label), "--°"));
    weather.valid = true;
    strcpy(weather.city, "Beijing");
    strcpy(weather.condition_cn, "小雨");
    weather.temperature = 18;
    weather.humidity = 72;
    smart_home_lvgl_refresh_home(ui);
    assert(!strcmp(lv_label_get_text(ui->home_weather_city_label), "Beijing"));
    assert(!strcmp(lv_label_get_text(ui->screensaver_weather_label), "小雨 · 18°C"));
    weather.valid = false;
    smart_home_lvgl_refresh_home(ui);
    assert(!strcmp(lv_label_get_text(ui->screensaver_weather_label), "天气数据暂不可用"));
    gateway_online = false;
    smart_home_lvgl_refresh_home(ui);
    assert(strstr(lv_label_get_text(ui->home_status_label), "未知"));
    gateway_online = true;
    smart_home_lvgl_refresh_home(ui);

    smart_home_lvgl_update_clock_at(ui, 1574985600); /* board's 2019 boot date */
    assert(!strcmp(lv_label_get_text(ui->home_time_label), "--:--"));
    assert(!strcmp(lv_label_get_text(ui->screensaver_date_label), "时间待同步"));
    struct tm moment = {.tm_year = 126, .tm_mon = 9, .tm_mday = 26,
                        .tm_hour = 15, .tm_min = 59};
    time_t utc = timegm(&moment);
    smart_home_lvgl_update_clock_at(ui, utc);
    assert(!strcmp(lv_label_get_text(ui->home_time_label), "23:59"));
    assert(strstr(lv_label_get_text(ui->screensaver_date_label), "10月26日"));
    smart_home_lvgl_update_clock_at(ui, utc + 60);
    assert(!strcmp(lv_label_get_text(ui->screensaver_time_label), "00:00"));
    assert(strstr(lv_label_get_text(ui->screensaver_date_label), "10月27日"));
    assert(!strcmp(lv_label_get_text(ui->home_date_label),
                   lv_label_get_text(ui->screensaver_date_label)));
    lv_obj_update_layout(ui->screen_home);
    lv_obj_t *card = lv_obj_get_parent(ui->home_status_label);
    lv_obj_t *title = lv_obj_get_child(card, 1);
    lv_area_t title_area, value_area;
    lv_obj_get_coords(title, &title_area);
    lv_obj_get_coords(ui->home_status_label, &value_area);
    assert(title_area.y2 < value_area.y1);
    test_press(ui);
    capture_home(ui);
    lv_timer_delete(ui->home_time_timer);
    ui->home_time_timer = NULL;
    lv_obj_delete(ui->screen_home);
    ui->screen_home = NULL;

    /* V9 merged home: six cards, room filter and final card art. */
    ui->topbar_clock_count = 0;
    ui->topbar_wifi_icon_count = 0;
    ui->topbar_camera_icon_count = 0;
    smart_home_lvgl_build_home_screen_v9(ui);
    assert(ui->screen_home != NULL);
    assert(ui->home_weather_condition_label != NULL);
    assert(smart_home_lvgl_v9_visible_card_count() == 6);
    assert(smart_home_lvgl_v9_select_room(3) == 0); /* 书房: empty */
    assert(smart_home_lvgl_v9_select_room(1) == 4); /* 客厅: light/ac/robot/camera */
    assert(smart_home_lvgl_v9_select_room(0) == 6);
    capture_to(ui, "SMART_HOME_V9_CAPTURE");

    smart_home_lvgl_build_settings_screen_v9(ui);
    assert(ui->screen_settings_v9 != NULL);
    assert(smart_home_lvgl_v9_settings_category_count() == 6);
    assert(smart_home_lvgl_v9_settings_select_category(2) == 2);
    assert(smart_home_lvgl_v9_settings_select_category(99) == -1);
    capture_screen(ui->screen_settings_v9, "SMART_HOME_V9_SETTINGS_CAPTURE");
    lv_obj_delete(ui->screen_settings_v9);
    ui->screen_settings_v9 = NULL;

    lv_timer_delete(ui->home_time_timer);
    ui->home_time_timer = NULL;
    lv_obj_delete(ui->screen_home);
    ui->screen_home = NULL;
    lv_obj_delete(ui->screen_screensaver);
    puts("PASS: cached weather/city, truthful counts, unknown status, midnight date, V9 home");
}

static void test_feedback(smart_home_lvgl_t *ui, lv_obj_t *screen)
{
    snapshot_count = 1;
    memset(snapshot, 0, sizeof(snapshot));
    strcpy(snapshot[0].did, "device-1");
    snapshot[0].online = true;
    snapshot[0].controllable = true;
    snapshot[0].power_on = true;
    ui->screen_panel = screen;
    ui->panel_feedback_label = lv_label_create(screen);
    lv_obj_t *sw = lv_switch_create(screen);
    lv_obj_set_user_data(sw, snapshot[0].did);
    lv_obj_add_event_cb(sw, miloco_switch_cb, LV_EVENT_VALUE_CHANGED, ui);
    submit_result = AGENT_ERROR_INVALID;
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
    assert(lv_obj_has_state(sw, LV_STATE_CHECKED)); /* failed OFF restores ON */
    assert(strstr(lv_label_get_text(ui->panel_feedback_label), "未能提交"));
    snapshot[0].power_on = false;
    lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
    assert(!lv_obj_has_state(sw, LV_STATE_CHECKED)); /* failed ON restores OFF */
    int before = submissions;
    snapshot[0].online = false;
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
    assert(submissions == before);
    snapshot[0].online = true;
    submit_result = AGENT_OK;
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
    assert(strstr(lv_label_get_text(ui->panel_feedback_label), "等待设备状态"));

    snapshot[0].control_count = 1;
    smart_home_miloco_control_t *ctrl = &snapshot[0].controls[0];
    strcpy(ctrl->iid, "2.1");
    ctrl->type = SMART_HOME_MILOCO_CTRL_BOOL;
    ctrl->value = 1;
    miloco_sheet_build(ui, snapshot[0].did);
    lv_obj_t *row = lv_obj_get_child(ui->miloco_sheet, 4);
    lv_obj_t *sheet_sw = lv_obj_get_child(row, 1);
    assert(lv_obj_check_type(sheet_sw, &lv_switch_class));
    submit_result = AGENT_ERROR_INVALID;
    lv_obj_remove_state(sheet_sw, LV_STATE_CHECKED);
    lv_obj_send_event(sheet_sw, LV_EVENT_VALUE_CHANGED, NULL);
    assert(lv_obj_has_state(sheet_sw, LV_STATE_CHECKED));
    assert(strstr(lv_label_get_text(ui->miloco_sheet_feedback), "未能提交"));
    miloco_sheet_close_impl(ui);

    /* Maximum enum schema used to overflow the nine-entry callback pool. */
    snapshot[0].control_count = SMART_HOME_MILOCO_MAX_CONTROLS;
    for (int i = 0; i < SMART_HOME_MILOCO_MAX_CONTROLS; i++) {
        ctrl = &snapshot[0].controls[i];
        snprintf(ctrl->iid, sizeof(ctrl->iid), "2.%d", i);
        ctrl->type = SMART_HOME_MILOCO_CTRL_ENUM;
        ctrl->option_count = SMART_HOME_MILOCO_MAX_OPTIONS;
        for (int j = 0; j < SMART_HOME_MILOCO_MAX_OPTIONS; j++) {
            ctrl->options[j].value = j;
            snprintf(ctrl->options[j].name, sizeof(ctrl->options[j].name), "%d", j);
        }
    }
    miloco_sheet_build(ui, snapshot[0].did);
    for (int i = 0; i < SMART_HOME_MILOCO_MAX_CONTROLS; i++) {
        row = lv_obj_get_child(ui->miloco_sheet, i + 4);
        for (int j = 0; j < SMART_HOME_MILOCO_MAX_OPTIONS; j++) {
            before = submissions;
            lv_obj_send_event(lv_obj_get_child(row, j + 1), LV_EVENT_CLICKED, NULL);
            assert(submissions == before + 1);
            assert(strstr(lv_label_get_text(ui->miloco_sheet_feedback), "未能提交"));
        }
    }
    miloco_sheet_close_impl(ui);
    ctrl->type = SMART_HOME_MILOCO_CTRL_ACTION;
    snapshot[0].control_count = 1;
    miloco_sheet_build(ui, snapshot[0].did);
    row = lv_obj_get_child(ui->miloco_sheet, 4);
    before = submissions;
    lv_obj_send_event(lv_obj_get_child(row, 1), LV_EVENT_CLICKED, NULL);
    assert(submissions == before + 1);
    assert(strstr(lv_label_get_text(ui->miloco_sheet_feedback), "未能提交"));
    miloco_sheet_close_impl(ui);
    puts("PASS: power and BOOL rollback; offline guard; ENUM/ACTION feedback; 8x4 controls");
}

static unsigned deleted_objects;
static void count_deleted(lv_event_t *event)
{ (void)event; deleted_objects++; }

static void test_incremental(smart_home_lvgl_t *ui, lv_obj_t *screen)
{
    ui->device_state = &ui->app->device_state;
    ui->panel_grid = lv_obj_create(screen);
    lv_obj_set_size(ui->panel_grid, 968, 180);
    lv_obj_set_flex_flow(ui->panel_grid, LV_FLEX_FLOW_ROW_WRAP);
    ui->panel_filter_row = lv_obj_create(screen);
    ui->panel_source_dd = lv_dropdown_create(screen);
    lv_dropdown_set_options(ui->panel_source_dd, "全部来源\n米家\nNode");
    lv_dropdown_set_selected(ui->panel_room_dd, 0);
    memset(snapshot, 0, sizeof(snapshot));
    snapshot_count = SMART_HOME_MILOCO_MAX_DEVICES;
    for (size_t i = 0; i < snapshot_count; i++) {
        snprintf(snapshot[i].did, sizeof(snapshot[i].did), "device-%u", (unsigned)i);
        strcpy(snapshot[i].room, "living_room");
        strcpy(snapshot[i].name, "测试设备");
        snapshot[i].online = true;
        snapshot[i].controllable = true;
        snapshot[i].control_count = SMART_HOME_MILOCO_MAX_CONTROLS;
        for (size_t j = 0; j < SMART_HOME_MILOCO_MAX_CONTROLS; j++) {
            snprintf(snapshot[i].controls[j].iid, sizeof(snapshot[i].controls[j].iid),
                     "2.%u", (unsigned)j);
            snapshot[i].controls[j].type = SMART_HOME_MILOCO_CTRL_BOOL;
        }
    }
    rebuild_device_cards(ui);
    assert(lv_obj_get_child_count(ui->panel_grid) == SMART_HOME_MILOCO_MAX_DEVICES);
    lv_obj_t *card = ui->miloco_cards[0].card;
    lv_obj_t *power = ui->miloco_cards[0].power;
    lv_obj_add_event_cb(card, count_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_update_layout(ui->panel_grid);
    lv_obj_scroll_to_y(ui->panel_grid, 80, LV_ANIM_OFF);
    int32_t grid_scroll = lv_obj_get_scroll_y(ui->panel_grid);
    assert(grid_scroll == 80);
    strcpy(snapshot[0].name, "更新的设备名");
    snapshot[0].power_on = true;
    rebuild_device_cards(ui);
    assert(deleted_objects == 0 && ui->miloco_cards[0].card == card);
    assert(ui->miloco_cards[0].power == power && lv_obj_has_state(power, LV_STATE_CHECKED));
    assert(!strcmp(lv_label_get_text(ui->miloco_cards[0].name), "更新的设备名"));
    assert(lv_obj_get_scroll_y(ui->panel_grid) == grid_scroll);
    smart_home_miloco_device_t swap = snapshot[0];
    snapshot[0] = snapshot[1]; snapshot[1] = swap;
    rebuild_device_cards(ui);
    assert(ui->miloco_cards[0].card == card && deleted_objects == 0);
    assert(!strcmp(lv_obj_get_user_data(power), "device-0"));
    /* A queued command keeps the pending value until readback or timeout. */
    submit_result = AGENT_OK;
    lv_obj_remove_state(power, LV_STATE_CHECKED);
    lv_obj_send_event(power, LV_EVENT_VALUE_CHANGED, NULL);
    append_miloco_cards(ui);
    assert(!lv_obj_has_state(power, LV_STATE_CHECKED));
    lv_tick_inc(MILOCO_CONFIRM_WAIT_MS + 1);
    append_miloco_cards(ui);
    assert(lv_obj_has_state(power, LV_STATE_CHECKED));
    assert(strstr(lv_label_get_text(ui->panel_feedback_label), "尚未确认"));

    miloco_sheet_build(ui, "device-0");
    lv_obj_t *sheet = ui->miloco_sheet;
    miloco_sheet_context_t *context = ui->miloco_sheet_context;
    lv_obj_t *control = context->requests[0].widget;
    lv_obj_add_event_cb(sheet, count_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_update_layout(sheet);
    lv_obj_scroll_to_y(sheet, 40, LV_ANIM_OFF);
    int32_t sheet_scroll = lv_obj_get_scroll_y(sheet);
    assert(sheet_scroll == 40);
    int before = submissions;
    snapshot[1].controls[0].value = 1;
    smart_home_lvgl_miloco_sheet_refresh(ui);
    assert(ui->miloco_sheet == sheet && deleted_objects == 0);
    assert(context->requests[0].widget == control);
    assert(lv_obj_has_state(control, LV_STATE_CHECKED));
    assert(lv_obj_get_scroll_y(sheet) == sheet_scroll && submissions == before);
    snapshot[1].online = false;
    smart_home_lvgl_miloco_sheet_refresh(ui);
    assert(lv_obj_has_state(control, LV_STATE_DISABLED));
    snapshot[1].online = true;
    smart_home_lvgl_miloco_sheet_refresh(ui);
    assert(!lv_obj_has_state(control, LV_STATE_DISABLED));
    strcpy(snapshot[1].controls[0].desc, "新的控制项");
    smart_home_lvgl_miloco_sheet_refresh(ui);
    assert(deleted_objects == 1); /* schema change alone rebuilds the drawer */
    assert(lv_obj_get_scroll_y(ui->miloco_sheet) == sheet_scroll);
    snapshot[1].controls[1].type = SMART_HOME_MILOCO_CTRL_ENUM;
    snapshot[1].controls[1].option_count = 2;
    snapshot[1].controls[1].options[0].value = 0;
    snapshot[1].controls[1].options[1].value = 1;
    smart_home_lvgl_miloco_sheet_refresh(ui);
    context = ui->miloco_sheet_context;
    sheet = ui->miloco_sheet;
    lv_obj_t *option = context->requests[3].widget;
    snapshot[1].controls[1].value = 1;
    smart_home_lvgl_miloco_sheet_refresh(ui);
    assert(ui->miloco_sheet == sheet && context->requests[3].widget == option);
    assert(lv_color_eq(lv_obj_get_style_bg_color(option, 0), SMART_HOME_UI_COLOR_PRIMARY));
    snapshot_count = 0;
    smart_home_lvgl_miloco_sheet_refresh(ui);
    rebuild_device_cards(ui);
    assert(!ui->miloco_sheet && !ui->miloco_sheet_context);
    assert(lv_obj_get_child_count(ui->panel_grid) == 0);
    for (size_t i = 0; i < SMART_HOME_MILOCO_MAX_DEVICES; i++)
        assert(!ui->miloco_cards[i].card);
    /* A home refresh consumes the revision without marking hidden cards current. */
    ui->active_tab = SMART_HOME_TAB_HOME;
    snapshot_revision = ui->miloco_revision + 1;
    smart_home_lvgl_miloco_poll_set_enabled(ui, 1);
    miloco_timer_cb(ui->miloco_timer);
    unsigned reads = snapshot_reads;
    miloco_timer_cb(ui->miloco_timer);
    assert(reads == snapshot_reads);
    assert(ui->miloco_polled_revision == snapshot_revision);
    assert(ui->miloco_revision != snapshot_revision);
    smart_home_lvgl_miloco_poll_set_enabled(ui, 0);
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
    ui->app->node_gateway = (smart_home_node_gateway_t *)&node_snapshot;
    node_count = 1;
    strcpy(node_snapshot[0].node_id, "node-1");
    strcpy(node_snapshot[0].display_name, "节点一");
    node_snapshot[0].online = true;
    rebuild_device_cards(ui);
    assert(lv_obj_get_child_count(ui->panel_grid) == 1);
    lv_obj_t *node_card = ui->node_cards[0].card;
    rebuild_device_cards(ui);
    assert(ui->node_cards[0].card == node_card);
    assert(lv_obj_get_child_count(ui->panel_grid) == 1);
    lv_dropdown_set_selected(ui->panel_source_dd, 1);
    rebuild_device_cards(ui);
    assert(!ui->node_cards[0].card && !lv_obj_get_child_count(ui->panel_grid));
    lv_dropdown_set_selected(ui->panel_source_dd, 0);
    rebuild_device_cards(ui);
    node_snapshot[0].online = false;
    rebuild_device_cards(ui);
    assert(!ui->node_cards[0].card);
    node_count = 0;
#endif
    puts("PASS: stable card/control objects, scroll, device reorder/removal, pending timeout, revision gating");
}

static void test_v9_views(void)
{
    smart_home_state_t state;
    smart_home_v9_device_view_t views[SMART_HOME_V9_MAX_VIEWS];
    smart_home_v9_control_t control;
    size_t count;

    smart_home_device_init(&state);
    count = smart_home_v9_collect_views(&state, NULL, views,
                                        SMART_HOME_V9_MAX_VIEWS);
    assert(count == 3);
    assert(views[0].source == SMART_HOME_V9_SOURCE_LOCAL);
    assert(views[0].kind == SMART_HOME_V9_KIND_LIGHT);
    assert((views[0].capabilities & SMART_HOME_V9_CAP_BRIGHTNESS) != 0);
    assert(views[2].kind == SMART_HOME_V9_KIND_AC);
    assert((views[2].capabilities & SMART_HOME_V9_CAP_TEMPERATURE) != 0);

    count = smart_home_v9_demo_views(views, SMART_HOME_V9_MAX_VIEWS);
    assert(count == 6);
    assert(views[3].kind == SMART_HOME_V9_KIND_ROBOT);
    assert(views[3].source == SMART_HOME_V9_SOURCE_DEMO);

    smart_home_v9_control_begin(&control, &views[0], 0, 7, 1000);
    assert(control.state == SMART_HOME_V9_CONTROL_QUEUED);
    smart_home_v9_control_result(&control, AGENT_OK);
    assert(control.state == SMART_HOME_V9_CONTROL_WAIT_READBACK);
    views[0].on = 0;
    views[0].state_valid = true;
    smart_home_v9_control_snapshot(&control, &views[0], 1100, 10000);
    assert(control.state == SMART_HOME_V9_CONTROL_SUCCEEDED);

    smart_home_v9_control_begin(&control, &views[0], 0, 8, 1000);
    smart_home_v9_control_result(&control, AGENT_OK);
    views[0].on = 1;
    smart_home_v9_control_snapshot(&control, &views[0], 12000, 10000);
    assert(control.state == SMART_HOME_V9_CONTROL_TIMED_OUT);
    puts("PASS: V9 view mapping and async control readback/timeout");
}

/* The panel harness does not link the full device service; local real
 * control is exercised on target.  The V9 view/control state machine has its
 * own host test below. */
int smart_home_device_service_set_device_control(
    smart_home_device_service_t *service, int device_id, int on, int value,
    int mode, int fan_speed)
{
    (void)service; (void)device_id; (void)on; (void)value; (void)mode;
    (void)fan_speed;
    return AGENT_ERROR_INVALID;
}

static void test_room_transaction(void)
{
    smart_home_state_t state;
    smart_home_device_t *device;
    int device_id = 0;

    smart_home_device_init(&state);
    assert(state.room_count == 2);
    assert(smart_home_room_add(&state, "balcony") == AGENT_OK);
    assert(smart_home_device_add(&state, "balcony", "Washer",
                                 SMART_HOME_DEVICE_LIGHT,
                                 &device_id) == AGENT_OK);
    assert(smart_home_room_delete_and_reassign(&state, "balcony",
                                               "living_room") == AGENT_OK);
    assert(smart_home_room_index(&state, "balcony") < 0);
    assert(state.room_count == 2);
    device = smart_home_device_find_by_id(&state, device_id);
    assert(device && !strcmp(device->room, "living_room"));

    assert(smart_home_room_delete_and_reassign(&state, "living_room",
                                               "bedroom") == AGENT_OK);
    assert(state.room_count == 1);
    assert(smart_home_room_delete_and_reassign(&state, "bedroom",
                                               "living_room") ==
           AGENT_ERROR_NOT_FOUND);
    assert(smart_home_room_delete_and_reassign(&state, "bedroom",
                                               "bedroom") ==
           AGENT_ERROR_INVALID);
    puts("PASS: room delete reassigns devices and refuses last/duplicate rooms");
}

int main(void)
{
    test_v9_views();
    test_room_transaction();
    lv_init();
    lv_display_t *display = lv_display_create(1024, 600);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, flush);
    assert(smart_home_lvgl_style_init() == 0);
    smart_home_agent_app_t app = {0};
    smart_home_lvgl_t ui = {0};
    app.miloco = (smart_home_miloco_t *)&snapshot;
    ui.app = &app;
    lv_obj_t *screen = lv_obj_create(NULL);
    ui.panel_room_dd = lv_dropdown_create(screen);
    lv_dropdown_set_options(ui.panel_room_dd, "全部");
    snapshot_count = 4;
    strcpy(snapshot[0].room, "living_room");
    strcpy(snapshot[1].room, "书房");
    strcpy(snapshot[2].room, "书房");
    assert(miloco_sync_room_options(&ui));
    assert(!strcmp(lv_dropdown_get_options(ui.panel_room_dd),
                   "全部\n客厅\n书房\n未分配房间"));
    lv_dropdown_set_selected(ui.panel_room_dd, 2);
    assert(miloco_matches_room(&ui, "书房"));
    assert(!miloco_matches_room(&ui, "living_room"));
    assert(!miloco_matches_room(&ui, ""));
    strcpy(snapshot[0].room, "书房");
    strcpy(snapshot[1].room, "living_room");
    assert(miloco_sync_room_options(&ui));
    assert(lv_dropdown_get_selected(ui.panel_room_dd) == 1);
    assert(miloco_matches_room(&ui, "书房"));
    lv_dropdown_set_selected(ui.panel_room_dd, 3);
    assert(miloco_matches_room(&ui, ""));
    snapshot_count = 1;
    assert(miloco_sync_room_options(&ui));
    assert(lv_dropdown_get_selected(ui.panel_room_dd) == 0);
    assert(miloco_matches_room(&ui, "living_room"));
    snapshot_count = 0;
    assert(miloco_sync_room_options(&ui));
    assert(!strcmp(lv_dropdown_get_options(ui.panel_room_dd), "全部"));
    assert(!miloco_sync_room_options(&ui));
    puts("PASS: real-room catalog, deduplication, filtering, reorder and removal");
    test_feedback(&ui, screen);
    test_incremental(&ui, screen);
    test_home(&ui);
    lv_obj_delete(screen);
    return 0;
}
