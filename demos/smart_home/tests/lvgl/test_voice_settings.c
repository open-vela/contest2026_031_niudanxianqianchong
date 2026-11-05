/* SPDX-License-Identifier: Apache-2.0 */
/* Real V9 screens and voice callbacks. Service doubles isolate UI/lifetime tests. */
#include <pthread.h>
#include <assert.h>
#include <errno.h>
static int finish_before_create_returns;
static int fast_done;
static pthread_mutex_t fast_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fast_cond = PTHREAD_COND_INITIALIZER;
struct fast_call { void *(*function)(void *); void *argument; };
static void *fast_worker(void *argument)
{
    struct fast_call call = *(struct fast_call *)argument;
    call.function(call.argument);
    pthread_mutex_lock(&fast_mutex);
    fast_done = 1;
    pthread_cond_signal(&fast_cond);
    pthread_mutex_unlock(&fast_mutex);
    return NULL;
}
static int fast_create(pthread_t *thread, const pthread_attr_t *attr,
                       void *(*function)(void *), void *argument)
{
    if (!finish_before_create_returns) return pthread_create(thread, attr, function, argument);
    struct fast_call call = {function, argument};
    fast_done = 0;
    int ret = pthread_create(thread, attr, fast_worker, &call);
    if (ret) return ret;
    pthread_mutex_lock(&fast_mutex);
    while (!fast_done) pthread_cond_wait(&fast_cond, &fast_mutex);
    pthread_mutex_unlock(&fast_mutex);
    return 0;
}
#define pthread_create fast_create
#define main settings_fixture_main
#define SMART_HOME_TEST_HAS_MILOCO_STUBS
#define SMART_HOME_TEST_HAS_VOICE_SECRET_STUB
#include "test_settings_v9.c"
#undef main
#include "../../src/voice/smart_home_asr.h"
#include "../../src/voice/smart_home_tts.h"
#include "../../src/voice/smart_home_voice_play.h"
#include "../../src/voice/smart_home_voice_player.h"
#include "smart_home_lvgl_voice_settings.c"
#ifdef CONFIG_SMART_HOME_KWS
#include "smart_home_lvgl_wake_settings.c"
void smart_home_kws_config_load(smart_home_kws_config_t *config) { config->enabled = 0; }
int smart_home_kws_config_save(const smart_home_kws_config_t *config) { (void)config; return AGENT_OK; }
void kws_service_set_paused(int paused) { (void)paused; }
int kws_service_running(void) { return 1; }
#endif
#undef pthread_create

/* Services outside the voice/model panels are not exercised here. */
int smart_home_agent_app_set_local_tool_access(smart_home_agent_app_t *app,
    const char *name, int allowed) { (void)app; (void)name; (void)allowed; return 0; }
bool smart_home_miloco_config_valid(const smart_home_miloco_config_t *config)
{ (void)config; return false; }
int smart_home_miloco_request_save(smart_home_miloco_t *service,
    const smart_home_miloco_config_t *config) { (void)service; (void)config; return 0; }
void smart_home_lvgl_open_miloco_bind(smart_home_lvgl_t *ui) { (void)ui; }
void smart_home_lvgl_build_top_bar(lv_obj_t *screen,smart_home_lvgl_t *ui,
    const char *title) { (void)screen; (void)ui; (void)title; }
lv_obj_t *smart_home_lvgl_build_nav_bar(lv_obj_t *screen,smart_home_lvgl_t *ui)
{ (void)ui; return lv_obj_create(screen); }
void smart_home_lvgl_update_clock(smart_home_lvgl_t *ui) { (void)ui; }

static int save_result, tts_code, play_code, key_calls;
static smart_home_secret_service_t saved_service;
static char saved_key[256];
static smart_home_asr_config_t stored_asr = {.backend_id="mimo", .host="test.invalid",
    .path="/v1/chat/completions", .port="443", .model="asr", .language="zh"};
static smart_home_tts_config_t stored_tts = {.backend_id="mimo", .host="test.invalid",
    .path="/v1/chat/completions", .port="443", .model="tts", .voice="mimo_default"};
static const smart_home_asr_preset_t asr_preset = {"mimo","MiMo ASR","test.invalid","/chat","443","asr"};
static const smart_home_tts_preset_t tts_preset = {"mimo","MiMo TTS","test.invalid","/chat","443","tts","mimo_default",1};
size_t smart_home_asr_preset_count(void) { return 1; }
size_t smart_home_tts_preset_count(void) { return 1; }
const smart_home_asr_preset_t *smart_home_asr_preset_get(size_t i) { return i==0?&asr_preset:NULL; }
const smart_home_tts_preset_t *smart_home_tts_preset_get(size_t i) { return i==0?&tts_preset:NULL; }
void smart_home_asr_config_load(smart_home_asr_config_t *c) { *c=stored_asr; }
void smart_home_tts_config_load(smart_home_tts_config_t *c) { *c=stored_tts; }
int smart_home_asr_config_save(const smart_home_asr_config_t *c)
{ if (!save_result) stored_asr=*c; return save_result; }
int smart_home_tts_config_save(const smart_home_tts_config_t *c)
{ if (!save_result) stored_tts=*c; return save_result; }
const char *smart_home_secrets_error_text(int code) { return code==0?"configured":"credential error"; }
int smart_home_secrets_api_key_status(smart_home_secret_service_t s, const char *b)
{ (void)s; (void)b; return 0; }
int smart_home_secrets_set_api_key(smart_home_secret_service_t s,const char *b,const char *key)
{ assert(b); saved_service=s; snprintf(saved_key,sizeof(saved_key),"%s",key); key_calls++; return 0; }
static int voice_stop_calls;
void voice_play_stop(void) { voice_stop_calls++; }
voice_play_state_t voice_play_state(void) { return VOICE_PLAY_IDLE; }
int voice_capture_record(int seconds,const volatile int *abort,uint8_t **pcm,size_t *bytes)
{ (void)seconds; (void)abort; *pcm=malloc(3200); *bytes=3200; return 0; }
int smart_home_asr_recognize_ex(const uint8_t *p,size_t b,char *text,size_t n,smart_home_voice_result_t *result)
{ (void)p; (void)b; snprintf(text,n,"测试识别"); *result=(smart_home_voice_result_t){0,200,SMART_HOME_VOICE_RESPONSE}; return 0; }
int smart_home_tts_synth_ex(const char *text,uint8_t **pcm,size_t *bytes,uint32_t *rate,smart_home_voice_result_t *result)
{
    (void)text;
    *result=(smart_home_voice_result_t){tts_code,tts_code?401:200,
        tts_code?SMART_HOME_VOICE_HTTP:SMART_HOME_VOICE_RESPONSE};
    *pcm=tts_code?NULL:malloc(320); *bytes=tts_code?0:320; *rate=16000;
    return tts_code;
}
int smart_home_voice_player_play_mem(const uint8_t *p,size_t b,uint32_t rate,smart_home_voice_abort_t *abort)
{ (void)p; (void)b; (void)rate; (void)abort; return play_code; }

static void press_voice(struct voice_controls_s *v, enum voice_job_action action)
{
    lv_obj_t *button=lv_button_create(g_settings.screen);
    lv_obj_set_user_data(button,(void *)(intptr_t)action);
    lv_obj_add_event_cb(button,voice_button_cb,LV_EVENT_CLICKED,v);
    lv_obj_send_event(button,LV_EVENT_CLICKED,NULL);
    assert(g_voice_job.active && g_voice_job.ready);
    smart_home_lvgl_voice_settings_poll(v->ui);
    assert(!g_voice_job.active);
    lv_obj_delete(button);
}

int main(void)
{
    static uint16_t pixels[1024*600];
    smart_home_agent_app_t app={0};
    lv_init();
    lv_display_t *display=lv_display_create(1024,600);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display,flush);
    assert(smart_home_lvgl_style_init()==0);
    pthread_mutex_init(&app.agent_mutex,NULL);
    app.mcp_bridge=(smart_home_mcp_bridge_t *)&app;
    smart_home_lvgl_t *ui=calloc(1,sizeof(*ui));
    assert(ui);
    pthread_mutex_init(&ui->pending_mutex,NULL);
    ui->app=&app;
    smart_home_lvgl_build_settings_screen_v9(ui);
    lv_screen_load(ui->screen_settings_v9);
    lv_obj_send_event(g_settings.button[V9_CAT_VOICE], LV_EVENT_CLICKED, NULL);
    lv_obj_update_layout(ui->screen_settings_v9);
    assert(!lv_obj_has_flag(g_settings.panel[V9_CAT_VOICE], LV_OBJ_FLAG_SCROLLABLE));
    for (int i = 0; i < 2; ++i) {
        lv_obj_send_event(g_voice_page.tabs[i], LV_EVENT_CLICKED, NULL);
        lv_obj_update_layout(ui->screen_settings_v9);
        assert(g_voice_page.selected == i);
        assert(!lv_obj_has_flag(g_voice_page.panel[i], LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_has_flag(g_voice_page.panel[1-i], LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_get_scroll_bottom(g_settings.panel[V9_CAT_VOICE]) <= 0);
        assert(lv_obj_get_scroll_y(g_settings.panel[V9_CAT_VOICE]) == 0);
        if (!g_voice[i].basic) {
            assert(lv_obj_get_child_count(g_voice_page.panel[i]) == 2);
            continue;
        }
        struct voice_controls_s *v = &g_voice[i];
        assert(v->keyboard == g_settings.keyboard);
        lv_textarea_set_text(v->model, i == 0 ? "asr-draft" : "tts-draft");
        lv_obj_send_event(v->model, LV_EVENT_FOCUSED, NULL);
        assert(lv_keyboard_get_textarea(g_settings.keyboard) == v->model);
        lv_obj_send_event(g_settings.keyboard, LV_EVENT_READY, NULL);
        lv_obj_t *connection_button = NULL;
        for (uint32_t j = 0; j < lv_obj_get_child_count(v->basic); ++j) {
            lv_obj_t *child = lv_obj_get_child(v->basic, j);
            if (lv_obj_check_type(child, &lv_button_class) &&
                !strcmp(lv_label_get_text(lv_obj_get_child(child, 0)), "连接设置")) connection_button = child;
        }
#ifdef CONFIG_SMART_HOME_VOICE_TTS
        if (i == 1) {
            lv_obj_t *stop = NULL;
            for (uint32_t j = 0; j < lv_obj_get_child_count(v->basic); j++) {
                lv_obj_t *child = lv_obj_get_child(v->basic, j);
                if (lv_obj_check_type(child, &lv_button_class) &&
                    !strcmp(lv_label_get_text(lv_obj_get_child(child, 0)), "停止播报")) stop = child;
            }
            assert(stop);
            lv_obj_send_event(stop, LV_EVENT_CLICKED, NULL);
            assert(voice_stop_calls == 1);
        }
#endif
        assert(connection_button);
        lv_obj_send_event(connection_button, LV_EVENT_CLICKED, NULL);
        assert(lv_obj_has_flag(v->basic, LV_OBJ_FLAG_HIDDEN));
        assert(!lv_obj_has_flag(v->connection, LV_OBJ_FLAG_HIDDEN));
        lv_obj_send_event(lv_obj_get_child(v->connection, -1), LV_EVENT_CLICKED, NULL);
        assert(!lv_obj_has_flag(v->basic, LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_has_flag(v->connection, LV_OBJ_FLAG_HIDDEN));
        assert(!strcmp(lv_textarea_get_text(v->model), i == 0 ? "asr-draft" : "tts-draft"));
        lv_obj_update_layout(ui->screen_settings_v9);
        lv_area_t body, field;
        lv_obj_get_coords(g_settings.panel[V9_CAT_VOICE], &body);
        lv_obj_get_coords(v->save, &field);
        assert(field.y2 <= body.y2 && field.x2 <= body.x2);
        lv_area_t title;
        lv_obj_t *heading = lv_obj_get_child(g_settings.panel[V9_CAT_VOICE], 0);
        lv_obj_get_coords(heading, &title);
        assert(lv_obj_is_visible(heading) && title.y1 >= body.y1);
        printf("VOICE %s: heading=(%d,%d)-(%d,%d), panel=(%d,%d)-(%d,%d), save bottom=%d\n",
            i ? "TTS" : "ASR", title.x1, title.y1, title.x2, title.y2,
            body.x1, body.y1, body.x2, body.y2, field.y2);
#ifdef CONFIG_SMART_HOME_VOICE_TTS
        if (i == 1) {
            lv_area_t keyboard;
            lv_obj_send_event(v->option, LV_EVENT_FOCUSED, NULL);
            lv_obj_update_layout(ui->screen_settings_v9);
            lv_obj_get_coords(v->option, &field);
            lv_obj_get_coords(g_settings.keyboard, &keyboard);
            assert(field.y2 < keyboard.y1);
            lv_obj_send_event(g_settings.keyboard, LV_EVENT_READY, NULL);
        }
#endif
        lv_obj_invalidate(ui->screen_settings_v9);
        lv_tick_inc(32);
        lv_timer_handler();
        lv_refr_now(NULL);
        snapshot(pixels, i == 0 ? "SMART_HOME_VOICE_ASR_CAPTURE" : "SMART_HOME_VOICE_TTS_CAPTURE");
    }
    puts("PASS: ASR/TTS click tabs, draft preservation, connection views and non-scrolling layout");
    puts("PASS: actual V9 voice fields share an initialized keyboard");

    finish_before_create_returns=1;
#ifdef CONFIG_SMART_HOME_VOICE_ASR
    for (int i=0;i<30;i++) {
        lv_obj_add_state(g_voice[0].enabled,LV_STATE_CHECKED);
        press_voice(&g_voice[0],VOICE_SAVE);
        assert(stored_asr.enabled);
    }
    puts("PASS: worker completes and frees its job before pthread_create returns");
    save_result=AGENT_ERROR;
    lv_obj_remove_state(g_voice[0].enabled,LV_STATE_CHECKED);
    press_voice(&g_voice[0],VOICE_SAVE);
    assert(lv_obj_has_state(g_voice[0].enabled,LV_STATE_CHECKED));
    save_result=0;
#endif

#ifdef CONFIG_SMART_HOME_VOICE_TTS
    tts_code=AGENT_ERROR_MODEL_FAILED;
    press_voice(&g_voice[1],VOICE_TEST);
    assert(strstr(lv_label_get_text(g_voice[1].status),"鉴权"));
    tts_code=0; play_code=-EINVAL;
    press_voice(&g_voice[1],VOICE_TEST);
    assert(strstr(lv_label_get_text(g_voice[1].status),"扬声器"));
    play_code=0;
    press_voice(&g_voice[1],VOICE_TEST);
    assert(strstr(lv_label_get_text(g_voice[1].status),"测试完成"));
    puts("PASS: TTS HTTP and playback failures remain distinct; retry succeeds");

#endif

#ifdef CONFIG_SMART_HOME_KWS
    lv_obj_send_event(g_voice_page.tabs[2], LV_EVENT_CLICKED, NULL);
    lv_obj_update_layout(ui->screen_settings_v9);
    assert(g_voice_page.selected == 2 && lv_obj_is_visible(g_wake.toggle));
#endif
    lv_textarea_set_text(g_settings.model_key,"host-test-llm-key");
    lv_obj_t *button=lv_button_create(g_settings.screen);
    lv_obj_add_event_cb(button,model_save_key_cb,LV_EVENT_CLICKED,ui);
    lv_obj_send_event(button,LV_EVENT_CLICKED,NULL);
    smart_home_lvgl_voice_settings_poll(ui);
    assert(key_calls==1 && saved_service==SMART_HOME_SECRET_LLM);
    assert(!strcmp(saved_key,"host-test-llm-key"));
    assert(lv_textarea_get_text(g_settings.model_key)[0]==0);
    assert(strstr(lv_label_get_text(g_settings.model_status),"LLM"));
    puts("PASS: LLM persistent-save callback updates only the LLM namespace");
    smart_home_lvgl_settings_v9_deinit(ui);
    lv_obj_delete(ui->screen_settings_v9);
    pthread_mutex_destroy(&ui->pending_mutex);
    free(ui);
    pthread_mutex_destroy(&app.agent_mutex);
    return 0;
}
