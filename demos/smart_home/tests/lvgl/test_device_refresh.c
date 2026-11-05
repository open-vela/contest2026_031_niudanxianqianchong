/* Product refresh path with no legacy page allocated. */
#include "smart_home_lvgl_device_refresh.c"
#include <assert.h>
static uint32_t revision;
static int refreshes;
void smart_home_lvgl_refresh_home(smart_home_lvgl_t *ui) { assert(ui); refreshes++; }
size_t smart_home_miloco_list(const smart_home_miloco_t *service,
    smart_home_miloco_device_t *devices, size_t capacity, uint32_t *out)
{ (void)service; (void)devices; (void)capacity; if (out) *out = revision; return 0; }
int main(void)
{
    smart_home_agent_app_t app = {0};
    smart_home_lvgl_t ui = {.app = &app, .active_tab = SMART_HOME_TAB_HOME};
    app.miloco = (smart_home_miloco_t *)&app;
    lv_init();
    smart_home_lvgl_miloco_poll_set_enabled(&ui, 1);
    assert(ui.miloco_timer);
    lv_timer_t *timer = ui.miloco_timer;
    smart_home_lvgl_miloco_poll_set_enabled(&ui, 1);
    assert(ui.miloco_timer == timer);
    revision++;
    device_revision_timer(timer);
    assert(refreshes == 1);
    device_revision_timer(timer);
    assert(refreshes == 1);
    smart_home_lvgl_refresh_cards(&ui);
    assert(refreshes == 2);
    smart_home_lvgl_miloco_poll_set_enabled(&ui, 0);
    assert(!ui.miloco_timer);
    lv_deinit();
    return 0;
}
