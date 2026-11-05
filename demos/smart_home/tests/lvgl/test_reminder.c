/* SPDX-License-Identifier: Apache-2.0 */
#include "smart_home_lvgl_internal.h"
#include "reminder/smart_home_reminder.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static smart_home_reminder_snapshot_t snapshot;
static int rings, stops, acknowledgements, ring_error;
static uint16_t pixels[1024*80];
void smart_home_reminder_snapshot(smart_home_reminder_snapshot_t *out) { *out=snapshot; }
int smart_home_reminder_ack(int id) { assert(id>0); ++acknowledgements; return 0; }
int voice_play_alarm(int id) { assert(id>0); ++rings; return 0; }
void voice_play_alarm_stop(int id) { assert(id>0); ++stops; }
int voice_play_alarm_error(int id) { (void)id; return ring_error; }
static void flush(lv_display_t *d,const lv_area_t *area,uint8_t *data)
{ (void)area; (void)data; lv_display_flush_ready(d); }

int main(void)
{
    smart_home_lvgl_t ui={0};
    lv_init();
    lv_display_t *display=lv_display_create(1024,600);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    smart_home_lvgl_reminder_init(&ui);
    snapshot.ready=1;
    snapshot.count=2;
    snapshot.items[0]=(smart_home_reminder_t){.id=1,.state=SMART_HOME_REMINDER_DUE,.audible=1};
    strcpy(snapshot.items[0].name,"Countdown"); strcpy(snapshot.items[0].message,"Drink water");
    snapshot.items[1]=(smart_home_reminder_t){.id=2,.state=SMART_HOME_REMINDER_MISSED};
    strcpy(snapshot.items[1].name,"Missed"); strcpy(snapshot.items[1].message,"Alarm");
    smart_home_lvgl_reminder_poll(&ui);
    assert(ui.reminder_popup && ui.reminder_id==1 && rings==1);
    smart_home_lvgl_reminder_poll(&ui);
    assert(rings==1);
    lv_obj_t *screen=lv_obj_create(NULL);
    lv_screen_load(screen);
    assert(lv_obj_get_parent(ui.reminder_popup)==lv_layer_top());
    ring_error=-1;
    smart_home_lvgl_reminder_poll(&ui);
    assert(strstr(lv_label_get_text(ui.reminder_status),"失败"));
    ring_error=0;
    lv_obj_send_event(ui.reminder_button,LV_EVENT_CLICKED,NULL);
    assert(acknowledgements==1 && ui.reminder_ack_pending && stops==1);
    lv_tick_inc(2100);
    smart_home_lvgl_reminder_poll(&ui);
    assert(!ui.reminder_ack_pending); /* failed durable acknowledgement stays visible */
    assert(!lv_obj_has_state(ui.reminder_button,LV_STATE_DISABLED));
    snapshot.items[0]=snapshot.items[1]; snapshot.count=1;
    smart_home_lvgl_reminder_poll(&ui);
    assert(ui.reminder_id==2 && rings==1);
    snapshot.count=0;
    smart_home_lvgl_reminder_poll(&ui);
    assert(!ui.reminder_popup);
    snapshot.count=1;
    snapshot.items[0].state=SMART_HOME_REMINDER_DUE;
    snapshot.items[0].audible=0;
    smart_home_lvgl_reminder_poll(&ui);
    assert(ui.reminder_popup && rings==1); /* restored due popup stays silent */
    smart_home_lvgl_reminder_deinit(&ui);
    assert(!ui.reminder_popup && !ui.reminder_timer);
    lv_display_delete(display);
    lv_deinit();
    puts("PASS: real LVGL popup, multiple reminders, screen switching, ack retry and cleanup");
    return 0;
}
