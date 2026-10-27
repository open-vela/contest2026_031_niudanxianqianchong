/* Exercise the actual panel helpers against LVGL, with a snapshot-only service. */
#include "smart_home_lvgl_panel.c"
#include <assert.h>
#include <stdlib.h>

static smart_home_miloco_device_t snapshot[SMART_HOME_MILOCO_MAX_DEVICES];
static size_t snapshot_count;
size_t smart_home_miloco_list(const smart_home_miloco_t *service,
                             smart_home_miloco_device_t *out,
                             size_t capacity, uint32_t *revision)
{
    (void)service;
    if (revision) *revision = 1;
    size_t n = capacity < snapshot_count ? capacity : snapshot_count;
    if (out) memcpy(out, snapshot, n * sizeof(*out));
    return out ? n : snapshot_count;
}

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

int main(void)
{
    lv_init();
    lv_display_create(1024, 600);
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
    lv_obj_delete(screen);
    return 0;
}
