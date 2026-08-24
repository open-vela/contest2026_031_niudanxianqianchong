/**
 * smart_home LVGL UI runtime entry.
 */

#include "smart_home_lvgl_internal.h"
#include "../../smart_home_cpu_debug.h"
#include "../../smart_home_memory.h"

#include <nuttx/config.h>

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

smart_home_lvgl_t *smart_home_lvgl_init(smart_home_agent_app_t *app)
{
    smart_home_lvgl_t *ui;

    ui = (smart_home_lvgl_t *)calloc(1, sizeof(*ui));
    if (!ui) {
        return NULL;
    }

    ui->app = app;
    ui->device_state = app ? &app->device_state : NULL;
    ui->active_tab = SMART_HOME_TAB_PANEL;

    if (pthread_mutex_init(&ui->pending_mutex, NULL) != 0) {
        free(ui);
        return NULL;
    }

    smart_home_lvgl_build_panel_screen(ui);
    smart_home_lvgl_build_chat_screen(ui);
    smart_home_lvgl_build_settings_screen(ui);
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
    if (!ui || !ui->screen_panel) {
        return;
    }

    lv_scr_load(ui->screen_panel);
}

void smart_home_lvgl_deinit(smart_home_lvgl_t *ui)
{
    if (!ui) {
        return;
    }

    if (ui->agent_worker_active) {
        pthread_join(ui->agent_worker, NULL);
        ui->agent_worker_active = 0;
    }
    smart_home_lvgl_discard_pending(ui);
    smart_home_lvgl_settings_deinit();
    if (ui->remote_node_timer) {
        lv_timer_delete(ui->remote_node_timer);
        ui->remote_node_timer = NULL;
    }
    if (ui->screen_panel) {
        lv_obj_del(ui->screen_panel);
    }
    if (ui->screen_chat) {
        lv_obj_del(ui->screen_chat);
    }
    if (ui->screen_settings) {
        lv_obj_del(ui->screen_settings);
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

    agent_set_event_callback(app->agent, smart_home_lvgl_event_cb, ui);
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
    for (;;) {
        uint32_t idle;

        smart_home_lvgl_process_pending(ui);
        idle = lv_timer_handler();
        smart_home_lvgl_process_pending(ui);
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
