/* SPDX-License-Identifier: Apache-2.0 */
#include "smart_home_lvgl_internal.h"
#include "../../reminder/smart_home_reminder.h"
#ifdef CONFIG_SMART_HOME_REMINDER_RING
#include "../../voice/smart_home_voice_play.h"
#endif

static void reminder_close(smart_home_lvgl_t *ui)
{
#ifdef CONFIG_SMART_HOME_REMINDER_RING
    if (ui->reminder_id) voice_play_alarm_stop(ui->reminder_id);
#endif
    if (ui->reminder_popup) lv_obj_delete(ui->reminder_popup);
    ui->reminder_popup = NULL;
    ui->reminder_status = NULL;
    ui->reminder_button = NULL;
    ui->reminder_id = 0;
    ui->reminder_ack_pending = 0;
}

static void acknowledge(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
#ifdef CONFIG_SMART_HOME_REMINDER_RING
    voice_play_alarm_stop(ui->reminder_id);
#endif
    if (smart_home_reminder_ack(ui->reminder_id) == AGENT_OK) {
        ui->reminder_ack_pending = 1;
        ui->reminder_ack_tick = lv_tick_get();
        lv_obj_add_state(ui->reminder_button,LV_STATE_DISABLED);
        lv_label_set_text(ui->reminder_status,"正在保存确认…");
    } else lv_label_set_text(ui->reminder_status,"确认失败，请重试");
}

void smart_home_lvgl_reminder_poll(smart_home_lvgl_t *ui)
{
    smart_home_reminder_snapshot_t snapshot;
    smart_home_reminder_t *next = NULL;
    lv_obj_t *card, *label;
    int i;
    if (!ui) return;
    smart_home_reminder_snapshot(&snapshot);
    if (!snapshot.ready) return;
    for (i = 0; i < snapshot.count; ++i) {
        smart_home_reminder_t *r = &snapshot.items[i];
        if (r->state == SMART_HOME_REMINDER_ACTIVE) continue;
        if (!next || r->id == ui->reminder_id) next = r;
        if (r->id == ui->reminder_id) break;
    }
    if (ui->reminder_id && (!next || next->id != ui->reminder_id)) reminder_close(ui);
    if (!next) return;
    if (ui->reminder_popup) {
        if (ui->reminder_ack_pending && lv_tick_elaps(ui->reminder_ack_tick) > 2000u) {
            ui->reminder_ack_pending = 0;
            lv_obj_remove_state(ui->reminder_button,LV_STATE_DISABLED);
            lv_label_set_text(ui->reminder_status,"未能保存确认，请重试并检查存储日志");
        }
#ifdef CONFIG_SMART_HOME_REMINDER_RING
        if (!ui->reminder_ack_pending && voice_play_alarm_error(ui->reminder_id) < 0)
            lv_label_set_text(ui->reminder_status,"本地响铃失败，请检查音频设备日志");
#endif
        return;
    }
    ui->reminder_id = next->id;
    lv_display_trigger_activity(lv_display_get_default());
    ui->reminder_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ui->reminder_popup,LV_PCT(100),LV_PCT(100));
    lv_obj_set_style_bg_color(ui->reminder_popup,lv_color_hex(0x18252c),0);
    lv_obj_set_style_bg_opa(ui->reminder_popup,LV_OPA_60,0);
    lv_obj_remove_flag(ui->reminder_popup,LV_OBJ_FLAG_SCROLLABLE);
    card = lv_obj_create(ui->reminder_popup);
    lv_obj_set_width(card,smart_home_lvgl_disp_w() < 600 ? LV_PCT(90) : 520);
    lv_obj_set_height(card,LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_pad_all(card,24,0);
    lv_obj_set_style_pad_row(card,18,0);
    lv_obj_set_flex_flow(card,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(card,SMART_HOME_UI_COLOR_SURFACE,0);
    label = smart_home_lvgl_label_create(card,next->name,SMART_HOME_UI_COLOR_TEXT_PRIMARY,24);
    lv_obj_set_width(label,LV_PCT(100));
    label = smart_home_lvgl_label_create(card,next->message,SMART_HOME_UI_COLOR_TEXT_PRIMARY,20);
    lv_obj_set_width(label,LV_PCT(100));
    lv_label_set_long_mode(label,LV_LABEL_LONG_WRAP);
    ui->reminder_status = smart_home_lvgl_label_create(card,
        next->state == SMART_HOME_REMINDER_MISSED ? "已错过的提醒（静音）" :
        !next->audible ? "重启恢复的提醒（静音）" : "提醒已到期",
        SMART_HOME_UI_COLOR_TEXT_SECONDARY,16);
    lv_obj_set_width(ui->reminder_status,LV_PCT(100));
    ui->reminder_button = lv_button_create(card);
    lv_obj_set_size(ui->reminder_button,LV_PCT(100),48);
    label = smart_home_lvgl_label_create(ui->reminder_button,"知道了",SMART_HOME_UI_COLOR_TEXT_PRIMARY,18);
    lv_obj_center(label);
    lv_obj_add_event_cb(ui->reminder_button,acknowledge,LV_EVENT_CLICKED,ui);
#ifdef CONFIG_SMART_HOME_REMINDER_RING
    if (next->audible) {
        if (voice_play_alarm(next->id) != 0)
            lv_label_set_text(ui->reminder_status,"无法启动本地响铃，请检查音频服务");
        else lv_label_set_text(ui->reminder_status,"本地提示音 5 秒，点击确认关闭提醒");
    }
#endif
}

static void reminder_timer(lv_timer_t *timer)
{
    smart_home_lvgl_reminder_poll(lv_timer_get_user_data(timer));
}

void smart_home_lvgl_reminder_init(smart_home_lvgl_t *ui)
{
    ui->reminder_timer = lv_timer_create(reminder_timer,250,ui);
}

void smart_home_lvgl_reminder_deinit(smart_home_lvgl_t *ui)
{
    if (ui->reminder_timer) lv_timer_delete(ui->reminder_timer);
    ui->reminder_timer = NULL;
    reminder_close(ui);
}
