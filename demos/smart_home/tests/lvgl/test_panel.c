/* Exercise the actual panel helpers against LVGL, with a snapshot-only service. */
#include "smart_home_lvgl_panel.c"
#include <assert.h>

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
    lv_obj_delete(screen);
    return 0;
}
