/* Real KWS-only V9 entry, asynchronous persistence and rollback. */
#define main settings_fixture_main
#include "test_settings_v9.c"
#undef main
#include "smart_home_lvgl_wake_settings.c"
static int paused = -1, running = 1;
void smart_home_kws_config_load(smart_home_kws_config_t *config) { config->enabled = 0; }
int smart_home_kws_config_save(const smart_home_kws_config_t *config)
{ assert(config->enabled); return block_service(0); }
void kws_service_set_paused(int value) { paused = value; }
int kws_service_running(void) { return running; }
void smart_home_lvgl_build_top_bar(lv_obj_t *screen, smart_home_lvgl_t *ui, const char *title)
{ (void)screen; (void)ui; (void)title; }
lv_obj_t *smart_home_lvgl_build_nav_bar(lv_obj_t *screen, smart_home_lvgl_t *ui)
{ (void)screen; (void)ui; return NULL; }
void smart_home_lvgl_update_clock(smart_home_lvgl_t *ui) { (void)ui; }
static void finish_save(smart_home_lvgl_t *ui)
{
    release_service(0);
    for (int i = 0; i < 3000 && g_wake.active; i++) {
        smart_home_lvgl_wake_settings_poll(ui);
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    assert(!g_wake.active);
}
int main(void)
{
    static uint16_t pixels[1024 * 600];
    smart_home_agent_app_t app = {0};
    smart_home_lvgl_t ui = {.app = &app};
    lv_init();
    lv_display_t *display = lv_display_create(1024, 600);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, flush);
    assert(pthread_mutex_init(&ui.pending_mutex, NULL) == 0);
    assert(smart_home_lvgl_style_init() == 0);
    smart_home_lvgl_build_settings_screen_v9(&ui);
    lv_screen_load(ui.screen_settings_v9);
    lv_obj_send_event(g_settings.button[V9_CAT_VOICE], LV_EVENT_CLICKED, NULL);
    lv_obj_update_layout(ui.screen_settings_v9);
    assert(g_wake.toggle && lv_obj_is_visible(g_wake.toggle));
    gates[0].result = AGENT_ERROR_IO;
    lv_obj_add_state(g_wake.toggle, LV_STATE_CHECKED);
    lv_obj_send_event(g_wake.toggle, LV_EVENT_VALUE_CHANGED, NULL);
    wait_entered(0);
    assert(g_wake.active && !pthread_equal(gates[0].thread, pthread_self()));
    lv_tick_inc(300);
    lv_timer_handler();
    finish_save(&ui);
    assert(!lv_obj_has_state(g_wake.toggle, LV_STATE_CHECKED) && paused == -1);
    gates[0].result = AGENT_OK;
    lv_obj_add_state(g_wake.toggle, LV_STATE_CHECKED);
    lv_obj_send_event(g_wake.toggle, LV_EVENT_VALUE_CHANGED, NULL);
    finish_save(&ui);
    assert(g_wake.applied && paused == 0);
    smart_home_lvgl_settings_v9_deinit(&ui);
    lv_obj_delete(ui.screen_settings_v9);
    pthread_mutex_destroy(&ui.pending_mutex);
    puts("PASS: KWS-only V9 entry, nonblocking save, rollback and runtime resume");
    return 0;
}
