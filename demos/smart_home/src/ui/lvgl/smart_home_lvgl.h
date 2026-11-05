/**
 * smart_home LVGL UI — main header.
 *
 * Provides types and public API for the multi-screen LVGL smart home UI.
 * LVGL is owned exclusively by the UI thread. Agent workers enqueue plain C
 * payloads; the UI loop drains them before updating LVGL widgets.
 *
 * Product screens: Home / Chat / Settings. Device, camera and service
 * controls are integrated into V9 pages and dialogs.
 *
 * Usage:
 *   smart_home_lvgl_t *ui = smart_home_lvgl_init(&app);
 *   smart_home_agent_app_set_observer(app, smart_home_lvgl_event_cb, ui);
 *   smart_home_lvgl_show(ui);   // load screensaver, then enter Home by touch
 *   smart_home_lvgl_deinit(ui); // cleanup
 */

#pragma once

#include <agent.h>
#include <lvgl/lvgl.h>
#include <pthread.h>
#include <stdint.h>
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
#include "../../addons/smart_home_node_gateway.h"
#endif

#include "../../agent/smart_home_agent.h"
#include "../../camera/smart_home_camera_service.h"
#include "../../device/smart_home_device.h"
#include "smart_home_lvgl_style.h"
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
#include "../../miloco/smart_home_miloco.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── UI state type ──────────────────────────────────────────── */

#define SMART_HOME_LVGL_TRACE_TOOL_MAX 8

typedef struct {
    lv_obj_t *row;
    lv_obj_t *status_label;
    char id[64];
    char name[64];
    int state; /* -2 unconfirmed, -1 running, 0 failed, 1 result returned */
} smart_home_lvgl_trace_tool_t;

typedef struct {
#ifdef CONFIG_SMART_HOME_REMINDERS
    lv_obj_t *reminder_popup;
    lv_obj_t *reminder_status;
    lv_obj_t *reminder_button;
    lv_timer_t *reminder_timer;
    int reminder_id;
    int reminder_ack_pending;
    uint32_t reminder_ack_tick;
#endif
    /* Screens */
    lv_obj_t *screen_screensaver;
    lv_obj_t *screen_home;
    lv_obj_t *screen_chat;
    lv_obj_t *screen_settings_v9;
    int active_tab;

    /* Chat: message list + status hint */
    lv_obj_t *chat_list;
    lv_obj_t *chat_status;
    lv_obj_t *chat_input_bar;
    lv_obj_t *chat_input;
    lv_obj_t *chat_send_btn;
    lv_obj_t *chat_keyboard;
    lv_obj_t *chat_trace_row;
    lv_obj_t *chat_trace_body;
    lv_obj_t *chat_trace_title;
    lv_obj_t *chat_trace_status;
    smart_home_lvgl_trace_tool_t
              chat_trace_tools[SMART_HOME_LVGL_TRACE_TOOL_MAX];
    int       chat_trace_collapsed;
    int       chat_trace_active;
    int       chat_trace_tool_count;
    int       chat_msg_count;
#ifdef CONFIG_SMART_HOME_VOICE_ASR
    /* PTT 语音输入（点击开始/点击结束，识别后自动发送）。 */
    lv_obj_t *chat_mic_btn;
    lv_obj_t *chat_mic_label;
    int       asr_active;          /* 仅 LVGL 线程读写 */
    volatile int asr_abort;        /* LVGL 线程置 1 请求结束录音 */
    int       asr_worker_active;   /* worker 存活标志（LVGL 线程 join） */
    pthread_t asr_worker;
    void     *asr_worker_stack_alloc;
    void     *asr_worker_stack;
#endif

    lv_obj_t *topbar_wifi_icons[16];
    /* 顶栏摄像头状态图标（安防页开关联动：off 显 camera-off，
     * 开启显 camera），与 wifi 图标同款跨屏刷新模式。 */
    lv_obj_t *topbar_camera_icons[16];
    lv_obj_t *topbar_clock_labels[16];  /* 每屏一个顶栏时钟，定时器统一刷新 */
    int topbar_clock_count;
    int topbar_camera_icon_count;
    int topbar_camera_on;
    lv_obj_t *home_weather_badge;
    lv_obj_t *home_weather_temp_label;
    lv_obj_t *home_weather_condition_label;
    lv_obj_t *home_date_label;
    lv_obj_t *home_time_label;
    lv_timer_t *home_time_timer;
    lv_timer_t *v9_poll_timer;
    lv_timer_t *v9_network_timer;
    lv_obj_t *screensaver_time_label;
    lv_obj_t *screensaver_date_label;
    lv_obj_t *screensaver_temp_label;
    lv_obj_t *screensaver_condition_label;
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    lv_timer_t *miloco_timer;
    uint32_t miloco_polled_revision;
    lv_obj_t *screen_miloco_bind;   /* 绑定弹窗的遮罩根对象，NULL = 未打开 */
    lv_obj_t *bind_status_label;
    lv_obj_t *bind_head_label;
    lv_obj_t *bind_head_dot;
    lv_obj_t *bind_check_button;
    lv_timer_t *bind_timer;
#endif
    int topbar_wifi_icon_count;
    /* Device state (shared, NOT owned) */
    smart_home_state_t *device_state;

    /* Shared application object (agent/tools/device, NOT owned) */
    smart_home_agent_app_t *app;

    /* Pending worker-to-UI messages.  The queue itself contains no LVGL
     * objects and is protected by pending_mutex. */
    pthread_mutex_t pending_mutex;
    void *pending_head;
    void *pending_tail;
    int pending_tool_count;
    int request_inflight;

    /* One reusable PSRAM stack backs the joinable Chat Agent worker. */
    pthread_t agent_worker;
    void *agent_worker_stack_alloc;
    void *agent_worker_stack;
    int agent_worker_active;
} smart_home_lvgl_t;

/* ── Public API ─────────────────────────────────────────────── */

/** Allocate and initialise the UI state. app is shared and not owned. */
smart_home_lvgl_t *smart_home_lvgl_init(smart_home_agent_app_t *app);

/** Bind a new app object after init, if needed. */
void smart_home_lvgl_set_app(smart_home_lvgl_t *ui, smart_home_agent_app_t *app);

/** Load the lightweight screensaver (typically called once after init). */
void smart_home_lvgl_show(smart_home_lvgl_t *ui);

/** Tear down all screens and free state. */
void smart_home_lvgl_deinit(smart_home_lvgl_t *ui);

/** Refresh device cards from current device_state. UI owner thread only. */
void smart_home_lvgl_refresh_cards(smart_home_lvgl_t *ui);

/** cAGENT event callback — queues an update for the LVGL thread. */
void smart_home_lvgl_event_cb(void *user_data, const agent_event_t *event);

/** Apply queued worker updates.  Must be called by the LVGL owner thread. */
void smart_home_lvgl_process_pending(smart_home_lvgl_t *ui);

/** Discard queued worker updates during UI teardown. */
void smart_home_lvgl_discard_pending(smart_home_lvgl_t *ui);

/** Run LVGL backend: init LVGL, show screens, enter lv_timer_handler loop. */
int smart_home_lvgl_run(smart_home_agent_app_t *app, const char *initial_input);

#ifdef __cplusplus
}
#endif
