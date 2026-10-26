/**
 * smart_home LVGL agent bridge.
 */

#include "smart_home_lvgl_internal.h"

#include "../../config/smart_home_config.h"
#include "../../smart_home_cpu_debug.h"
#include "../../smart_home_memory.h"
#include "../smart_home_ui_event.h"
#ifdef CONFIG_SMART_HOME_VOICE_ASR
#include "../../voice/smart_home_asr.h"
#include "../../voice/smart_home_voice_capture.h"
#endif
#ifdef CONFIG_SMART_HOME_VOICE_TTS
#include "../../voice/smart_home_voice_play.h"
#endif

#ifndef CONFIG_SMART_HOME_DEMO_OFFLINE_UI
#include <arpa/inet.h>
#endif
#include <nuttx/irq.h>
#include <nuttx/sched.h>
#include <sched.h>
#include <pthread.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef CONFIG_SMART_HOME_DEMO_OFFLINE_UI
#include "netutils/netlib.h"
#endif

/* Temporary BOX-3 memory-pressure A/B test: match the successful console
 * smart_home task stack instead of reserving 64 KiB for each chat worker. */
#define SMART_HOME_AGENT_WORKER_STACK_SIZE 32768u

typedef struct {
    smart_home_lvgl_t *ui;
    int ret;
    char output[SMART_HOME_OUTPUT_SIZE];
} agent_done_t;

typedef struct {
    smart_home_lvgl_t *ui;
    agent_done_t *done;
    struct ui_pending_item *completion;
    char input[SMART_HOME_INPUT_SIZE];
} agent_job_t;

typedef smart_home_ui_event_t ui_event_payload_t;

typedef enum {
    UI_PENDING_EVENT = 0,
    UI_PENDING_DONE,
#ifdef CONFIG_SMART_HOME_VOICE_ASR
    UI_PENDING_ASR_TEXT,
#endif
#ifdef CONFIG_SMART_HOME_KWS
    UI_PENDING_KWS_WAKE,
#endif
} ui_pending_type_t;

#ifdef CONFIG_SMART_HOME_VOICE_ASR
typedef struct {
    char text[SMART_HOME_INPUT_SIZE];
} ui_asr_payload_t;
#endif

#ifdef CONFIG_SMART_HOME_KWS
typedef struct {
    float wake_score;
} ui_kws_payload_t;
#endif

typedef struct ui_pending_item {
    struct ui_pending_item *next;
    ui_pending_type_t type;
    union {
        ui_event_payload_t event;
        agent_done_t *done;
#ifdef CONFIG_SMART_HOME_VOICE_ASR
        ui_asr_payload_t asr;
#endif
#ifdef CONFIG_SMART_HOME_KWS
        ui_kws_payload_t kws;
#endif
    } data;
} ui_pending_item_t;

#ifdef CONFIG_SMART_HOME_DEMO_DEBUG_LOG
static void agent_worker_log_context(const char *stage,
                                     const smart_home_agent_app_t *app)
{
    struct sched_param sched_param;
    struct stackinfo_s stack_info;
    smart_home_network_status_t network;
#ifndef CONFIG_SMART_HOME_DEMO_OFFLINE_UI
    struct in_addr address;
    char address_text[INET_ADDRSTRLEN] = "none";
#else
    char address_text[] = "offline";
#endif
    int policy = -1;
    int priority = -1;
    int stack_ret;
    int probe_ret = SMART_HOME_NETWORK_ERR_NO_IP;

    memset(&sched_param, 0, sizeof(sched_param));
    if (pthread_getschedparam(pthread_self(), &policy, &sched_param) == 0) {
        priority = sched_param.sched_priority;
    }

    memset(&stack_info, 0, sizeof(stack_info));
    stack_ret = nxsched_get_stackinfo(0, &stack_info);
    printf("[agent_worker] %s tid=%d policy=%d priority=%d "
           "stack_capacity=%zu stack_query=%d\n",
           stage,
           (int)gettid(),
           policy,
           priority,
           stack_ret == 0 ? stack_info.adj_stack_size :
                            (size_t)SMART_HOME_AGENT_WORKER_STACK_SIZE,
           stack_ret);

    if (!app) {
        printf("[agent_worker] %s network app=none\n", stage);
        return;
    }

    network = app->system_status.network_status;
#ifndef CONFIG_SMART_HOME_DEMO_OFFLINE_UI
    memset(&address, 0, sizeof(address));
    if (network.ifname) {
        netlib_get_ipv4addr(network.ifname, &address);
        if (address.s_addr != 0) {
            inet_ntop(AF_INET, &address, address_text, sizeof(address_text));
        }

        probe_ret = smart_home_network_probe(&network);
    }
#else
    probe_ret = smart_home_network_probe(&network);
#endif

    printf("[agent_worker] %s network if=%s ip=%s probe=%d "
           "init=%d ip_status=%d dns_status=%d online=%d\n",
           stage,
           network.ifname ? network.ifname : "none",
           address_text,
           probe_ret,
           network.init_status,
           network.ip_status,
           network.dns_status,
           network.online ? 1 : 0);
}
#endif

static int agent_ui_enqueue(smart_home_lvgl_t *ui, ui_pending_item_t *item)
{
    int ret;

    if (!ui || !item) {
        return -1;
    }

    ret = pthread_mutex_lock(&ui->pending_mutex);
    if (ret != 0) {
        return -ret;
    }

    item->next = NULL;
    if (ui->pending_tail) {
        ((ui_pending_item_t *)ui->pending_tail)->next = item;
    } else {
        ui->pending_head = item;
    }
    ui->pending_tail = item;
    pthread_mutex_unlock(&ui->pending_mutex);
    return 0;
}

static void agent_done_apply(agent_done_t *done)
{
    smart_home_lvgl_t *ui = done ? done->ui : NULL;

    if (!done) {
        return;
    }

    if (ui) {
        if (ui->agent_worker_active) {
            pthread_join(ui->agent_worker, NULL);
            ui->agent_worker_active = 0;
        }
        ui->request_inflight = 0;
#ifdef CONFIG_SMART_HOME_DEMO_DEBUG_LOG
        printf("[agent] reply(%d): %.200s\n",
               done->ret,
               done->output[0] ? done->output : "(empty)");
#endif
        if (done->ret == AGENT_OK || done->ret == AGENT_ERROR_TRUNCATED) {
            smart_home_lvgl_finish_thinking(ui);
            smart_home_lvgl_append_msg_bubble(ui, done->output, 0);
            smart_home_lvgl_refresh_cards(ui);
            if (done->ret == AGENT_ERROR_TRUNCATED)
                smart_home_lvgl_append_error_bubble(ui, "回复超出显示容量，以上内容已截断。");
#ifdef CONFIG_SMART_HOME_VOICE_TTS
            /* 语音播报在独立 worker 中合成与播放；开关关闭或服务
             * 未就绪时静默跳过，不影响气泡展示路径。 */
            if (done->ret == AGENT_OK && voice_play_announce_enabled()) {
                if (voice_play_text(done->output) != 0) {
                    printf("[voice] announce enqueue failed\n");
                }
            }
#endif
        } else {
            char message[160];
            snprintf(message,
                     sizeof(message),
                     "Agent call failed (%d): %.96s",
                     done->ret,
                     done->output[0] ? done->output : "no detail");
            smart_home_lvgl_finish_thinking(ui);
            smart_home_lvgl_append_error_bubble(ui, message);
        }
        /* Update status hint */
        if (ui->chat_status) {
            lv_label_set_text(ui->chat_status,
                              "Done.  (NSH: smart_home \"...\")");
            lv_obj_clear_flag(ui->chat_status, LV_OBJ_FLAG_HIDDEN);
        }
        /* Force display refresh so the response appears immediately
         * without waiting for a user touch event. */
        lv_refr_now(NULL);
    }

    free(done);
}

static void *agent_worker_main(void *arg)
{
    char stack_marker;
    agent_job_t *job = (agent_job_t *)arg;
    agent_done_t *done;
    ui_pending_item_t *item;

    if (!job) {
        return NULL;
    }

    smart_home_cpu_debug_log("lvgl-agent-worker-start");
    smart_home_memory_log_region("lvgl-agent-worker-stack", &stack_marker);

#ifdef CONFIG_SMART_HOME_DEMO_DEBUG_LOG
    agent_worker_log_context("start", job->ui ? job->ui->app : NULL);
#endif

    done = job->done;
    smart_home_cpu_debug_log("lvgl-agent-before-agent");
    done->ret = smart_home_agent_run(job->ui->app,
                                     job->input,
                                     done->output,
                                     sizeof(done->output));
#ifdef CONFIG_SMART_HOME_DEMO_DEBUG_LOG
    agent_worker_log_context("done", job->ui ? job->ui->app : NULL);
#endif
    item = job->completion;
    item->type = UI_PENDING_DONE;
    item->data.done = done;
    if (agent_ui_enqueue(job->ui, item) == 0) {
        free(job);
        return NULL;
    }
    free(item);

    free(done);
    free(job);
    return NULL;
}

int smart_home_lvgl_submit_agent_job(smart_home_lvgl_t *ui, const char *text)
{
    agent_job_t *job;
    agent_done_t *done;
    int ret;

    if (!ui || !ui->app || !text || !text[0]) {
        return AGENT_ERROR_INVALID;
    }

    if (ui->request_inflight) {
        smart_home_lvgl_append_error_bubble(ui, "Previous request is still running.");
        return AGENT_ERROR_BUSY;
    }

    job = (agent_job_t *)calloc(1, sizeof(*job));
    if (!job) {
        smart_home_lvgl_append_error_bubble(ui, "Not enough memory to submit request.");
        return AGENT_ERROR_NOMEM;
    }

    done = (agent_done_t *)calloc(1, sizeof(*done));
    if (!done) {
        free(job);
        smart_home_lvgl_append_error_bubble(ui, "Not enough memory to start Agent worker.");
        return AGENT_ERROR_NOMEM;
    }

    job->completion = calloc(1, sizeof(*job->completion));
    if (!job->completion) {
        free(done);
        free(job);
        return AGENT_ERROR_NOMEM;
    }
    job->ui = ui;
    job->done = done;
    strncpy(job->input, text, sizeof(job->input) - 1);
    done->ui = ui;
    ui->request_inflight = 1;

    {
        pthread_attr_t attr;
        int attr_ready = 0;

        if (!ui->agent_worker_stack_alloc) {
            ui->agent_worker_stack_alloc = smart_home_bulk_alloc(
                SMART_HOME_AGENT_WORKER_STACK_SIZE + STACK_ALIGNMENT - 1u);
            if (!ui->agent_worker_stack_alloc) {
                ui->request_inflight = 0;
                free(job->completion);
                free(done);
                free(job);
                smart_home_lvgl_append_error_bubble(
                    ui, "Not enough PSRAM to start Agent worker.");
                return AGENT_ERROR_NOMEM;
            }

            ui->agent_worker_stack = (void *)STACK_ALIGN_UP(
                (uintptr_t)ui->agent_worker_stack_alloc);
            smart_home_memory_log_region("lvgl-agent-worker-stack-base",
                              ui->agent_worker_stack);
            smart_home_bulk_diag("lvgl-agent-worker-stack-reserved");
        }

        ret = pthread_attr_init(&attr);
        if (ret == 0) {
            attr_ready = 1;
            ret = pthread_attr_setstack(&attr, ui->agent_worker_stack,
                                        SMART_HOME_AGENT_WORKER_STACK_SIZE);
        }
        if (ret == 0) {
            ret = pthread_create(&ui->agent_worker, &attr,
                                 agent_worker_main, job);
        }
        if (attr_ready) {
            pthread_attr_destroy(&attr);
        }
    }
    if (ret != 0) {
        ui->request_inflight = 0;
        free(job->completion);
        free(done);
        free(job);
        smart_home_lvgl_append_error_bubble(ui, "Failed to create Agent worker.");
        return AGENT_ERROR;
    }

    ui->agent_worker_active = 1;
    return AGENT_OK;
}

#ifdef CONFIG_SMART_HOME_VOICE_ASR
/* ASR worker：录音 + MiMo 云端识别，结果经 pending 队列回投 LVGL 线程。
 * 本函数绝不调用任何 LVGL API。 */
static void *asr_worker_main(void *arg)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)arg;
    ui_pending_item_t *item;
    uint8_t *pcm = NULL;
    size_t bytes = 0;
    char text[SMART_HOME_INPUT_SIZE];
    int ret;

    text[0] = '\0';

    ret = voice_capture_record(CONFIG_SMART_HOME_ASR_AUDIO_MAX_SECONDS,
                               &ui->asr_abort, &pcm, &bytes);
    if (ret == 0 && bytes >= 3200)
      {
        /* 至少 0.1 s（3200 字节）才值得上传识别。 */
        smart_home_asr_recognize(pcm, bytes, text, sizeof(text));
      }

    if (pcm != NULL)
      {
        smart_home_bulk_free(pcm);
      }

    item = (ui_pending_item_t *)calloc(1, sizeof(*item));
    if (item != NULL)
      {
        item->type = UI_PENDING_ASR_TEXT;
        strncpy(item->data.asr.text, text, sizeof(item->data.asr.text) - 1);
        item->data.asr.text[sizeof(item->data.asr.text) - 1] = '\0';
        if (agent_ui_enqueue(ui, item) != 0)
          {
            free(item);
          }
      }

    return NULL;
}

int smart_home_lvgl_submit_asr_job(smart_home_lvgl_t *ui)
{
    pthread_attr_t attr;
    int attr_ready = 0;
    int ret;

    if (!ui)
      {
        return AGENT_ERROR_INVALID;
      }

    if (ui->asr_worker_active)
      {
        pthread_join(ui->asr_worker, NULL);
        ui->asr_worker_active = 0;
      }

    if (!ui->asr_worker_stack_alloc)
      {
        ui->asr_worker_stack_alloc = smart_home_bulk_alloc(
            CONFIG_SMART_HOME_VOICE_ASR_WORKER_STACKSIZE +
            STACK_ALIGNMENT - 1u);
        if (!ui->asr_worker_stack_alloc)
          {
            return AGENT_ERROR_NOMEM;
          }

        ui->asr_worker_stack = (void *)STACK_ALIGN_UP(
            (uintptr_t)ui->asr_worker_stack_alloc);
      }

    ret = pthread_attr_init(&attr);
    if (ret == 0)
      {
        attr_ready = 1;
        ret = pthread_attr_setstack(&attr, ui->asr_worker_stack,
                                    CONFIG_SMART_HOME_VOICE_ASR_WORKER_STACKSIZE);
      }
    if (ret == 0)
      {
        ret = pthread_create(&ui->asr_worker, &attr, asr_worker_main, ui);
      }
    if (attr_ready)
      {
        pthread_attr_destroy(&attr);
      }

    if (ret != 0)
      {
        return AGENT_ERROR;
      }

    ui->asr_worker_active = 1;
    return AGENT_OK;
}
#endif

#ifdef CONFIG_SMART_HOME_KWS
/* KWS worker 线程调用：投递唤醒事件，不做任何 UI 操作。 */
int smart_home_lvgl_post_kws_wake(smart_home_lvgl_t *ui, float score)
{
    ui_pending_item_t *item;

    if (!ui) {
        return AGENT_ERROR_INVALID;
    }

    item = (ui_pending_item_t *)calloc(1, sizeof(*item));
    if (item == NULL) {
        return AGENT_ERROR_NOMEM;
    }

    item->type = UI_PENDING_KWS_WAKE;
    item->data.kws.wake_score = score;
    if (agent_ui_enqueue(ui, item) != 0) {
        free(item);
        return AGENT_ERROR;
    }

    return AGENT_OK;
}
#endif

static void ui_event_apply(smart_home_lvgl_t *ui,
                           const ui_event_payload_t *payload)
{
    if (!ui || !payload) {
        return;
    }

    switch (payload->type) {
    case AGENT_EVENT_MODEL_BEGIN:
        smart_home_lvgl_show_thinking(ui);
        break;
    case AGENT_EVENT_TOOL_BEGIN:
        smart_home_lvgl_trace_tool(ui,
                                   payload->tool_name,
                                   payload->tool_call_id,
                                   -1);
        break;
    case AGENT_EVENT_TOOL_END:
        if (strcmp(payload->tool_name, "set_light") == 0 ||
            strcmp(payload->tool_name, "set_ac") == 0 ||
            strcmp(payload->tool_name, "run_scene") == 0) {
            smart_home_lvgl_refresh_cards(ui);
        }
        smart_home_lvgl_trace_tool(ui,
                                   payload->tool_name,
                                   payload->tool_call_id,
                                   payload->status == AGENT_OK ? 1 : 0);
        break;
    case AGENT_EVENT_TURN_END:
        smart_home_lvgl_finish_thinking(ui);
        break;
    default:
        break;
    }

}

void smart_home_lvgl_event_cb(void *user_data, const agent_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)user_data;
    ui_pending_item_t *item;

    if (!ui || !event) {
        return;
    }

    item = (ui_pending_item_t *)calloc(1, sizeof(*item));
    if (!item) {
        return;
    }

    item->type = UI_PENDING_EVENT;
    smart_home_ui_event_copy(&item->data.event, event);

    if (agent_ui_enqueue(ui, item) != 0) {
        free(item);
    }
}

void smart_home_lvgl_process_pending(smart_home_lvgl_t *ui)
{
    ui_pending_item_t *item;

    if (!ui) {
        return;
    }

    for (;;) {
        if (pthread_mutex_lock(&ui->pending_mutex) != 0) {
            return;
        }

        item = (ui_pending_item_t *)ui->pending_head;
        if (item) {
            ui->pending_head = item->next;
            if (!ui->pending_head) {
                ui->pending_tail = NULL;
            }
        }
        pthread_mutex_unlock(&ui->pending_mutex);

        if (!item) {
            return;
        }

        if (item->type == UI_PENDING_DONE) {
            agent_done_apply(item->data.done);
        }
#ifdef CONFIG_SMART_HOME_VOICE_ASR
        else if (item->type == UI_PENDING_ASR_TEXT) {
            if (ui->asr_worker_active) {
                pthread_join(ui->asr_worker, NULL);
                ui->asr_worker_active = 0;
            }

            if (item->data.asr.text[0]) {
                smart_home_lvgl_chat_send_text(ui, item->data.asr.text);
            } else {
                smart_home_lvgl_append_error_bubble(
                    ui, "语音识别失败：录音太短、密钥未配置或网络错误。");
            }
            smart_home_lvgl_chat_asr_finish(ui);
        }
#endif
#ifdef CONFIG_SMART_HOME_KWS
        else if (item->type == UI_PENDING_KWS_WAKE) {
            smart_home_voice_session_on_wake(ui, item->data.kws.wake_score);
        }
#endif
        else {
            ui_event_apply(ui, &item->data.event);
        }
        free(item);
    }
}

void smart_home_lvgl_discard_pending(smart_home_lvgl_t *ui)
{
    ui_pending_item_t *item;
    ui_pending_item_t *next;

    if (!ui || pthread_mutex_lock(&ui->pending_mutex) != 0) {
        return;
    }

    item = (ui_pending_item_t *)ui->pending_head;
    ui->pending_head = NULL;
    ui->pending_tail = NULL;
    pthread_mutex_unlock(&ui->pending_mutex);

    while (item) {
        next = item->next;
        if (item->type == UI_PENDING_DONE) {
            free(item->data.done);
        }
        free(item);
        item = next;
    }
}
