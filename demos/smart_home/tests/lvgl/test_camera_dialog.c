#include "smart_home_lvgl_camera_dialog.c"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include <errno.h>

static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static struct smart_home_camera_status_s fake;
static pthread_t ui_thread;
static atomic_int hold_stop, fail_start, fail_alloc;
static int starts, stops, room_opened;
static uint16_t screen_pixels[1024 * 600];
void *smart_home_bulk_alloc(size_t n) {
    return atomic_load(&fail_alloc) ? NULL : malloc(n);
}
void smart_home_bulk_free(void *p) {
    free(p);
}
int smart_home_camera_get_status(struct smart_home_camera_status_s *out) {
    pthread_mutex_lock(&fake_lock);
    *out = fake;
    pthread_mutex_unlock(&fake_lock);
    return 0;
}
int smart_home_camera_start(void) {
    assert(!pthread_equal(pthread_self(), ui_thread));
    pthread_mutex_lock(&fake_lock);
    starts++;
    fake.state =
        atomic_load(&fail_start) ? SMART_HOME_CAMERA_ERROR : SMART_HOME_CAMERA_STARTING;
    fake.last_error = atomic_load(&fail_start) ? -ENODEV : 0;
    int ret = fake.last_error;
    pthread_mutex_unlock(&fake_lock);
    return ret;
}
int smart_home_camera_stop(void) {
    assert(!pthread_equal(pthread_self(), ui_thread));
    while (atomic_load(&hold_stop))
        usleep(1000);
    pthread_mutex_lock(&fake_lock);
    stops++;
    memset(&fake, 0, sizeof fake);
    pthread_mutex_unlock(&fake_lock);
    return 0;
}
int smart_home_camera_copy_latest(uint8_t *dst, size_t n, uint32_t *seq) {
    assert(n == SMART_HOME_CAMERA_PREVIEW_BYTES);
    pthread_mutex_lock(&fake_lock);
    if (fake.state != SMART_HOME_CAMERA_RUNNING || !fake.preview_sequence) {
        pthread_mutex_unlock(&fake_lock);
        return -EAGAIN;
    }
    *seq = fake.preview_sequence;
    pthread_mutex_unlock(&fake_lock);
    /* Synthetic RGB565 scene for host-only rendering, not camera evidence. */
    uint16_t *p = (uint16_t *)dst;
    for (unsigned y = 0; y < 300; y++)
        for (unsigned x = 0; x < 512; x++)
            p[y * 512 + x] = y < 180 ? (uint16_t)(0x7c00 + ((x / 64) << 1)) : 0x8cf0;
    return 0;
}
void smart_home_lvgl_set_camera_indicator(smart_home_lvgl_t *ui, int on) {
    ui->topbar_camera_on = on != 0;
}
const char *smart_home_lvgl_room_display(const char *room) {
    return !strcmp(room, "living_room") ? "客厅" : (!strcmp(room, "bedroom") ? "卧室" : room);
}
void smart_home_lvgl_room_assign_open(smart_home_lvgl_t *ui,
                                      const smart_home_v9_device_view_t *view) {
    (void)ui;
    assert(!strcmp(view->key, SMART_HOME_BOARD_CAMERA_KEY));
    room_opened++;
}
size_t smart_home_miloco_list(const smart_home_miloco_t *s,
                              smart_home_miloco_device_t *out, size_t capacity,
                              uint32_t *rev) {
    (void)s;
    (void)rev;
    size_t n = capacity < 16 ? capacity : 16;
    memset(out, 0, n * sizeof(*out));
    for (size_t i = 0; i < n; i++) {
        snprintf(out[i].did, sizeof out[i].did, "%zu", i);
        snprintf(out[i].name, sizeof out[i].name, "米家灯-%zu", i);
        strcpy(out[i].room, "bedroom");
        out[i].category = SMART_HOME_MILOCO_CATEGORY_LIGHT;
        out[i].online = out[i].controllable = true;
    }
    return n;
}
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *p) {
    (void)a;
    (void)p;
    lv_display_flush_ready(d);
}
static void settle(void) {
    smart_home_camera_control_status_t status;
    for (int i = 0; i < 3000; i++) {
        smart_home_camera_control_status(&status);
        if (!status.busy)
            return;
        usleep(1000);
    }
    assert(!"camera control did not settle");
}
static void frames(void) {
    pthread_mutex_lock(&fake_lock);
    fake.state = SMART_HOME_CAMERA_RUNNING;
    fake.preview_sequence++;
    fake.preview_fps_x100 = 1500;
    fake.capture_fps_x100 = 3000;
    pthread_mutex_unlock(&fake_lock);
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
        unsigned p = screen_pixels[i];
        uint8_t rgb[] = {((p >> 11) & 31) * 255 / 31, ((p >> 5) & 63) * 255 / 63,
                         (p & 31) * 255 / 31};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}
int main(void) {
    ui_thread = pthread_self();
    lv_init();
    lv_display_t *d = lv_display_create(1024, 600);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, screen_pixels, NULL, sizeof screen_pixels,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush);
    assert(!smart_home_lvgl_style_init());
    smart_home_state_t state, restored;
    smart_home_device_init(&state);
    assert(!strcmp(smart_home_device_room_override(&state, SMART_HOME_BOARD_CAMERA_KEY),
                   "living_room"));
    assert(!smart_home_room_add(&state, "书房"));
    assert(!smart_home_device_assign_room(&state, SMART_HOME_BOARD_CAMERA_KEY, "书房"));
    cJSON *json = smart_home_device_state_to_json(&state);
    assert(!smart_home_device_state_from_json(&restored, json));
    cJSON_Delete(json);
    assert(
        !strcmp(smart_home_device_room_override(&restored, SMART_HOME_BOARD_CAMERA_KEY),
                "书房"));
    assert(!smart_home_room_delete_and_reassign(&restored, "书房", "bedroom"));
    assert(
        !strcmp(smart_home_device_room_override(&restored, SMART_HOME_BOARD_CAMERA_KEY),
                "bedroom"));
    smart_home_v9_device_view_t views[16], view;
    size_t count = smart_home_v9_collect_views(
        &restored, (const smart_home_miloco_t *)1, views, 16);
    assert(count == 16 && views[0].source == SMART_HOME_V9_SOURCE_BOARD);
    assert(views[1].source == SMART_HOME_V9_SOURCE_LOCAL &&
           views[4].source == SMART_HOME_V9_SOURCE_MILOCO);
    assert(!strcmp(views[0].room_id, "bedroom") && !views[0].on);
    assert(smart_home_v9_collect_views(&restored, (const smart_home_miloco_t *)1, views,
                                       1) == 1);
    assert(!strcmp(views[0].key, SMART_HOME_BOARD_CAMERA_KEY));
    view = views[0];
    smart_home_lvgl_t ui = {0};
    ui.screen_home = lv_obj_create(NULL);
    lv_screen_load(ui.screen_home);
    atomic_store(&fail_alloc, 1);
    assert(smart_home_lvgl_camera_dialog_open(&ui, &view) == AGENT_ERROR_NOMEM);
    assert(!g_camera_dialog && !starts);
    atomic_store(&fail_alloc, 0);
    assert(!smart_home_lvgl_camera_dialog_open(&ui, &view));
    assert(!starts && !g_camera_dialog->pixels && !ui.topbar_camera_on);
    capture("SMART_HOME_CAMERA_OFF_CAPTURE");
    lv_obj_add_state(g_camera_dialog->sw, LV_STATE_CHECKED);
    lv_obj_send_event(g_camera_dialog->sw, LV_EVENT_VALUE_CHANGED, NULL);
    settle();
    refresh(g_camera_dialog->timer);
    assert(starts == 1 && !ui.topbar_camera_on);
    assert(strstr(lv_label_get_text(g_camera_dialog->status), "等待首帧"));
    frames();
    refresh(g_camera_dialog->timer);
    assert(g_camera_dialog->pixels && ui.topbar_camera_on);
    assert(!lv_obj_has_flag(g_camera_dialog->image, LV_OBJ_FLAG_HIDDEN));
    assert(!strcmp(lv_label_get_text(g_camera_dialog->status), "实时预览"));
    assert(!smart_home_v9_board_camera_view(&restored, &view) &&
           view.activity == SMART_HOME_V9_CAMERA_PREVIEWING);
    capture("SMART_HOME_CAMERA_ON_CAPTURE");
    atomic_store(&hold_stop, 1);
    smart_home_lvgl_camera_dialog_close();
    assert(!g_camera_dialog); /* close returns before the blocked stop worker */
    smart_home_camera_control_status_t pending;
    smart_home_camera_control_status(&pending);
    assert(pending.busy && !pending.desired_on);
    assert(!smart_home_camera_request(true));
    assert(!smart_home_camera_request(false)); /* latest request wins during teardown */
    atomic_store(&hold_stop, 0);
    settle();
    assert(starts == 1);
    atomic_store(&fail_start, 1);
    assert(!smart_home_lvgl_camera_dialog_open(&ui, &view));
    lv_obj_add_state(g_camera_dialog->sw, LV_STATE_CHECKED);
    lv_obj_send_event(g_camera_dialog->sw, LV_EVENT_VALUE_CHANGED, NULL);
    settle();
    refresh(g_camera_dialog->timer);
    assert(strstr(lv_label_get_text(g_camera_dialog->status), "失败"));
    atomic_store(&fail_start, 0);
    lv_obj_add_state(g_camera_dialog->sw, LV_STATE_CHECKED);
    lv_obj_send_event(g_camera_dialog->sw, LV_EVENT_VALUE_CHANGED, NULL);
    settle();
    refresh(g_camera_dialog->timer);
    lv_tick_inc(10001);
    refresh(g_camera_dialog->timer);
    settle();
    refresh(g_camera_dialog->timer);
    assert(g_camera_dialog->timeout &&
           strstr(lv_label_get_text(g_camera_dialog->status), "未获取画面"));
    assert(!ui.topbar_camera_on);
    smart_home_lvgl_camera_dialog_close();
    settle();
    assert(!smart_home_lvgl_camera_dialog_open(&ui, &view));
    /* Room navigation closes the preview and passes the fixed board key. */
    lv_obj_t *panel = lv_obj_get_child(g_camera_dialog->mask, 0);
    lv_obj_update_layout(panel);
    for (uint32_t i = 0; i < lv_obj_get_child_count(panel); i++) {
        lv_obj_t *obj = lv_obj_get_child(panel, i);
        if (lv_obj_check_type(obj, &lv_button_class) && lv_obj_get_y(obj) == 302) {
            lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
            break;
        }
    }
    assert(!g_camera_dialog && room_opened == 1);
    settle();
    assert(!smart_home_lvgl_camera_dialog_open(&ui, &view));
    lv_obj_add_state(g_camera_dialog->sw, LV_STATE_CHECKED);
    lv_obj_send_event(g_camera_dialog->sw, LV_EVENT_VALUE_CHANGED, NULL);
    settle();
    frames();
    atomic_store(&fail_alloc, 1);
    refresh(g_camera_dialog->timer);
    assert(g_camera_dialog->memory_error && !g_camera_dialog->pixels);
    atomic_store(&fail_alloc, 0);
    settle();
    refresh(g_camera_dialog->timer);
    assert(strstr(lv_label_get_text(g_camera_dialog->status), "内存不足"));
    lv_obj_add_state(g_camera_dialog->sw, LV_STATE_CHECKED);
    lv_obj_send_event(g_camera_dialog->sw, LV_EVENT_VALUE_CHANGED, NULL);
    settle();
    frames();
    refresh(g_camera_dialog->timer);
    assert(!g_camera_dialog->memory_error && g_camera_dialog->pixels);
    lv_obj_delete(ui.screen_home);
    assert(!g_camera_dialog);
    settle();
    assert(!smart_home_camera_control_shutdown());
    smart_home_lvgl_style_deinit();
    lv_deinit();
    puts(
        "PASS: board camera + gateway merge/capacity, room persistence/migration, lazy "
        "preview, first frame, async close, errors/retry/timeout, page destruction");
    return 0;
}
