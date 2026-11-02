/* SPDX-License-Identifier: Apache-2.0 */
/* Independent voice credentials and configuration, with file/network work off LVGL. */
#include "smart_home_lvgl_internal.h"
#include "../../config/smart_home_secrets.h"
#include "../../smart_home_memory.h"
#ifdef CONFIG_SMART_HOME_VOICE_ASR
#include "../../voice/smart_home_asr.h"
#include "../../voice/smart_home_voice_capture.h"
#endif
#ifdef CONFIG_SMART_HOME_VOICE_TTS
#include "../../voice/smart_home_tts.h"
#include "../../voice/smart_home_voice_player.h"
#include "../../voice/smart_home_voice_play.h"
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nuttx/irq.h>

struct voice_controls_s {
    smart_home_secret_service_t service;
    smart_home_lvgl_t *ui;
    lv_obj_t *keyboard, *backend, *model, *host, *path, *port, *option;
    lv_obj_t *key, *enabled, *status, *save;
};
static struct voice_controls_s g_voice[2];
static pthread_mutex_t g_voice_job_mutex = PTHREAD_MUTEX_INITIALIZER;
enum voice_job_action { VOICE_SAVE, VOICE_KEY, VOICE_TEST };
struct voice_job_s {
    smart_home_lvgl_t *ui;
    smart_home_secret_service_t service;
    enum voice_job_action action;
    char backend[32], key[256];
    int update_key;
#ifdef CONFIG_SMART_HOME_VOICE_ASR
    smart_home_asr_config_t asr;
#endif
#ifdef CONFIG_SMART_HOME_VOICE_TTS
    smart_home_tts_config_t tts;
#endif
};
static struct {
    pthread_t thread;
    smart_home_lvgl_t *owner;
    struct voice_controls_s *controls;
    lv_obj_t *status, *key_input;
    void *stack_alloc;          /* bulk 原始指针（释放用） */
    int active, ready, result, key_saved;
    char message[256];
} g_voice_job;

static void clear_key(char *key, size_t size)
{
    volatile char *p = key;
    while (size--) *p++ = 0;
}

static const char *backend_id(struct voice_controls_s *v)
{
    size_t index = lv_dropdown_get_selected(v->backend);
#ifdef CONFIG_SMART_HOME_VOICE_ASR
    if (v->service == SMART_HOME_SECRET_ASR) {
        const smart_home_asr_preset_t *p = smart_home_asr_preset_get(index);
        return p ? p->id : NULL;
    }
#endif
#ifdef CONFIG_SMART_HOME_VOICE_TTS
    if (v->service == SMART_HOME_SECRET_TTS) {
        const smart_home_tts_preset_t *p = smart_home_tts_preset_get(index);
        return p ? p->id : NULL;
    }
#endif
    return NULL;
}

static void *voice_settings_worker(void *argument)
{
    struct voice_job_s *job = argument;
    smart_home_voice_result_t result = {AGENT_OK, 0, SMART_HOME_VOICE_RESPONSE};
    char message[256] = "";
    int ret = AGENT_OK, saved = 0;
    uint8_t *pcm = NULL;
    size_t bytes = 0;
#ifdef CONFIG_SMART_HOME_VOICE_TTS
    uint32_t rate = 0;
#endif
    if (job->update_key) {
        ret = smart_home_secrets_set_api_key(job->service, job->backend, job->key);
        saved = ret == AGENT_OK;
    }
    if (ret == AGENT_OK && job->action == VOICE_SAVE) {
#ifdef CONFIG_SMART_HOME_VOICE_ASR
        if (job->service == SMART_HOME_SECRET_ASR)
            ret = smart_home_asr_config_save(&job->asr);
#endif
#ifdef CONFIG_SMART_HOME_VOICE_TTS
        if (job->service == SMART_HOME_SECRET_TTS)
            ret = smart_home_tts_config_save(&job->tts);
#endif
    }
    if (ret == AGENT_OK && job->action == VOICE_TEST) {
#ifdef CONFIG_SMART_HOME_VOICE_ASR
        if (job->service == SMART_HOME_SECRET_ASR) {
            result.stage = SMART_HOME_VOICE_CAPTURE;
            ret = voice_capture_record(4, NULL, &pcm, &bytes);
            result.code = ret;
            if (ret == AGENT_OK)
                ret = smart_home_asr_recognize_ex(pcm, bytes, message,
                                                  sizeof(message), &result);

            /* 采集服务是 ASR 与 KWS 共用的单实例，voice_capture_record()
             * 结束时已把消费者槽位还给上一个注册者。这里不能 deinit：
             * 那会销毁 worker，而常驻 KWS 不会自己重启它。 */
        }
#endif
#ifdef CONFIG_SMART_HOME_VOICE_TTS
        if (job->service == SMART_HOME_SECRET_TTS) {
            ret = smart_home_tts_synth_ex("你好，这是语音播报测试", &pcm, &bytes,
                                           &rate, &result);
            if (ret == AGENT_OK) {
                result.stage = SMART_HOME_VOICE_PLAYBACK;
                ret = smart_home_voice_player_play_mem(pcm, bytes, rate, NULL);
                result.code = ret;
            }
        }
#endif
        smart_home_bulk_free(pcm);
    }
    clear_key(job->key, sizeof(job->key));
    pthread_mutex_lock(&g_voice_job_mutex);
    g_voice_job.result = ret;
    g_voice_job.key_saved = saved;
    if (ret != AGENT_OK) {
        snprintf(g_voice_job.message, sizeof(g_voice_job.message), "%s%s (ret=%d)",
            saved ? "密钥已保存；后续操作失败：" : "",
            job->action == VOICE_TEST ? smart_home_voice_result_text(&result) :
                smart_home_secrets_error_text(ret), ret);
    } else if (job->action == VOICE_TEST) {
        snprintf(g_voice_job.message, sizeof(g_voice_job.message),
            "测试完成%s%.200s", message[0] ? "：" : "", message);
    } else if (job->action == VOICE_KEY && job->service == SMART_HOME_SECRET_LLM) {
        snprintf(g_voice_job.message, sizeof(g_voice_job.message),
            "LLM 密钥已保存；请重新应用上方模型配置");
    } else {
        snprintf(g_voice_job.message, sizeof(g_voice_job.message),
            job->action == VOICE_KEY ? "本服务密钥已清除" : "配置已保存并生效");
    }
    g_voice_job.ready = 1;
    pthread_mutex_unlock(&g_voice_job_mutex);
    clear_key((char *)job, sizeof(*job));
    free(job);
    return NULL;
}

static int start_job(struct voice_job_s *job, struct voice_controls_s *v,
                      lv_obj_t *status, lv_obj_t *key_input)
{
    /* RISC-V 的 pthread 栈必须 16 B 对齐，bulk 堆不保证（同 voice_play /
     * voice_capture / kws_service），所以保留原始指针供 deinit 释放。 */
    size_t stack_size;
    pthread_attr_t attr;
    int ret;
    const int testing = job->action == VOICE_TEST;
    int busy = g_voice_job.active || job->ui->request_inflight;
#ifdef CONFIG_SMART_HOME_VOICE_ASR
    busy = busy || job->ui->asr_active;
#endif
    if (busy) {
        lv_label_set_text(status, "请等待当前请求结束后重试");
        clear_key((char *)job, sizeof(*job));
        free(job);
        return AGENT_ERROR_BUSY;
    }
#ifdef CONFIG_SMART_HOME_VOICE_TTS
    if (voice_play_state() != VOICE_PLAY_IDLE) {
        lv_label_set_text(status, "请先停止当前语音播报");
        clear_key((char *)job, sizeof(*job));
        free(job);
        return AGENT_ERROR_BUSY;
    }
#endif
    /* 栈按最大需求一次分配：VOICE_TEST 走 TLS 握手与播放，比保存路径深。 */
    stack_size = 32768u;
    if (g_voice_job.stack_alloc == NULL) {
        g_voice_job.stack_alloc =
            smart_home_bulk_alloc(stack_size + STACK_ALIGNMENT - 1u);
        if (g_voice_job.stack_alloc == NULL) {
            clear_key((char *)job, sizeof(*job));
            free(job);
            lv_label_set_text(status, "内存不足，无法创建语音配置任务");
            return AGENT_ERROR_NOMEM;
        }
    }
    g_voice_job.owner = job->ui;
    g_voice_job.status = status;
    g_voice_job.key_input = key_input;
    g_voice_job.controls = v;
    g_voice_job.ready = 0;
    ret = pthread_attr_init(&attr);
    if (!ret) {
        ret = pthread_attr_setstack(&attr,
                                    (void *)STACK_ALIGN_UP(
                                        (uintptr_t)g_voice_job.stack_alloc),
                                    stack_size);
        if (!ret) ret = pthread_create(&g_voice_job.thread, &attr,
                                        voice_settings_worker, job);
        pthread_attr_destroy(&attr);
    }
    if (ret) {
        clear_key((char *)job, sizeof(*job));
        free(job);
        lv_label_set_text(status, "无法创建语音配置任务");
        return AGENT_ERROR;
    }
    g_voice_job.active = 1;
    lv_label_set_text(status, testing ?
        "正在测试；ASR 请连续说话约四秒" : "正在保存…");
    return AGENT_OK;
}

int smart_home_lvgl_save_llm_key(smart_home_lvgl_t *ui, const char *backend,
                                 const char *key, lv_obj_t *status, lv_obj_t *input)
{
    struct voice_job_s *job;
    if (!ui || !backend || !key || !key[0] || strlen(key) >= 256) {
        lv_label_set_text(status, "请填写要保存的 LLM 密钥");
        return AGENT_ERROR_INVALID;
    }
    job = calloc(1, sizeof(*job));
    if (!job) return AGENT_ERROR_NOMEM;
    job->ui = ui;
    job->service = SMART_HOME_SECRET_LLM;
    job->action = VOICE_KEY;
    job->update_key = 1;
    snprintf(job->backend, sizeof(job->backend), "%s", backend);
    snprintf(job->key, sizeof(job->key), "%s", key);
    return start_job(job, NULL, status, input);
}

static void voice_button_cb(lv_event_t *event)
{
    struct voice_controls_s *v = lv_event_get_user_data(event);
    struct voice_job_s *job;
    const char *id = backend_id(v);
    int action = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(event));
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !id) return;
    job = calloc(1, sizeof(*job));
    if (!job) { lv_label_set_text(v->status, "内存不足"); return; }
    job->ui = v->ui;
    job->service = v->service;
    job->action = action;
    snprintf(job->backend, sizeof(job->backend), "%s", id);
    if (action != VOICE_TEST) {
        snprintf(job->key, sizeof(job->key), "%s", lv_textarea_get_text(v->key));
        job->update_key = job->key[0] || action == VOICE_KEY;
    }
#ifdef CONFIG_SMART_HOME_VOICE_ASR
    if (v->service == SMART_HOME_SECRET_ASR) {
        smart_home_asr_config_t *c = &job->asr;
        const char *langs[] = {"zh", "en", "auto"};
        c->enabled = lv_obj_has_state(v->enabled, LV_STATE_CHECKED);
        snprintf(c->backend_id, sizeof(c->backend_id), "%s", id);
        snprintf(c->model, sizeof(c->model), "%s", lv_textarea_get_text(v->model));
        snprintf(c->host, sizeof(c->host), "%s", lv_textarea_get_text(v->host));
        snprintf(c->path, sizeof(c->path), "%s", lv_textarea_get_text(v->path));
        snprintf(c->port, sizeof(c->port), "%s", lv_textarea_get_text(v->port));
        snprintf(c->language, sizeof(c->language), "%s", langs[lv_dropdown_get_selected(v->option)]);
    }
#endif
#ifdef CONFIG_SMART_HOME_VOICE_TTS
    if (v->service == SMART_HOME_SECRET_TTS) {
        smart_home_tts_config_t *c = &job->tts;
        c->enabled = lv_obj_has_state(v->enabled, LV_STATE_CHECKED);
        snprintf(c->backend_id, sizeof(c->backend_id), "%s", id);
        snprintf(c->model, sizeof(c->model), "%s", lv_textarea_get_text(v->model));
        snprintf(c->host, sizeof(c->host), "%s", lv_textarea_get_text(v->host));
        snprintf(c->path, sizeof(c->path), "%s", lv_textarea_get_text(v->path));
        snprintf(c->port, sizeof(c->port), "%s", lv_textarea_get_text(v->port));
        snprintf(c->voice, sizeof(c->voice), "%s", lv_textarea_get_text(v->option));
        c->sample_rate = 16000;
    }
#endif
    start_job(job, v, v->status, v->key);
}

void smart_home_lvgl_voice_settings_poll(smart_home_lvgl_t *ui)
{
    int ready;
    if (!g_voice_job.active || g_voice_job.owner != ui) return;
    pthread_mutex_lock(&g_voice_job_mutex);
    ready = g_voice_job.ready;
    pthread_mutex_unlock(&g_voice_job_mutex);
    if (!ready) return;
    pthread_join(g_voice_job.thread, NULL);
    g_voice_job.active = 0;
    lv_label_set_text(g_voice_job.status, g_voice_job.message);
    if (g_voice_job.key_saved && g_voice_job.key_input)
        lv_textarea_set_text(g_voice_job.key_input, "");
    lv_obj_set_style_text_color(g_voice_job.status, g_voice_job.result == AGENT_OK ?
        SMART_HOME_UI_COLOR_SUCCESS : SMART_HOME_UI_COLOR_DANGER, 0);
    if (g_voice_job.result != AGENT_OK && g_voice_job.controls) {
        int enabled = 0;
#ifdef CONFIG_SMART_HOME_VOICE_ASR
        if (g_voice_job.controls->service == SMART_HOME_SECRET_ASR) {
            smart_home_asr_config_t c; smart_home_asr_config_load(&c); enabled = c.enabled;
        }
#endif
#ifdef CONFIG_SMART_HOME_VOICE_TTS
        if (g_voice_job.controls->service == SMART_HOME_SECRET_TTS) {
            smart_home_tts_config_t c; smart_home_tts_config_load(&c); enabled = c.enabled;
        }
#endif
        if (enabled) lv_obj_add_state(g_voice_job.controls->enabled, LV_STATE_CHECKED);
        else lv_obj_remove_state(g_voice_job.controls->enabled, LV_STATE_CHECKED);
    }
}

void smart_home_lvgl_voice_settings_deinit(smart_home_lvgl_t *ui)
{
    if (g_voice_job.owner == ui) {
        if (g_voice_job.active) pthread_join(g_voice_job.thread, NULL);
        smart_home_bulk_free(g_voice_job.stack_alloc);
        memset(&g_voice_job, 0, sizeof(g_voice_job));
    }
    memset(g_voice, 0, sizeof(g_voice));
}

static void voice_focus_cb(lv_event_t *event)
{
    struct voice_controls_s *v = lv_event_get_user_data(event);
    if (!v || !v->keyboard) return;
    lv_keyboard_set_textarea(v->keyboard, lv_event_get_target(event));
    lv_obj_clear_flag(v->keyboard, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *voice_input(struct voice_controls_s *v, lv_obj_t *panel,
                              int x, int y, int width, const char *caption,
                              const char *value, unsigned int max_length)
{
    lv_obj_t *label = smart_home_lvgl_label_create(panel, caption,
        SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
    lv_obj_t *input = lv_textarea_create(panel);
    lv_obj_set_pos(label, x, y - 23);
    lv_obj_set_pos(input, x, y);
    lv_obj_set_size(input, width, 44);
    smart_home_lvgl_style_input(input, 16);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_max_length(input, max_length);
    lv_textarea_set_text(input, value ? value : "");
    lv_obj_add_event_cb(input, voice_focus_cb, LV_EVENT_FOCUSED, v);
    return input;
}

static void voice_select_cb(lv_event_t *event)
{
    struct voice_controls_s *v = lv_event_get_user_data(event);
    const char *host = "", *path = "", *port = "443", *model = "";
    size_t index = lv_dropdown_get_selected(v->backend);
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
#ifdef CONFIG_SMART_HOME_VOICE_ASR
    if (v->service == SMART_HOME_SECRET_ASR) {
        const smart_home_asr_preset_t *p = smart_home_asr_preset_get(index);
        if (!p) return;
        host = p->host; path = p->path; port = p->port; model = p->default_model;
    }
#endif
#ifdef CONFIG_SMART_HOME_VOICE_TTS
    if (v->service == SMART_HOME_SECRET_TTS) {
        const smart_home_tts_preset_t *p = smart_home_tts_preset_get(index);
        if (!p) return;
        host = p->host; path = p->path; port = p->port; model = p->default_model;
        lv_textarea_set_text(v->option, p->default_voice);
    }
#endif
    lv_textarea_set_text(v->host, host); lv_textarea_set_text(v->path, path);
    lv_textarea_set_text(v->port, port); lv_textarea_set_text(v->model, model);
    lv_textarea_set_text(v->key, "");
    lv_label_set_text(v->status, smart_home_secrets_error_text(
        smart_home_secrets_api_key_status(v->service, backend_id(v))));
}

static void build_voice_panel(smart_home_lvgl_t *ui, lv_obj_t *panel,
                              lv_obj_t *keyboard, smart_home_secret_service_t service,
                              int y)
{
    struct voice_controls_s *v = &g_voice[service == SMART_HOME_SECRET_ASR ? 0 : 1];
    const char *id = "mimo", *host = "", *path = "", *port = "443", *model = "";
    const char *option = "";
    char choices[256] = "";
    size_t count = 0, selected = 0;
    int enabled = 0;
    lv_obj_t *label;
#ifdef CONFIG_SMART_HOME_VOICE_ASR
    smart_home_asr_config_t asr;
    if (service == SMART_HOME_SECRET_ASR) {
        smart_home_asr_config_load(&asr);
        id = asr.backend_id; host = asr.host; path = asr.path; port = asr.port;
        model = asr.model; option = asr.language; enabled = asr.enabled;
        count = smart_home_asr_preset_count();
    }
#endif
#ifdef CONFIG_SMART_HOME_VOICE_TTS
    smart_home_tts_config_t tts;
    if (service == SMART_HOME_SECRET_TTS) {
        smart_home_tts_config_load(&tts);
        id = tts.backend_id; host = tts.host; path = tts.path; port = tts.port;
        model = tts.model; option = tts.voice; enabled = tts.enabled;
        count = smart_home_tts_preset_count();
    }
#endif
    v->service = service; v->ui = ui; v->keyboard = keyboard;
    label = smart_home_lvgl_label_create(panel,
        service == SMART_HOME_SECRET_ASR ? "语音输入 ASR（保存后生效）" : "自动播报 TTS（保存后生效）",
        SMART_HOME_UI_COLOR_TEXT_PRIMARY, 19);
    lv_obj_set_pos(label, 0, y);
    v->enabled = lv_switch_create(panel); lv_obj_set_pos(v->enabled, 640, y);
    if (enabled) lv_obj_add_state(v->enabled, LV_STATE_CHECKED);
    v->backend = lv_dropdown_create(panel);
    lv_obj_set_pos(v->backend, 0, y + 46); lv_obj_set_size(v->backend, 336, 44);
    for (size_t i = 0; i < count; i++) {
        const char *name = "", *candidate = "";
#ifdef CONFIG_SMART_HOME_VOICE_ASR
        if (service == SMART_HOME_SECRET_ASR) {
            const smart_home_asr_preset_t *p = smart_home_asr_preset_get(i);
            name = p->name; candidate = p->id;
        }
#endif
#ifdef CONFIG_SMART_HOME_VOICE_TTS
        if (service == SMART_HOME_SECRET_TTS) {
            const smart_home_tts_preset_t *p = smart_home_tts_preset_get(i);
            name = p->name; candidate = p->id;
        }
#endif
        if (i) strncat(choices, "\n", sizeof(choices) - strlen(choices) - 1);
        strncat(choices, name, sizeof(choices) - strlen(choices) - 1);
        if (!strcmp(id, candidate)) selected = i;
    }
    lv_dropdown_set_options(v->backend, choices); lv_dropdown_set_selected(v->backend, selected);
    smart_home_lvgl_style_dropdown(v->backend, 16);
    v->model = voice_input(v, panel, 360, y + 46, 344, "模型", model, 63);
    v->host = voice_input(v, panel, 0, y + 118, 520, "服务域名", host, 127);
    v->port = voice_input(v, panel, 544, y + 118, 160, "端口", port, 7);
    v->path = voice_input(v, panel, 0, y + 190, 704, "API 路径", path, 127);
    v->key = voice_input(v, panel, 0, y + 262, 704, "独立 API 密钥（留空保留）", "", 255);
    lv_textarea_set_password_mode(v->key, true);
    if (service == SMART_HOME_SECRET_ASR) {
        label = smart_home_lvgl_label_create(panel, "识别语言", SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
        lv_obj_set_pos(label, 0, y + 310);
        v->option = lv_dropdown_create(panel); lv_obj_set_pos(v->option, 0, y + 334);
        lv_obj_set_size(v->option, 336, 44); lv_dropdown_set_options(v->option, "中文\nEnglish\n自动");
        lv_dropdown_set_selected(v->option, !strcmp(option, "en") ? 1 : !strcmp(option, "auto") ? 2 : 0);
        smart_home_lvgl_style_dropdown(v->option, 16);
    } else v->option = voice_input(v, panel, 0, y + 334, 336, "音色", option, 63);
    v->status = smart_home_lvgl_label_create(panel, smart_home_secrets_error_text(
        smart_home_secrets_api_key_status(service, id)), SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
    lv_obj_set_pos(v->status, 0, y + 386); lv_obj_set_width(v->status, 704);
    lv_label_set_long_mode(v->status, LV_LABEL_LONG_WRAP);
    for (int i = 0; i < 3; i++) {
        const char *titles[] = {"保存配置", "清除本服务密钥", "测试已保存配置"};
        lv_obj_t *button = lv_button_create(panel);
        lv_obj_set_pos(button, i * 240, y + 442); lv_obj_set_size(button, 224, 42);
        lv_obj_set_user_data(button, (void *)(intptr_t)i);
        lv_obj_add_event_cb(button, voice_button_cb, LV_EVENT_CLICKED, v);
        smart_home_lvgl_press_feedback(button);
        label = smart_home_lvgl_label_create(button, titles[i], SMART_HOME_UI_COLOR_TEXT_PRIMARY, 14);
        lv_obj_center(label);
    }
    lv_obj_add_event_cb(v->backend, voice_select_cb, LV_EVENT_VALUE_CHANGED, v);
}

void smart_home_lvgl_build_voice_settings(smart_home_lvgl_t *ui, lv_obj_t *panel,
                                          lv_obj_t *keyboard)
{
    lv_obj_t *title;
    int y = 56;

    /* 两个面板共用一个纵向滚动区：v9 右栏只有 408 高，单个面板约 470，
     * 所以整体滚动而不是压缩行距。 */
    title = smart_home_lvgl_label_create(panel, "语音服务",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 22);
    lv_obj_set_pos(title, 0, 0);

#ifdef CONFIG_SMART_HOME_VOICE_ASR
    build_voice_panel(ui, panel, keyboard, SMART_HOME_SECRET_ASR, y);
    y += 480;
#endif
#ifdef CONFIG_SMART_HOME_VOICE_TTS
    build_voice_panel(ui, panel, keyboard, SMART_HOME_SECRET_TTS, y);
#endif
}

int smart_home_lvgl_voice_settings_busy(smart_home_lvgl_t *ui)
{
    return g_voice_job.active && g_voice_job.owner == ui;
}
