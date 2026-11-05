/**
 * smart_home LVGL UI runtime entry.
 */

#include "smart_home_lvgl_internal.h"
#include "smart_home_lvgl_camera_dialog.h"
#include "../../smart_home_cpu_debug.h"
#include "../../smart_home_memory.h"

#include <nuttx/config.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/boardctl.h>

#ifdef CONFIG_LV_USE_NUTTX_LIBUV
#include <uv.h>
#endif

#undef NEED_BOARDINIT

#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
#define NEED_BOARDINIT 1
#endif

#ifdef CONFIG_ESP32S3_BOARD_TOUCHSCREEN_PATH
#define SMART_HOME_TOUCH_PATH CONFIG_ESP32S3_BOARD_TOUCHSCREEN_PATH
#else
#define SMART_HOME_TOUCH_PATH "/dev/input0"
#endif

#define SMART_HOME_LVGL_QUEUE_POLL_MS 10u

#ifdef CONFIG_SMART_HOME_DEMO_DEBUG_LOG
static void smart_home_lvgl_log_build_stage(const char *stage,
                                            const char *phase)
{
    printf("[smart_home_lvgl] build %s %s\n", stage, phase);
}
#else
#define smart_home_lvgl_log_build_stage(stage, phase) ((void)0)
#endif

#ifdef CONFIG_SMART_HOME_V9_PERF_LOG
/* 给性能行打上当前页面标签，板端一份日志就能按场景分组回填 P01-P12。
 * 弹窗叠加在所在页面上，不单独计数。
 */
static const char *v9_perf_scene(smart_home_lvgl_t *ui)
{
    lv_obj_t *active;

    if (!ui) {
        return "none";
    }
    active = lv_screen_active();
    if (active == ui->screen_home) return "home";
    if (active == ui->screen_chat) return "chat";
    if (active == ui->screen_settings_v9) return "settings";
    if (active == ui->screen_screensaver) return "screensaver";
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (smart_home_lvgl_miloco_bind_is_open(ui)) return "miloco-bind";
#endif
    return "other";
}
#endif

smart_home_lvgl_t *smart_home_lvgl_init(smart_home_agent_app_t *app)
{
    smart_home_lvgl_t *ui;

    ui = (smart_home_lvgl_t *)calloc(1, sizeof(*ui));
    if (!ui) {
        return NULL;
    }

    ui->app = app;
    ui->device_state = app ? &app->device_state : NULL;
    ui->active_tab = SMART_HOME_TAB_HOME;

    if (pthread_mutex_init(&ui->pending_mutex, NULL) != 0) {
        free(ui);
        return NULL;
    }

    smart_home_lvgl_log_build_stage("screensaver", "begin");
    smart_home_lvgl_build_screensaver_screen(ui);
    smart_home_lvgl_log_build_stage("screensaver", "done");

    smart_home_lvgl_log_build_stage("home", "begin");
    smart_home_lvgl_build_home_screen_v9(ui);
    smart_home_lvgl_log_build_stage("home", "done");

    /* V9 camera preview is an on-demand popup. Do not allocate the old
     * security page and its second RGB565 preview buffer at startup. */

    smart_home_lvgl_log_build_stage("chat", "begin");
    smart_home_lvgl_build_chat_screen(ui);
    (void)smart_home_lvgl_chat_restore(ui);
    smart_home_lvgl_log_build_stage("chat", "done");

    smart_home_lvgl_log_build_stage("settings", "begin");
    smart_home_lvgl_build_settings_screen_v9(ui);
    smart_home_lvgl_log_build_stage("settings", "done");

#ifdef CONFIG_SMART_HOME_KWS
    /* 语音会话联动：KWS 常驻监听（kws.json enabled 时）+
     * TTS 播报期间自动暂停推理。 */
    smart_home_lvgl_voice_session_start(ui);
#endif
#ifdef CONFIG_SMART_HOME_REMINDERS
    smart_home_lvgl_reminder_init(ui);
#endif
    return ui;
}

void smart_home_lvgl_set_app(smart_home_lvgl_t *ui,
                             smart_home_agent_app_t *app)
{
    if (!ui) {
        return;
    }

    ui->app = app;
    ui->device_state = app ? &app->device_state : NULL;
}

void smart_home_lvgl_show(smart_home_lvgl_t *ui)
{
    if (!ui || !ui->screen_screensaver) {
        return;
    }

    lv_scr_load(ui->screen_screensaver);
}

void smart_home_lvgl_deinit(smart_home_lvgl_t *ui)
{
    if (!ui) {
        return;
    }

#ifdef CONFIG_SMART_HOME_REMINDERS
    smart_home_lvgl_reminder_deinit(ui);
#endif
    smart_home_lvgl_camera_dialog_close();
    smart_home_lvgl_settings_v9_deinit(ui);
    if (ui->agent_worker_active) {
        pthread_join(ui->agent_worker, NULL);
        ui->agent_worker_active = 0;
    }
    (void)smart_home_agent_app_set_observer(ui->app, NULL, NULL);
    smart_home_lvgl_discard_pending(ui);
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (ui->miloco_timer) {
        lv_timer_delete(ui->miloco_timer);
        ui->miloco_timer = NULL;
    }
    if (ui->bind_timer) {
        lv_timer_delete(ui->bind_timer);
        ui->bind_timer = NULL;
    }
#endif
    if (ui->home_time_timer) {
        lv_timer_delete(ui->home_time_timer);
        ui->home_time_timer = NULL;
    }
    if (ui->v9_poll_timer) {
        lv_timer_delete(ui->v9_poll_timer);
        ui->v9_poll_timer = NULL;
    }
    if (ui->screen_screensaver) {
        lv_obj_del(ui->screen_screensaver);
    }
    if (ui->screen_home) {
        lv_obj_del(ui->screen_home);
    }
    if (ui->screen_chat) {
        lv_obj_del(ui->screen_chat);
    }
    if (ui->screen_settings_v9) {
        lv_obj_del(ui->screen_settings_v9);
    }
    pthread_mutex_destroy(&ui->pending_mutex);
    smart_home_bulk_free(ui->agent_worker_stack_alloc);
    free(ui);
}

#ifdef CONFIG_LV_USE_NUTTX_LIBUV
static void smart_home_lvgl_uv_pending_cb(uv_timer_t *timer)
{
    smart_home_lvgl_process_pending((smart_home_lvgl_t *)timer->data);
}

static void smart_home_lvgl_uv_loop(uv_loop_t *loop,
                                    lv_nuttx_result_t *result,
                                    smart_home_lvgl_t *ui)
{
    lv_nuttx_uv_t uv_info;
    uv_timer_t pending_timer;
    void *data;

    uv_loop_init(loop);
    lv_memzero(&uv_info, sizeof(uv_info));
    uv_info.loop = loop;
    uv_info.disp = result->disp;
    uv_info.indev = result->indev;
#ifdef CONFIG_UINPUT_TOUCH
    uv_info.uindev = result->utouch_indev;
#endif
#ifdef CONFIG_LV_USE_NUTTX_MOUSE
    uv_info.mouse_indev = result->mouse_indev;
#endif

    data = lv_nuttx_uv_init(&uv_info);
    if (uv_timer_init(loop, &pending_timer) == 0) {
        pending_timer.data = ui;
        uv_timer_start(&pending_timer,
                       smart_home_lvgl_uv_pending_cb,
                       0,
                       SMART_HOME_LVGL_QUEUE_POLL_MS);
    }
    uv_run(loop, UV_RUN_DEFAULT);
    lv_nuttx_uv_deinit(&data);
}
#endif

int smart_home_lvgl_run(smart_home_agent_app_t *app,
                        const char *initial_input)
{
    lv_nuttx_dsc_t info;
    lv_nuttx_result_t result;
    smart_home_lvgl_t *ui;

#ifdef CONFIG_LV_USE_NUTTX_LIBUV
    uv_loop_t ui_loop;
    lv_memzero(&ui_loop, sizeof(ui_loop));
#endif

    if (!app) {
        return AGENT_ERROR_INVALID;
    }

    if (lv_is_initialized()) {
        return AGENT_ERROR_BUSY;
    }

    smart_home_cpu_debug_log("lvgl-run-start");

#ifdef NEED_BOARDINIT
    boardctl(BOARDIOC_INIT, 0);
#endif

    lv_init();
#ifdef CONFIG_SMART_HOME_DEMO_DEBUG_LOG
    printf("[lvgl] lv_init done\n");
#endif
    smart_home_lvgl_style_init();
    lv_nuttx_dsc_init(&info);

    info.fb_path = CONFIG_SMART_HOME_DEMO_LVGL_FB_PATH;

#ifdef CONFIG_INPUT_TOUCHSCREEN
    info.input_path = SMART_HOME_TOUCH_PATH;
#endif

    lv_nuttx_init(&info, &result);
    if (!result.disp) {
        smart_home_lvgl_style_deinit();
        lv_deinit();
        return AGENT_ERROR;
    }

#ifdef CONFIG_INPUT_TOUCHSCREEN
    printf("[smart_home_lvgl] disp=%p indev=%p input=%s\n",
           (void *)result.disp,
           (void *)result.indev,
           SMART_HOME_TOUCH_PATH);
#else
    printf("[smart_home_lvgl] disp=%p indev=%p input=disabled\n",
           (void *)result.disp,
           (void *)result.indev);
#endif

    ui = smart_home_lvgl_init(app);
    if (!ui) {
#ifdef CONFIG_SMART_HOME_DEMO_DEBUG_LOG
        printf("[lvgl] ui init FAILED\n");
#endif
        lv_nuttx_deinit(&result);
        smart_home_lvgl_style_deinit();
        lv_deinit();
        return AGENT_ERROR_NOMEM;
    }
#ifdef CONFIG_SMART_HOME_DEMO_DEBUG_LOG
    printf("[lvgl] ui init done\n");
#endif

    smart_home_agent_app_set_observer(app, smart_home_lvgl_event_cb, ui);
    smart_home_lvgl_show(ui);
#ifdef CONFIG_SMART_HOME_DEMO_DEBUG_LOG
    printf("[lvgl] show done, entering run loop\n");
#endif

    if (initial_input && initial_input[0]) {
        ui->active_tab = SMART_HOME_TAB_CHAT;
        lv_scr_load(ui->screen_chat);
        smart_home_lvgl_chat_send_text(ui, initial_input);
    }

#ifdef CONFIG_LV_USE_NUTTX_LIBUV
    smart_home_lvgl_uv_loop(&ui_loop, &result, ui);
#else
    smart_home_cpu_debug_log("lvgl-loop-start");
#ifdef CONFIG_SMART_HOME_V9_PERF_LOG
    uint32_t perf_frames = 0;
    uint32_t perf_sum_ms = 0;
    uint32_t perf_max_ms = 0;
    uint32_t perf_window = lv_tick_get();
#endif
    for (;;) {
        uint32_t idle;
#ifdef CONFIG_SMART_HOME_V9_PERF_LOG
        uint32_t started = lv_tick_get();
#endif

        smart_home_lvgl_process_pending(ui);
        idle = lv_timer_handler();
        smart_home_lvgl_process_pending(ui);
#ifdef CONFIG_SMART_HOME_V9_PERF_LOG
        {
            uint32_t elapsed = lv_tick_get() - started;

            perf_frames++;
            perf_sum_ms += elapsed;
            if (elapsed > perf_max_ms) perf_max_ms = elapsed;
            if (lv_tick_elaps(perf_window) >= 5000u) {
                /* 同一行带出用户堆快照，P01-P12 只需一份串口日志即可回填；
                 * mallinfo() 反映 umm 用户堆（LVGL 与业务对象所在堆）。
                 */
                struct mallinfo mem = mallinfo();

                printf("[v9-perf] frames=%u avg_ms=%u max_ms=%u "
                       "heap_used=%u heap_free=%u heap_largest=%u scene=%s\n",
                       (unsigned)perf_frames,
                       (unsigned)(perf_sum_ms / (perf_frames ? perf_frames : 1)),
                       (unsigned)perf_max_ms,
                       (unsigned)mem.uordblks, (unsigned)mem.fordblks,
                       (unsigned)mem.mxordblk, v9_perf_scene(ui));
                perf_frames = 0;
                perf_sum_ms = 0;
                perf_max_ms = 0;
                perf_window = lv_tick_get();
            }
        }
#endif
        if (idle == 0 || idle > SMART_HOME_LVGL_QUEUE_POLL_MS) {
            idle = SMART_HOME_LVGL_QUEUE_POLL_MS;
        }
        usleep((idle ? idle : 1) * 1000);
    }
#endif
#ifdef CONFIG_SMART_HOME_DEMO_DEBUG_LOG
    printf("[lvgl] run loop exited\n");
#endif

    smart_home_lvgl_deinit(ui);
    lv_nuttx_deinit(&result);
    smart_home_lvgl_style_deinit();
    lv_deinit();
    return AGENT_OK;
}
