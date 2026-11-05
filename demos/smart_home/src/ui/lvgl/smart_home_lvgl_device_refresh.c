/* SPDX-License-Identifier: Apache-2.0 */
/* V9 device refresh without constructing a legacy device page. */
#include "smart_home_lvgl_internal.h"
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
#include "../../miloco/smart_home_miloco.h"
#endif

void smart_home_lvgl_refresh_cards(smart_home_lvgl_t *ui)
{
    if (ui) smart_home_lvgl_refresh_home(ui);
}

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
static void device_revision_timer(lv_timer_t *timer)
{
    smart_home_lvgl_t *ui = lv_timer_get_user_data(timer);
    uint32_t revision = 0;
    if (!ui || !ui->app || !ui->app->miloco) return;
    (void)smart_home_miloco_list(ui->app->miloco, NULL, 0, &revision);
    if (revision != ui->miloco_polled_revision) {
        ui->miloco_polled_revision = revision;
        if (ui->active_tab == SMART_HOME_TAB_HOME) smart_home_lvgl_refresh_home(ui);
    }
}

void smart_home_lvgl_miloco_poll_set_enabled(smart_home_lvgl_t *ui, int enable)
{
    if (!ui) return;
    if (enable && !ui->miloco_timer)
        ui->miloco_timer = lv_timer_create(device_revision_timer, 1000, ui);
    else if (!enable && ui->miloco_timer) {
        lv_timer_delete(ui->miloco_timer);
        ui->miloco_timer = NULL;
    }
}
#endif
