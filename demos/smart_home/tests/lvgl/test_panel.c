/* Exercise the actual panel helpers against LVGL, with a snapshot-only service. */
#include "smart_home_lvgl_panel.c"
#include <assert.h>
#include <stdlib.h>

static uint16_t pixels[1024 * 600];
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{ (void)area; (void)data; lv_display_flush_ready(display); }
static void capture_home(smart_home_lvgl_t *ui)
{
    const char *path = getenv("SMART_HOME_LVGL_CAPTURE");
    if (!path) return;
    lv_screen_load(ui->screen_home);
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
lv_obj_t *smart_home_lvgl_build_nav_bar(lv_obj_t *screen, smart_home_lvgl_t *ui)
{ (void)screen; (void)ui; return NULL; }
void smart_home_lvgl_load_tab(smart_home_lvgl_t *ui, int tab)
{ ui->active_tab = tab; }
void smart_home_lvgl_refresh_miloco_screen(smart_home_lvgl_t *ui) { (void)ui; }
void smart_home_lvgl_chat_send_text(smart_home_lvgl_t *ui, const char *text)
{ (void)ui; (void)text; }

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
    capture_home(ui);
    lv_timer_delete(ui->home_time_timer);
    ui->home_time_timer = NULL;
    lv_obj_delete(ui->screen_home);
    lv_obj_delete(ui->screen_screensaver);
    puts("PASS: cached weather/city, truthful counts, unknown status, midnight date, separated layout");
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

int main(void)
{
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
