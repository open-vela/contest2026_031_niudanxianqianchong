/* SPDX-License-Identifier: Apache-2.0 */
/* Reuse the panel harness services while exercising the camera-enabled home. */
#define main v9_harness_main
#include "test_home_v9.c"
#undef main
#include "smart_home_lvgl_camera_dialog.h"
#include "../../src/camera/smart_home_camera_control.h"
#include "../../src/camera/smart_home_camera_service.h"

static struct smart_home_camera_status_s camera_status;
static unsigned start_requests, stop_requests;

int smart_home_camera_get_status(struct smart_home_camera_status_s *out)
{
    *out = camera_status;
    return 0;
}

int smart_home_camera_request(bool on)
{
    if (on) start_requests++;
    else stop_requests++;
    memset(&camera_status, 0, sizeof camera_status);
    camera_status.state = on ? SMART_HOME_CAMERA_STARTING : SMART_HOME_CAMERA_OFF;
    return 0;
}

void smart_home_camera_control_status(smart_home_camera_control_status_t *out)
{
    memset(out, 0, sizeof(*out));
}

int smart_home_camera_copy_latest(uint8_t *dst, size_t size, uint32_t *sequence)
{
    assert(size == SMART_HOME_CAMERA_PREVIEW_BYTES);
    memset(dst, 0, size);
    *sequence = camera_status.preview_sequence;
    return 0;
}

static lv_obj_t *camera_card(smart_home_lvgl_t *ui)
{
    lv_obj_t *name = find_v9_label(ui->screen_home, "本机摄像头");
    assert(name);
    return lv_obj_get_parent(name);
}

static void assert_camera_art(lv_obj_t *card, const char *filename)
{
    lv_obj_t *art = NULL;

    for (uint32_t i = 0; i < lv_obj_get_child_count(card); i++) {
        lv_obj_t *child = lv_obj_get_child(card, i);
        if (lv_obj_check_type(child, &lv_image_class)) {
            art = child;
            break;
        }
    }

    assert(art);
    const char *src = lv_image_get_src(art);
    const char *basename;

    assert(src);
    basename = strrchr(src, '/');
    basename = basename ? basename + 1 : src;
    assert(!strcmp(basename, filename));
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(1024, 600);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof pixels,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, flush);
    assert(!smart_home_lvgl_style_init());
    smart_home_agent_app_t app = {0};
    smart_home_lvgl_t ui = {0};
    smart_home_device_init(&app.device_state);
    app.device_service.state = &app.device_state;
    app.device_service.mutex_initialized = 1;
    app.miloco = (smart_home_miloco_t *)&snapshot;
    ui.app = &app;
    ui.device_state = &app.device_state;
    ui.active_tab = SMART_HOME_TAB_HOME;
    local_state_enabled = 1;
    snapshot_count = 12;
    for (size_t i = 0; i < snapshot_count; i++) {
        snprintf(snapshot[i].did, sizeof snapshot[i].did, "remote-%zu", i);
        snprintf(snapshot[i].name, sizeof snapshot[i].name, "米家灯-%zu", i);
        strcpy(snapshot[i].room, "living_room");
        snapshot[i].category = SMART_HOME_MILOCO_CATEGORY_LIGHT;
        snapshot[i].online = snapshot[i].controllable = snapshot[i].power_valid = true;
    }
    smart_home_lvgl_build_home_screen_v9(&ui);
    lv_obj_update_layout(ui.screen_home);
    assert(smart_home_lvgl_v9_visible_card_count() == 12);
    lv_obj_t *card = camera_card(&ui);
    assert_camera_art(card, "camera-online.png");
    camera_status.state = SMART_HOME_CAMERA_ERROR;
    smart_home_lvgl_v9_refresh_views(&ui);
    assert_camera_art(card, "camera-offline.png");
    camera_status.state = SMART_HOME_CAMERA_OFF;
    smart_home_lvgl_v9_refresh_views(&ui);
    assert_camera_art(card, "camera-online.png");
    assert(lv_obj_get_parent(card) == find_v9_carousel(ui.screen_home));
    assert(lv_obj_get_x(card) == 4);
    assert(find_v9_label(ui.screen_home, "米家灯-0"));
    assert(!start_requests && !ui.topbar_camera_on);
    capture_to(&ui, "SMART_HOME_CAMERA_HOME_CAPTURE");

    unsigned before = lv_obj_get_child_count(ui.screen_home);
    lv_obj_send_event(card, LV_EVENT_CLICKED, NULL);
    assert(lv_obj_get_child_count(ui.screen_home) == before + 1);
    assert(find_v9_label(ui.screen_home, "开启预览后显示实时画面"));
    assert(!start_requests);
    lv_obj_send_event(lv_obj_get_child(ui.screen_home, -1), LV_EVENT_CLICKED, NULL);
    assert(lv_obj_get_child_count(ui.screen_home) == before);

    lv_obj_t *sw = lv_obj_get_child(card, -1);
    lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
    assert(start_requests == 1);
    assert(lv_obj_get_child_count(ui.screen_home) == before + 1);
    assert(!ui.topbar_camera_on);
    camera_status.state = SMART_HOME_CAMERA_RUNNING;
    camera_status.preview_sequence = 1;
    camera_status.preview_fps_x100 = 1500;
    lv_tick_inc(70);
    lv_timer_handler();
    smart_home_lvgl_v9_refresh_views(&ui);
    assert(ui.topbar_camera_on);
    assert(find_v9_label(ui.screen_home, "预览中"));
    assert_camera_art(card, "camera-patrol.png");

    unsigned stopped = stop_requests;
    ui.screen_chat = lv_obj_create(NULL);
    smart_home_lvgl_load_tab(&ui, SMART_HOME_TAB_CHAT);
    assert(stop_requests > stopped && camera_status.state == SMART_HOME_CAMERA_OFF);
    assert(lv_obj_get_child_count(ui.screen_home) == before);
    smart_home_lvgl_v9_refresh_views(&ui);
    assert(!ui.topbar_camera_on);
    assert_camera_art(card, "camera-online.png");

    gateway_online = false;
    smart_home_lvgl_v9_refresh_views(&ui);
    assert(smart_home_lvgl_v9_visible_card_count() == 2);
    (void)camera_card(&ui);
    smart_home_lvgl_load_tab(&ui, SMART_HOME_TAB_HOME);
    assert(!smart_home_device_assign_room(&app.device_state, SMART_HOME_BOARD_CAMERA_KEY,
                                          "bedroom"));
    smart_home_lvgl_v9_refresh_views(&ui);
    assert(smart_home_lvgl_v9_select_room(2) == 1); /* Only the real board camera. */
    assert(!lv_obj_has_flag(camera_card(&ui), LV_OBJ_FLAG_HIDDEN));

    lv_timer_delete(ui.home_time_timer);
    lv_timer_delete(ui.v9_poll_timer);
    lv_obj_delete(ui.screen_chat);
    lv_obj_delete(ui.screen_home);
    smart_home_lvgl_style_deinit();
    lv_deinit();
    puts("PASS: actual home retains board camera with a full gateway list; card, switch, "
         "first frame, navigation stop, offline gateway and room filter");
    return 0;
}
