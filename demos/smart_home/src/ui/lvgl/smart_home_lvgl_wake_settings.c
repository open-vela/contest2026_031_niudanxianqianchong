/* SPDX-License-Identifier: Apache-2.0 */
#include "smart_home_lvgl_internal.h"
#ifdef CONFIG_SMART_HOME_KWS
#include "../../voice/smart_home_kws_service.h"
#include <stdlib.h>
#include <string.h>

static struct {
    smart_home_lvgl_t *owner;
    lv_obj_t *toggle, *status;
    pthread_t thread;
    smart_home_kws_config_t config;
    int active, ready, result, applied;
} g_wake;

static void *wake_save_worker(void *argument)
{
    smart_home_lvgl_t *ui = argument;
    int ret = smart_home_kws_config_save(&g_wake.config);
    pthread_mutex_lock(&ui->pending_mutex);
    g_wake.result = ret;
    g_wake.ready = 1;
    pthread_mutex_unlock(&ui->pending_mutex);
    return NULL;
}

static void wake_switch_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    pthread_attr_t attr;
    int ret;
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || g_wake.active) return;
    g_wake.config.enabled = lv_obj_has_state(g_wake.toggle, LV_STATE_CHECKED);
    g_wake.active = 1;
    ret = pthread_attr_init(&attr);
    if (ret == 0) {
        ret = pthread_attr_setstacksize(&attr, 16384u);
        if (ret == 0) ret = pthread_create(&g_wake.thread, &attr, wake_save_worker, ui);
        pthread_attr_destroy(&attr);
    }
    if (ret != 0) {
        g_wake.active = 0;
        if (g_wake.applied) lv_obj_add_state(g_wake.toggle, LV_STATE_CHECKED);
        else lv_obj_remove_state(g_wake.toggle, LV_STATE_CHECKED);
        lv_label_set_text(g_wake.status, "无法创建保存任务");
        return;
    }
    lv_obj_add_state(g_wake.toggle, LV_STATE_DISABLED);
    lv_label_set_text(g_wake.status, "正在保存唤醒设置…");
}

void smart_home_lvgl_wake_settings_poll(smart_home_lvgl_t *ui)
{
    int ready;
    if (g_wake.owner != ui) return;
    pthread_mutex_lock(&ui->pending_mutex);
    ready = g_wake.ready;
    g_wake.ready = 0;
    pthread_mutex_unlock(&ui->pending_mutex);
    if (!ready) return;
    pthread_join(g_wake.thread, NULL);
    g_wake.active = 0;
    lv_obj_remove_state(g_wake.toggle, LV_STATE_DISABLED);
    if (g_wake.result == AGENT_OK) {
        g_wake.applied = g_wake.config.enabled;
        if (kws_service_running()) {
            kws_service_set_paused(!g_wake.applied);
            lv_label_set_text(g_wake.status, g_wake.applied ? "语音唤醒已开启" : "语音唤醒已关闭");
        } else lv_label_set_text(g_wake.status, g_wake.applied ?
            "配置已保存，唤醒服务将在重启后启动" : "语音唤醒已关闭");
    } else {
        if (g_wake.applied) lv_obj_add_state(g_wake.toggle, LV_STATE_CHECKED);
        else lv_obj_remove_state(g_wake.toggle, LV_STATE_CHECKED);
        lv_label_set_text(g_wake.status, "唤醒配置保存失败，已恢复原设置");
    }
}

void smart_home_lvgl_build_wake_settings(smart_home_lvgl_t *ui, lv_obj_t *panel)
{
    g_wake.owner = ui;
    smart_home_kws_config_load(&g_wake.config);
    g_wake.applied = g_wake.config.enabled;
    lv_obj_t *label = smart_home_lvgl_label_create(panel, "语音唤醒 KWS",
        SMART_HOME_UI_COLOR_TEXT_PRIMARY, 20);
    lv_obj_set_pos(label, 0, 16);
    label = smart_home_lvgl_label_create(panel,
        "启用后监听唤醒词。启动时关闭的服务，需要重启后启用。",
        SMART_HOME_UI_COLOR_TEXT_SECONDARY, 14);
    lv_obj_set_pos(label, 0, 64);
    lv_obj_set_width(label, 610);
    g_wake.toggle = lv_switch_create(panel);
    lv_obj_set_size(g_wake.toggle, 54, 30);
    lv_obj_set_pos(g_wake.toggle, 640, 20);
    if (g_wake.applied) lv_obj_add_state(g_wake.toggle, LV_STATE_CHECKED);
    lv_obj_add_event_cb(g_wake.toggle, wake_switch_cb, LV_EVENT_VALUE_CHANGED, ui);
    g_wake.status = smart_home_lvgl_label_create(panel, g_wake.applied ?
        (kws_service_running() ? "唤醒服务已启动" : "唤醒服务未启动") : "语音唤醒已关闭",
        SMART_HOME_UI_COLOR_TEXT_MUTED, 14);
    lv_obj_set_pos(g_wake.status, 0, 140);
}

void smart_home_lvgl_wake_settings_deinit(smart_home_lvgl_t *ui)
{
    if (g_wake.owner != ui) return;
    if (g_wake.active) pthread_join(g_wake.thread, NULL);
    memset(&g_wake, 0, sizeof(g_wake));
}
#endif
