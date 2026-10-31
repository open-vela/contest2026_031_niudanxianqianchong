/** V9 Settings shell: six internal categories, summary panels and routes. */

#include "smart_home_lvgl_internal.h"
#include "images/smart_home_icons.h"
#include "../../config/smart_home_backends.h"
#include "../../net/smart_home_network.h"
#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
#include "../../addons/smart_home_mcp_bridge.h"
#endif
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
#include "../../addons/smart_home_node_gateway.h"
#endif

#include <pthread.h>
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
#include "../../miloco/smart_home_miloco.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define V9_SETTINGS_CATEGORIES 6

static const char *const g_category_names[V9_SETTINGS_CATEGORIES] = {
    "显示与声音", "网络连接", "模型服务", "工具权限", "设备与服务", "系统健康"
};

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *panel[V9_SETTINGS_CATEGORIES];
    lv_obj_t *button[V9_SETTINGS_CATEGORIES];
    lv_obj_t *services_modal;
    lv_obj_t *config_modal;
#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
    lv_obj_t *mcp_server;
    lv_obj_t *mcp_host;
    lv_obj_t *mcp_port;
    lv_obj_t *mcp_path;
    lv_obj_t *mcp_auth;
    lv_obj_t *mcp_protocol;
    lv_obj_t *mcp_status;
#endif
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
    lv_obj_t *node_port;
    lv_obj_t *node_status;
#endif
    lv_obj_t *model_dd;
    lv_obj_t *model_name;
    lv_obj_t *model_key;
    lv_obj_t *model_timeout;
    lv_obj_t *model_tokens;
    lv_obj_t *model_status;
    lv_obj_t *model_apply;
    lv_obj_t *net_ssid;
    lv_obj_t *net_password;
    lv_obj_t *net_status;
    lv_obj_t *keyboard;
    int model_index;
    char model_name_draft[8][64];
    char model_key_draft[8][128];
    char model_timeout_draft[8][16];
    char model_tokens_draft[8][16];
    char service_status[3][32];
    int category;
} v9_settings_t;

static v9_settings_t g_settings;

#if defined(CONFIG_SMART_HOME_MCP_BRIDGE) || defined(CONFIG_SMART_HOME_NODE_GATEWAY)
static void mcp_config_open(smart_home_lvgl_t *ui);
static void node_config_open(smart_home_lvgl_t *ui);
#endif

static void panel_show(int category)
{
    if (category < 0 || category >= V9_SETTINGS_CATEGORIES) {
        return;
    }
    g_settings.category = category;
    if (g_settings.keyboard) {
        lv_obj_add_flag(g_settings.keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(g_settings.keyboard, NULL);
    }
    for (int i = 0; i < V9_SETTINGS_CATEGORIES; i++) {
        lv_obj_t *btn = g_settings.button[i];
        lv_obj_t *label;
        if (btn) {
            label = lv_obj_get_child(btn, 0);
            if (i == category) {
                lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(btn, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
                if (label) lv_obj_set_style_text_color(label,
                    SMART_HOME_UI_COLOR_TEXT_PRIMARY, 0);
            } else {
                lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
                if (label) lv_obj_set_style_text_color(label,
                    SMART_HOME_UI_COLOR_TEXT_SECONDARY, 0);
            }
        }
        if (g_settings.panel[i]) {
            if (i == category) {
                lv_obj_clear_flag(g_settings.panel[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(g_settings.panel[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void category_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);

    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        panel_show(index);
    }
}

static lv_obj_t *make_panel(lv_obj_t *right)
{
    lv_obj_t *panel = lv_obj_create(right);

    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
    return panel;
}

static void panel_title(lv_obj_t *panel, const char *text)
{
    lv_obj_t *label = smart_home_lvgl_label_create(
        panel, text, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 22);
    lv_obj_set_pos(label, 0, 0);
}

static void panel_row(lv_obj_t *panel, int y, const char *name,
                      const char *value)
{
    lv_obj_t *label = smart_home_lvgl_label_create(
        panel, name, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 15);
    lv_obj_t *value_label;
    lv_obj_t *line;

    lv_obj_set_pos(label, 0, y);
    value_label = smart_home_lvgl_label_create(
        panel, value, SMART_HOME_UI_COLOR_TEXT_SECONDARY, 15);
    lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, 0, y);

    line = lv_obj_create(panel);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, lv_pct(100), 1);
    lv_obj_set_pos(line, 0, y + 36);
    lv_obj_set_style_bg_color(line, SMART_HOME_UI_COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
}

static void route_cb(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_user_data(event);

    if (lv_event_get_code(event) == LV_EVENT_CLICKED && target) {
        lv_scr_load(target);
    }
}

static void panel_button(lv_obj_t *panel, const char *text, lv_obj_t *target)
{
    lv_obj_t *button = lv_button_create(panel);
    lv_obj_t *label;

    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 180, 40);
    lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    smart_home_lvgl_press_feedback(button);
    lv_obj_add_event_cb(button, route_cb, LV_EVENT_CLICKED, target);

    label = smart_home_lvgl_label_create(button, text,
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 14);
    lv_obj_center(label);
}

static const char *state_text(int ready)
{
    return ready ? "已就绪" : "未就绪";
}

static void build_display_panel(lv_obj_t *panel, lv_obj_t *settings_target)
{
    panel_title(panel, "显示与声音");
    panel_row(panel, 56, "屏幕亮度", "由板级接口提供");
    panel_row(panel, 106, "播报音量", "由音频服务提供");
    panel_row(panel, 156, "自动进入屏保", "5 分钟");
    panel_row(panel, 206, "设备状态动效", "开启");
    panel_button(panel, "打开完整设置", settings_target);
}

typedef struct {
    smart_home_lvgl_t *ui;
    char ssid[33];
    char password[65];
} v9_net_job_t;

static struct {
    pthread_t thread;
    smart_home_lvgl_t *owner;
    int active;
    int ready;
    int result;
    smart_home_network_status_t status;
} g_net;

typedef struct {
    smart_home_lvgl_t *ui;
    smart_home_model_config_t config;
} v9_model_job_t;

static struct {
    pthread_t thread;
    smart_home_lvgl_t *owner;
    int active;
    int ready;
    int result;
} g_model;

static void model_set_busy(int busy)
{
    lv_obj_t *controls[] = {
        g_settings.model_dd, g_settings.model_name, g_settings.model_key,
        g_settings.model_timeout, g_settings.model_tokens, g_settings.model_apply
    };

    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        if (!controls[i]) continue;
        if (busy) lv_obj_add_state(controls[i], LV_STATE_DISABLED);
        else lv_obj_remove_state(controls[i], LV_STATE_DISABLED);
    }
}

static void *v9_model_worker(void *argument)
{
    v9_model_job_t *job = argument;
    smart_home_lvgl_t *ui = job->ui;
    int ret = smart_home_agent_app_apply_model_config(ui->app, &job->config);

    memset(&job->config, 0, sizeof(job->config));
    free(job);
    pthread_mutex_lock(&ui->pending_mutex);
    g_model.result = ret;
    g_model.ready = 1;
    pthread_mutex_unlock(&ui->pending_mutex);
    return NULL;
}

/* Called on the LVGL thread before any screen, app or mutex is released. */
void smart_home_lvgl_settings_v9_deinit(smart_home_lvgl_t *ui)
{
    if (!ui) return;
    if (ui->v9_network_timer) {
        lv_timer_delete(ui->v9_network_timer);
        ui->v9_network_timer = NULL;
    }
    if (g_net.owner == ui) {
        if (g_net.active) pthread_join(g_net.thread, NULL);
        memset(&g_net, 0, sizeof(g_net));
    }
    if (g_model.owner == ui) {
        if (g_model.active) pthread_join(g_model.thread, NULL);
        memset(&g_model, 0, sizeof(g_model));
    }
    memset(&g_settings, 0, sizeof(g_settings));
}

static void *v9_net_worker(void *argument)
{
    v9_net_job_t *job = argument;
    smart_home_network_status_t status = {0};
    int ret;

    if (!job || !job->ui || !job->ui->app) {
        free(job);
        return NULL;
    }
    ret = smart_home_network_connect_credentials(&status, job->ssid,
                                                 job->password);
    pthread_mutex_lock(&job->ui->pending_mutex);
    g_net.status = status;
    g_net.result = ret;
    g_net.ready = 1;
    pthread_mutex_unlock(&job->ui->pending_mutex);
    memset(job->password, 0, sizeof(job->password));
    memset(job->ssid, 0, sizeof(job->ssid));
    free(job);
    return NULL;
}

static void v9_net_status_refresh(smart_home_lvgl_t *ui)
{
    smart_home_network_status_t status;

    if (!ui || !ui->app || !g_settings.net_status) {
        return;
    }
    status = ui->app->system_status.network_status;
    if (status.online) {
        lv_label_set_text(g_settings.net_status, "已连接互联网");
        lv_obj_set_style_text_color(g_settings.net_status,
                                    SMART_HOME_UI_COLOR_SUCCESS, 0);
    } else if (status.ip_status == SMART_HOME_NETWORK_OK) {
        lv_label_set_text(g_settings.net_status,
                          "已连接 Wi-Fi，互联网/DNS 暂不可用");
        lv_obj_set_style_text_color(g_settings.net_status,
                                    SMART_HOME_UI_COLOR_WARNING, 0);
    } else if (g_net.active) {
        lv_label_set_text(g_settings.net_status, "正在连接，请稍候…");
        lv_obj_set_style_text_color(g_settings.net_status,
                                    SMART_HOME_UI_COLOR_TEXT_SECONDARY, 0);
    } else if (g_net.result < 0) {
        lv_label_set_text(g_settings.net_status, "连接失败，请检查密码或路由器");
        lv_obj_set_style_text_color(g_settings.net_status,
                                    SMART_HOME_UI_COLOR_DANGER, 0);
    } else {
        lv_label_set_text(g_settings.net_status, "未连接");
        lv_obj_set_style_text_color(g_settings.net_status,
                                    SMART_HOME_UI_COLOR_TEXT_MUTED, 0);
    }
    smart_home_lvgl_refresh_network_indicators(ui);
}

static void v9_net_timer_cb(lv_timer_t *timer)
{
    smart_home_lvgl_t *ui = lv_timer_get_user_data(timer);
    int ready = 0;
    int model_ready = 0;
    int model_result = 0;

    pthread_mutex_lock(&ui->pending_mutex);
    if (g_net.ready) {
        ready = 1;
        g_net.ready = 0;
        ui->app->system_status.network_status = g_net.status;
    }
    if (g_model.ready) {
        model_ready = 1;
        model_result = g_model.result;
        g_model.ready = 0;
    }
    pthread_mutex_unlock(&ui->pending_mutex);
    if (model_ready) {
        pthread_join(g_model.thread, NULL);
        g_model.active = 0;
        model_set_busy(0);
        if (g_settings.model_status) {
            lv_label_set_text(g_settings.model_status,
                model_result == AGENT_OK ? "本次配置已应用" : "配置应用失败");
            lv_obj_set_style_text_color(g_settings.model_status,
                model_result == AGENT_OK ? SMART_HOME_UI_COLOR_SUCCESS :
                                          SMART_HOME_UI_COLOR_DANGER, 0);
        }
    }
    if (ready) {
        pthread_join(g_net.thread, NULL);
        g_net.active = 0;
    }
    v9_net_status_refresh(ui);
}

static void v9_net_connect(smart_home_lvgl_t *ui)
{
    v9_net_job_t *job;
    pthread_attr_t attr;
    const char *ssid;
    const char *password;
    int ret;

    if (!ui || !ui->app || !g_settings.net_ssid ||
        !g_settings.net_password || g_net.active) {
        return;
    }
    ssid = lv_textarea_get_text(g_settings.net_ssid);
    password = lv_textarea_get_text(g_settings.net_password);
    if (!ssid || !ssid[0] || strlen(ssid) > 32u || strlen(password) > 64u) {
        lv_label_set_text(g_settings.net_status, "SSID 或密码长度不合法");
        return;
    }
    job = calloc(1u, sizeof(*job));
    if (!job) {
        lv_label_set_text(g_settings.net_status, "内存不足，无法开始连接");
        return;
    }
    job->ui = ui;
    snprintf(job->ssid, sizeof(job->ssid), "%s", ssid);
    snprintf(job->password, sizeof(job->password), "%s", password);
    g_net.owner = ui;
    g_net.result = 0;
    g_net.ready = 0;
    g_net.active = 1;
    lv_obj_add_flag(g_settings.keyboard, LV_OBJ_FLAG_HIDDEN);
    if (pthread_attr_init(&attr) != 0) {
        free(job);
        g_net.active = 0;
        return;
    }
    ret = pthread_attr_setstacksize(&attr, 16384u);
    if (ret == 0) ret = pthread_create(&g_net.thread, &attr, v9_net_worker, job);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        free(job);
        g_net.active = 0;
        lv_label_set_text(g_settings.net_status, "无法创建连接任务");
        return;
    }
    v9_net_status_refresh(ui);
}

static void v9_net_connect_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        v9_net_connect(lv_event_get_user_data(event));
    }
}

static lv_obj_t *model_input(lv_obj_t *panel, const char *caption, int x,
                             int y, int width, const char *placeholder);

static void build_network_panel(smart_home_lvgl_t *ui, lv_obj_t *panel)
{
    lv_obj_t *button;
    lv_obj_t *label;

    panel_title(panel, "无线网络");
    g_settings.net_ssid = model_input(panel, "Wi-Fi 名称（SSID）", 0, 68, 420,
                                      "家庭 Wi-Fi");
    g_settings.net_password = model_input(panel, "密码", 0, 136, 420,
                                          "开放网络可留空");
    lv_textarea_set_password_mode(g_settings.net_password, true);
    g_settings.net_status = smart_home_lvgl_label_create(
        panel, "未连接", SMART_HOME_UI_COLOR_TEXT_MUTED, 14);
    lv_obj_set_pos(g_settings.net_status, 0, 196);

    button = lv_button_create(panel);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 180, 40);
    lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    smart_home_lvgl_press_feedback(button);
    lv_obj_add_event_cb(button, v9_net_connect_cb, LV_EVENT_CLICKED, ui);
    label = smart_home_lvgl_label_create(button, "连接",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 14);
    lv_obj_center(label);
}

static void model_focus_cb(lv_event_t *event)
{
    lv_obj_t *textarea = lv_event_get_target(event);

    if (g_settings.keyboard) {
        lv_keyboard_set_textarea(g_settings.keyboard, textarea);
        lv_obj_clear_flag(g_settings.keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void model_keyboard_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(g_settings.keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(g_settings.keyboard, NULL);
    }
}

static lv_obj_t *model_input(lv_obj_t *panel, const char *caption, int x,
                             int y, int width, const char *placeholder)
{
    lv_obj_t *label = smart_home_lvgl_label_create(
        panel, caption, SMART_HOME_UI_COLOR_TEXT_SECONDARY, 13);
    lv_obj_t *textarea = lv_textarea_create(panel);

    lv_obj_set_pos(label, x, y - 20);
    lv_obj_set_pos(textarea, x, y);
    lv_obj_set_size(textarea, width, 40);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_placeholder_text(textarea, placeholder);
    lv_obj_add_event_cb(textarea, model_focus_cb, LV_EVENT_FOCUSED, NULL);
    return textarea;
}

static void model_save_draft(void)
{
    int index = g_settings.model_index;

    if (index < 0 || index >= 8) {
        return;
    }
    if (g_settings.model_name) {
        lv_textarea_get_text(g_settings.model_name);
        snprintf(g_settings.model_name_draft[index],
                 sizeof(g_settings.model_name_draft[index]), "%s",
                 lv_textarea_get_text(g_settings.model_name));
    }
    if (g_settings.model_key) {
        snprintf(g_settings.model_key_draft[index],
                 sizeof(g_settings.model_key_draft[index]), "%s",
                 lv_textarea_get_text(g_settings.model_key));
    }
    if (g_settings.model_timeout) {
        snprintf(g_settings.model_timeout_draft[index],
                 sizeof(g_settings.model_timeout_draft[index]), "%s",
                 lv_textarea_get_text(g_settings.model_timeout));
    }
    if (g_settings.model_tokens) {
        snprintf(g_settings.model_tokens_draft[index],
                 sizeof(g_settings.model_tokens_draft[index]), "%s",
                 lv_textarea_get_text(g_settings.model_tokens));
    }
}

static void model_load(smart_home_lvgl_t *ui, int index)
{
    const smart_home_llm_backend_preset_t *preset =
        smart_home_llm_backend_get((size_t)index);
    char buffer[32];

    g_settings.model_index = index;
    if (!preset) {
        return;
    }
    if (g_settings.model_name) {
        lv_textarea_set_text(g_settings.model_name,
                             g_settings.model_name_draft[index][0] ?
                             g_settings.model_name_draft[index] :
                             (preset->default_model ? preset->default_model : ""));
    }
    if (g_settings.model_key) {
        lv_textarea_set_text(g_settings.model_key,
                             g_settings.model_key_draft[index]);
    }
    if (g_settings.model_timeout) {
        if (g_settings.model_timeout_draft[index][0]) {
            lv_textarea_set_text(g_settings.model_timeout,
                                 g_settings.model_timeout_draft[index]);
        } else {
            snprintf(buffer, sizeof(buffer), "%u",
                     (unsigned)preset->default_timeout_ms);
            lv_textarea_set_text(g_settings.model_timeout, buffer);
        }
    }
    if (g_settings.model_tokens) {
        if (g_settings.model_tokens_draft[index][0]) {
            lv_textarea_set_text(g_settings.model_tokens,
                                 g_settings.model_tokens_draft[index]);
        } else if (ui && ui->app) {
            snprintf(buffer, sizeof(buffer), "%u",
                     (unsigned)ui->app->model_config.max_output_tokens);
            lv_textarea_set_text(g_settings.model_tokens, buffer);
        }
    }
    if (g_settings.model_status) {
        lv_label_set_text(g_settings.model_status, "切换后端会保留当前草稿");
        lv_obj_set_style_text_color(g_settings.model_status,
                                    SMART_HOME_UI_COLOR_TEXT_MUTED, 0);
    }
}

static void model_dropdown_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    int index;

    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    index = (int)lv_dropdown_get_selected(g_settings.model_dd);
    model_save_draft();
    model_load(ui, index);
}

static void model_apply_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    const smart_home_llm_backend_preset_t *preset;
    smart_home_model_config_t config;
    int index;
    int timeout;
    int tokens;
    int ret;
    pthread_attr_t attr;
    v9_model_job_t *job;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui || !ui->app) {
        return;
    }
    if (g_model.active) return;
    index = (int)lv_dropdown_get_selected(g_settings.model_dd);
    preset = smart_home_llm_backend_get((size_t)index);
    if (!preset) {
        return;
    }
    config = ui->app->model_config;
    if (!(preset->flags & SMART_HOME_LLM_FLAG_CUSTOM)) {
        (void)smart_home_model_config_from_preset(&config, preset, 0);
    }
    snprintf(config.backend_id, sizeof(config.backend_id), "%s", preset->id);
    snprintf(config.model, sizeof(config.model), "%s",
             lv_textarea_get_text(g_settings.model_name));
    snprintf(config.api_key, sizeof(config.api_key), "%s",
             lv_textarea_get_text(g_settings.model_key));
    timeout = atoi(lv_textarea_get_text(g_settings.model_timeout));
    if (timeout > 0) {
        config.timeout_ms = (uint32_t)timeout;
    }
    tokens = atoi(lv_textarea_get_text(g_settings.model_tokens));
    if (tokens > 0) {
        config.max_output_tokens = (uint32_t)tokens;
    }
    job = calloc(1u, sizeof(*job));
    if (!job) {
        memset(&config, 0, sizeof(config));
        lv_label_set_text(g_settings.model_status, "内存不足，无法应用配置");
        return;
    }
    job->ui = ui;
    job->config = config;
    memset(&config, 0, sizeof(config));
    ret = pthread_attr_init(&attr);
    if (ret == 0) {
        ret = pthread_attr_setstacksize(&attr, 16384u);
        g_model.owner = ui;
        g_model.ready = 0;
        g_model.active = 1;
        if (ret == 0)
            ret = pthread_create(&g_model.thread, &attr, v9_model_worker, job);
        pthread_attr_destroy(&attr);
    }
    if (ret != 0) {
        memset(&job->config, 0, sizeof(job->config));
        free(job);
        g_model.active = 0;
        lv_label_set_text(g_settings.model_status, "无法创建配置任务");
        return;
    }
    model_set_busy(1);
    lv_label_set_text(g_settings.model_status, "正在应用，等待当前请求完成…");
    lv_obj_set_style_text_color(g_settings.model_status,
                                SMART_HOME_UI_COLOR_TEXT_MUTED, 0);
    if (g_settings.keyboard) {
        lv_obj_add_flag(g_settings.keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_model_panel(smart_home_lvgl_t *ui, lv_obj_t *panel)
{
    char options[256] = "";
    size_t count = smart_home_llm_backend_count();
    int initial = 0;

    panel_title(panel, "模型服务");
    g_settings.model_dd = lv_dropdown_create(panel);
    lv_obj_set_pos(g_settings.model_dd, 0, 68);
    lv_obj_set_size(g_settings.model_dd, 240, 40);
    for (size_t i = 0; i < count; i++) {
        const smart_home_llm_backend_preset_t *preset =
            smart_home_llm_backend_get(i);
        if (!preset) continue;
        if (i) strncat(options, "\n", sizeof(options) - strlen(options) - 1);
        strncat(options, preset->name, sizeof(options) - strlen(options) - 1);
    }
    lv_dropdown_set_options(g_settings.model_dd, options);
    lv_obj_add_event_cb(g_settings.model_dd, model_dropdown_cb,
                        LV_EVENT_VALUE_CHANGED, ui);

    g_settings.model_name = model_input(panel, "模型标识", 260, 68, 300,
                                        "模型名称");
    g_settings.model_key = model_input(panel, "API 密钥", 0, 136, 560,
                                       "留空沿用系统凭据，本次运行有效");
    lv_textarea_set_password_mode(g_settings.model_key, true);
    g_settings.model_timeout = model_input(panel, "请求超时 / 毫秒", 0, 204,
                                           160, "30000");
    g_settings.model_tokens = model_input(panel, "最大输出 / 词元", 180, 204,
                                          160, "512");

    g_settings.model_status = smart_home_lvgl_label_create(
        panel, "", SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
    lv_obj_set_pos(g_settings.model_status, 0, 258);

    {
        lv_obj_t *button = lv_button_create(panel);
        lv_obj_t *label;
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, 180, 40);
        lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_set_style_radius(button, 10, 0);
        lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        smart_home_lvgl_press_feedback(button);
        g_settings.model_apply = button;
        lv_obj_add_event_cb(button, model_apply_cb, LV_EVENT_CLICKED, ui);
        label = smart_home_lvgl_label_create(button, "应用本次配置",
                                             SMART_HOME_UI_COLOR_TEXT_PRIMARY, 14);
        lv_obj_center(label);
    }

    if (ui && ui->app) {
        initial = smart_home_llm_backend_index_for_config(
            &ui->app->model_config);
    }
    if (initial < 0 || initial >= (int)count) {
        initial = 0;
    }
    lv_dropdown_set_selected(g_settings.model_dd, (uint32_t)initial);
    model_load(ui, initial);
}

static void tool_switch_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    lv_obj_t *sw = lv_event_get_current_target(event);
    const char *name = lv_obj_get_user_data(sw);

    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || !ui || !ui->app ||
        !name) {
        return;
    }
    (void)smart_home_agent_app_set_local_tool_access(
        ui->app, name, lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0);
}

static void build_tools_panel(smart_home_lvgl_t *ui, lv_obj_t *panel)
{
    size_t count = ui && ui->app ? ui->app->local_tool_access_count : 0;
    int y = 56;

    panel_title(panel, "工具权限");
    if (!ui || !ui->app) {
        panel_row(panel, y, "工具目录", "服务未初始化");
        return;
    }
    if (count == 0) {
        panel_row(panel, y, "本地工具", "尚未加载工具目录");
    }
    for (size_t i = 0; i < count && i < 6; i++) {
        smart_home_local_tool_access_t *tool =
            &ui->app->local_tool_access[i];
        lv_obj_t *name = smart_home_lvgl_label_create(
            panel, tool->name, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 17);
        lv_obj_t *sub = smart_home_lvgl_label_create(
            panel, "本地工具 · 本次运行有效",
            SMART_HOME_UI_COLOR_TEXT_MUTED, 12);
        lv_obj_t *sw = lv_switch_create(panel);

        lv_obj_set_pos(name, 0, y);
        lv_obj_set_pos(sub, 0, y + 22);
        lv_obj_set_size(sw, 50, 28);
        lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, 0, y + 2);
        lv_obj_set_style_bg_color(sw, lv_color_hex(0xCCD6D6), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(sw, 14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, SMART_HOME_UI_COLOR_PRIMARY,
                                  LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
        lv_obj_set_style_border_width(sw, 0, LV_PART_KNOB);
        lv_obj_set_style_radius(sw, 12, LV_PART_KNOB);
        lv_obj_set_style_pad_all(sw, 2, LV_PART_KNOB);
        lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
        if (tool->allowed) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_set_user_data(sw, tool->name);
        lv_obj_add_event_cb(sw, tool_switch_cb, LV_EVENT_VALUE_CHANGED, ui);
        y += 54;
    }
    panel_button(panel, "打开完整工具页", ui->screen_tools);
}

static void services_modal_close(void)
{
    if (g_settings.services_modal) {
        lv_obj_delete(g_settings.services_modal);
        g_settings.services_modal = NULL;
    }
}

static void services_mask_cb(lv_event_t *event)
{
    (void)event;
    services_modal_close();
}

static void services_stop_cb(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
}

static void services_modal_open(smart_home_lvgl_t *ui)
{
    lv_obj_t *mask;
    lv_obj_t *panel;
    lv_obj_t *label;
    static const char *const names[3] = {
        "米家网关", "MCP 服务", "局域网 Node"
    };

    if (!g_settings.screen) {
        return;
    }
    services_modal_close();
    mask = lv_obj_create(g_settings.screen);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, smart_home_lvgl_disp_w(), smart_home_lvgl_disp_h());
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_30, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mask, services_mask_cb, LV_EVENT_CLICKED, NULL);
    g_settings.services_modal = mask;

    panel = lv_obj_create(mask);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 680, 360);
    lv_obj_center(panel);
    smart_home_lvgl_card_style(panel);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_pad_all(panel, 24, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, services_stop_cb, LV_EVENT_CLICKED, NULL);

    label = smart_home_lvgl_label_create(panel, "设备与服务",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 22);
    lv_obj_set_pos(label, 0, 0);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *value;
        int y = 56 + i * 46;

        label = smart_home_lvgl_label_create(panel, names[i],
                                             SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                             16);
        lv_obj_set_pos(label, 0, y);
        value = smart_home_lvgl_label_create(panel,
                                             g_settings.service_status[i],
                                             SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                             14);
        lv_obj_align(value, LV_ALIGN_TOP_RIGHT, 0, y);
    }

    {
        lv_obj_t *button = lv_button_create(panel);
        lv_obj_t *button_label;
        lv_obj_t *targets[3] = {
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
            ui && ui->screen_miloco ? ui->screen_miloco : NULL,
#else
            NULL,
#endif
            ui && ui->screen_mcp ? ui->screen_mcp : NULL,
            ui && ui->screen_node ? ui->screen_node : NULL
        };
        static const char *const captions[3] = { "打开米家", "打开 MCP", "打开 Node" };

        for (int i = 0; i < 3; i++) {
            button = lv_button_create(panel);
            lv_obj_remove_style_all(button);
            lv_obj_set_size(button, 120, 36);
            lv_obj_set_pos(button, i * 132, 290);
            lv_obj_set_style_radius(button, 8, 0);
            lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
            lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
            smart_home_lvgl_press_feedback(button);
            lv_obj_add_event_cb(button, route_cb, LV_EVENT_CLICKED,
                                targets[i]);
            button_label = smart_home_lvgl_label_create(
                button, captions[i], SMART_HOME_UI_COLOR_TEXT_PRIMARY, 13);
            lv_obj_center(button_label);
        }
    }

#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
    {
        lv_obj_t *button = lv_button_create(panel);
        lv_obj_t *button_label;

        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, 140, 34);
        lv_obj_set_pos(button, 0, 238);
        lv_obj_set_style_radius(button, 8, 0);
        lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        smart_home_lvgl_press_feedback(button);
        lv_obj_add_event_cb(button, mcp_config_open, LV_EVENT_CLICKED, ui);
        button_label = smart_home_lvgl_label_create(
            button, "MCP 配置", SMART_HOME_UI_COLOR_TEXT_PRIMARY, 13);
        lv_obj_center(button_label);
    }
#endif
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
    {
        lv_obj_t *button = lv_button_create(panel);
        lv_obj_t *button_label;

        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, 140, 34);
        lv_obj_set_pos(button, 156, 238);
        lv_obj_set_style_radius(button, 8, 0);
        lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        smart_home_lvgl_press_feedback(button);
        lv_obj_add_event_cb(button, node_config_open, LV_EVENT_CLICKED, ui);
        button_label = smart_home_lvgl_label_create(
            button, "Node 配置", SMART_HOME_UI_COLOR_TEXT_PRIMARY, 13);
        lv_obj_center(button_label);
    }
#endif

    {
        lv_obj_t *button = lv_button_create(panel);
        lv_obj_t *button_label;

        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, 100, 36);
        lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_set_style_radius(button, 8, 0);
        lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(button, services_mask_cb, LV_EVENT_CLICKED, NULL);
        button_label = smart_home_lvgl_label_create(
            button, "关闭", SMART_HOME_UI_COLOR_TEXT_PRIMARY, 13);
        lv_obj_center(button_label);
    }
}

static void services_modal_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);

    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        services_modal_open(ui);
    }
}

#if defined(CONFIG_SMART_HOME_MCP_BRIDGE) || defined(CONFIG_SMART_HOME_NODE_GATEWAY)
static void config_modal_close(void)
{
    if (g_settings.config_modal) {
        lv_obj_delete(g_settings.config_modal);
        g_settings.config_modal = NULL;
    }
}

static void config_modal_mask_cb(lv_event_t *event)
{
    (void)event;
    config_modal_close();
}

static lv_obj_t *config_modal_begin(const char *title)
{
    lv_obj_t *mask;
    lv_obj_t *panel;
    lv_obj_t *label;

    config_modal_close();
    mask = lv_obj_create(g_settings.screen);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, smart_home_lvgl_disp_w(), smart_home_lvgl_disp_h());
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_30, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mask, config_modal_mask_cb, LV_EVENT_CLICKED, NULL);
    g_settings.config_modal = mask;

    panel = lv_obj_create(mask);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 680, 420);
    lv_obj_center(panel);
    smart_home_lvgl_card_style(panel);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_pad_all(panel, 24, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, services_stop_cb, LV_EVENT_CLICKED, NULL);

    label = smart_home_lvgl_label_create(panel, title,
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 22);
    lv_obj_set_pos(label, 0, 0);

    label = lv_button_create(panel);
    lv_obj_remove_style_all(label);
    lv_obj_set_size(label, 100, 34);
    lv_obj_align(label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(label, 8, 0);
    lv_obj_set_style_bg_color(label, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(label, config_modal_mask_cb, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *close_label = smart_home_lvgl_label_create(
            label, "关闭", SMART_HOME_UI_COLOR_TEXT_PRIMARY, 13);
        lv_obj_center(close_label);
    }
    return panel;
}
#endif

#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
static void mcp_apply_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    smart_home_mcp_runtime_config_t config;
    int ret;
    char *end;
    long port;
    const char *port_text;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui || !ui->app ||
        !ui->app->mcp_bridge || !g_settings.mcp_server) {
        return;
    }
    memset(&config, 0, sizeof(config));
    snprintf(config.server_id, sizeof(config.server_id), "%s",
             lv_textarea_get_text(g_settings.mcp_server));
    snprintf(config.host, sizeof(config.host), "%s",
             lv_textarea_get_text(g_settings.mcp_host));
    snprintf(config.path, sizeof(config.path), "%s",
             lv_textarea_get_text(g_settings.mcp_path));
    snprintf(config.authorization, sizeof(config.authorization), "%s",
             lv_textarea_get_text(g_settings.mcp_auth));
    port_text = lv_textarea_get_text(g_settings.mcp_port);
    port = strtol(port_text, &end, 10);
    if (end == port_text || *end != '\0' || port < 1 || port > 65535) {
        memset(&config, 0, sizeof(config));
        lv_label_set_text(g_settings.mcp_status, "端口须为 1～65535 的整数");
        return;
    }
    config.port = (uint16_t)port;
    config.use_tls = lv_dropdown_get_selected(g_settings.mcp_protocol) == 0;
    ret = smart_home_mcp_bridge_reconfigure(ui->app->mcp_bridge, &config);
    memset(&config, 0, sizeof(config));
    if (g_settings.mcp_status) {
        lv_label_set_text(g_settings.mcp_status,
                          ret == AGENT_OK ? "配置已应用，请重新发现工具" :
                          ret == AGENT_ERROR_BUSY ? "发现进行中，请稍后重试" :
                                                    "配置应用失败");
    }
}

static void mcp_config_open(smart_home_lvgl_t *ui)
{
    lv_obj_t *panel = config_modal_begin("MCP 服务配置");
    lv_obj_t *button;
    lv_obj_t *label;

    g_settings.mcp_server = model_input(panel, "服务名称", 0, 60, 240,
                                        "server id");
    g_settings.mcp_protocol = lv_dropdown_create(panel);
    lv_obj_set_pos(g_settings.mcp_protocol, 260, 60);
    lv_obj_set_size(g_settings.mcp_protocol, 200, 40);
    lv_dropdown_set_options(g_settings.mcp_protocol, "HTTPS (TLS)\nHTTP");
    lv_dropdown_set_selected(g_settings.mcp_protocol, 0);
    g_settings.mcp_host = model_input(panel, "主机", 0, 120, 340, "host");
    g_settings.mcp_port = model_input(panel, "端口", 360, 120, 100, "443");
    lv_textarea_set_text(g_settings.mcp_port, "443");
    g_settings.mcp_path = model_input(panel, "路径", 0, 180, 500, "/mcp");
    g_settings.mcp_auth = model_input(panel, "Authorization", 0, 240, 500,
                                      "Bearer ...");
    lv_textarea_set_password_mode(g_settings.mcp_auth, true);
    g_settings.mcp_status = smart_home_lvgl_label_create(
        panel, "本次运行有效", SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
    lv_obj_set_pos(g_settings.mcp_status, 0, 300);

    button = lv_button_create(panel);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 160, 36);
    lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    smart_home_lvgl_press_feedback(button);
    lv_obj_add_event_cb(button, mcp_apply_cb, LV_EVENT_CLICKED, ui);
    label = smart_home_lvgl_label_create(button, "应用配置",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 13);
    lv_obj_center(label);
}
#endif

#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
static void node_toggle_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    int port;
    int ret;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui || !ui->app) {
        return;
    }
    if (ui->app->node_gateway) {
        smart_home_node_gateway_stop(&ui->app->node_gateway);
        lv_label_set_text(g_settings.node_status, "已停止");
        return;
    }
    port = atoi(lv_textarea_get_text(g_settings.node_port));
    if (port <= 0 || port > 65535) {
        lv_label_set_text(g_settings.node_status, "端口不合法");
        return;
    }
    ret = smart_home_node_gateway_start_with_port(
        &ui->app->node_gateway, ui->app->agent, &ui->app->agent_mutex,
        (uint16_t)port);
    lv_label_set_text(g_settings.node_status,
                      ret == AGENT_OK ? "监听中" : "开启失败");
}

static void node_config_open(smart_home_lvgl_t *ui)
{
    lv_obj_t *panel = config_modal_begin("局域网 Node 配置");
    lv_obj_t *button;
    lv_obj_t *label;
    char buffer[16];

    g_settings.node_port = model_input(panel, "监听端口", 0, 60, 160,
                                       "18790");
    g_settings.node_status = smart_home_lvgl_label_create(
        panel, ui && ui->app && ui->app->node_gateway ? "监听中" : "未开启",
        SMART_HOME_UI_COLOR_TEXT_SECONDARY, 14);
    lv_obj_set_pos(g_settings.node_status, 0, 130);
    snprintf(buffer, sizeof(buffer), "%d",
             (int)CONFIG_SMART_HOME_NODE_GATEWAY_PORT);
    lv_textarea_set_text(g_settings.node_port, buffer);

    button = lv_button_create(panel);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 160, 36);
    lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    smart_home_lvgl_press_feedback(button);
    lv_obj_add_event_cb(button, node_toggle_cb, LV_EVENT_CLICKED, ui);
    label = smart_home_lvgl_label_create(button, "开启 / 停止",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 13);
    lv_obj_center(label);
}
#endif

static void build_services_panel(smart_home_lvgl_t *ui, lv_obj_t *panel)
{
    const char *miloco = "未启用";
    const char *mcp = "未启用";
    const char *node = "未启用";

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (ui && ui->app && ui->app->miloco) {
        if (!smart_home_miloco_reachable(ui->app->miloco)) {
            miloco = "未连接";
        } else if (smart_home_miloco_bound(ui->app->miloco)) {
            miloco = "已绑定";
        } else {
            miloco = "待绑定";
        }
    } else {
        miloco = "未连接";
    }
#endif
#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
    mcp = (ui && ui->app &&
           ui->app->system_status.mcp_bridge_status == AGENT_OK) ?
          "已就绪" : "未就绪";
#endif
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
    node = (ui && ui->app &&
            ui->app->system_status.node_gateway_status == AGENT_OK) ?
           "监听中" : "未开启";
#endif

    snprintf(g_settings.service_status[0], sizeof(g_settings.service_status[0]),
             "%s", miloco);
    snprintf(g_settings.service_status[1], sizeof(g_settings.service_status[1]),
             "%s", mcp);
    snprintf(g_settings.service_status[2], sizeof(g_settings.service_status[2]),
             "%s", node);
    panel_title(panel, "设备与服务");
    panel_row(panel, 56, "米家网关", miloco);
    panel_row(panel, 106, "MCP 服务", mcp);
    panel_row(panel, 156, "局域网 Node", node);

    {
        lv_obj_t *button = lv_button_create(panel);
        lv_obj_t *label;

        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, 180, 40);
        lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_set_style_radius(button, 10, 0);
        lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        smart_home_lvgl_press_feedback(button);
        lv_obj_add_event_cb(button, services_modal_cb, LV_EVENT_CLICKED, ui);
        label = smart_home_lvgl_label_create(button, "查看连接状态",
                                             SMART_HOME_UI_COLOR_TEXT_PRIMARY, 14);
        lv_obj_center(label);
    }
}

static void build_health_panel(smart_home_lvgl_t *ui, lv_obj_t *panel)
{
    int net = ui && ui->app && ui->app->system_status.network_status.online;
    int model = ui && ui->app &&
                ui->app->system_status.model_status == AGENT_OK;
    int tools = ui && ui->app &&
                ui->app->system_status.tools_status == AGENT_OK;

    panel_title(panel, "系统健康");
    panel_row(panel, 56, "家庭网络", net ? "已连接互联网" : "未连接");
    panel_row(panel, 106, "模型服务", state_text(model));
    panel_row(panel, 156, "本地工具", state_text(tools));
    panel_row(panel, 206, "最近错误", "无");
    panel_button(panel, "打开系统健康", ui ? ui->screen_settings : NULL);
}

void smart_home_lvgl_build_settings_screen_v9(smart_home_lvgl_t *ui)
{
    lv_obj_t *screen;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *left;
    lv_obj_t *right;

    if (!ui) {
        return;
    }
    memset(&g_settings, 0, sizeof(g_settings));

    screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    smart_home_lvgl_set_bg(screen, SMART_HOME_UI_COLOR_BG);
    ui->screen_settings_v9 = screen;
    g_settings.screen = screen;
    smart_home_lvgl_build_top_bar(screen, ui, "设置");

    title = smart_home_lvgl_label_create(screen, "设置",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 24);
    lv_obj_set_pos(title, 24, 76);
    subtitle = smart_home_lvgl_label_create(screen, "连接家庭，按习惯调整",
                                            SMART_HOME_UI_COLOR_TEXT_SECONDARY, 14);
    lv_obj_set_pos(subtitle, 24, 110);

    left = lv_obj_create(screen);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, 208, 384);
    lv_obj_set_pos(left, 24, 136);
    smart_home_lvgl_card_style(left);
    lv_obj_set_style_radius(left, 18, 0);
    lv_obj_set_style_pad_all(left, 0, 0);

    right = lv_obj_create(screen);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, 752, 384);
    lv_obj_set_pos(right, 248, 136);
    smart_home_lvgl_card_style(right);
    lv_obj_set_style_radius(right, 18, 0);
    lv_obj_set_style_pad_all(right, 24, 0);

    for (int i = 0; i < V9_SETTINGS_CATEGORIES; i++) {
        lv_obj_t *button = lv_button_create(left);
        lv_obj_t *label;

        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, 184, 50);
        lv_obj_set_pos(button, 12, 12 + i * 58);
        lv_obj_set_style_radius(button, 12, 0);
        lv_obj_set_user_data(button, (void *)(intptr_t)i);
        lv_obj_add_event_cb(button, category_cb, LV_EVENT_CLICKED, NULL);
        smart_home_lvgl_press_feedback(button);
        g_settings.button[i] = button;

        label = smart_home_lvgl_label_create(button, g_category_names[i],
                                             SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                             16);
        lv_obj_set_pos(label, 34, 14);
    }

    g_settings.panel[0] = make_panel(right);
    g_settings.panel[1] = make_panel(right);
    g_settings.panel[2] = make_panel(right);
    g_settings.panel[3] = make_panel(right);
    g_settings.panel[4] = make_panel(right);
    g_settings.panel[5] = make_panel(right);

    build_display_panel(g_settings.panel[0], ui->screen_settings);
    build_network_panel(ui, g_settings.panel[1]);
    build_model_panel(ui, g_settings.panel[2]);
    build_tools_panel(ui, g_settings.panel[3]);
    build_services_panel(ui, g_settings.panel[4]);
    build_health_panel(ui, g_settings.panel[5]);

    g_settings.keyboard = lv_keyboard_create(screen);
    smart_home_lvgl_style_keyboard(g_settings.keyboard);
    lv_obj_set_size(g_settings.keyboard, 704, 180);
    lv_obj_align(g_settings.keyboard, LV_ALIGN_BOTTOM_MID, 0, -72);
    lv_obj_add_event_cb(g_settings.keyboard, model_keyboard_cb, LV_EVENT_ALL,
                        NULL);
    lv_obj_add_flag(g_settings.keyboard, LV_OBJ_FLAG_HIDDEN);

    if (ui->v9_network_timer) {
        lv_timer_delete(ui->v9_network_timer);
    }
    ui->v9_network_timer = lv_timer_create(v9_net_timer_cb, 250, ui);

    panel_show(0);
    smart_home_lvgl_build_nav_bar(screen, ui);
}

int smart_home_lvgl_v9_settings_category_count(void)
{
    return V9_SETTINGS_CATEGORIES;
}

int smart_home_lvgl_v9_settings_select_category(int index)
{
    if (index < 0 || index >= V9_SETTINGS_CATEGORIES) {
        return -1;
    }
    panel_show(index);
    return index;
}
