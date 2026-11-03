/** V9 Settings shell: six internal categories, summary panels and routes. */

#include "smart_home_lvgl_internal.h"
#include "images/smart_home_icons.h"
#include "../../config/smart_home_backends.h"
#include "../../config/smart_home_secrets.h"
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
#include <time.h>

#define V9_SETTINGS_CATEGORIES 7

static const char *const g_category_icons[V9_SETTINGS_CATEGORIES] = {
    "asset:sun", "asset:wifi", "asset:setting-ai", "asset:microphone",
    "asset:shield", "asset:devices", "asset:status-ok"
};

static const char *const g_category_names[V9_SETTINGS_CATEGORIES] = {
    "显示与声音", "网络连接", "模型服务", "语音服务",
    "工具权限", "设备与服务", "系统健康"
};

/* 分类索引常量：panel[] 的填充顺序与左侧按钮顺序必须一致，
 * 内部跳转不要写裸数字。 */
#define V9_CAT_DISPLAY   0
#define V9_CAT_NETWORK   1
#define V9_CAT_MODEL     2
#define V9_CAT_VOICE     3
#define V9_CAT_TOOLS     4
#define V9_CAT_SERVICES  5
#define V9_CAT_HEALTH    6

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *panel[V9_SETTINGS_CATEGORIES];
    lv_obj_t *button[V9_SETTINGS_CATEGORIES];
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
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    lv_obj_t *miloco_host;
    lv_obj_t *miloco_port;
    lv_obj_t *miloco_token;
    lv_obj_t *miloco_modal_status;
    lv_timer_t *miloco_save_timer;
#endif
    lv_obj_t *model_dd;
    lv_obj_t *model_name;
    lv_obj_t *model_key;
    lv_obj_t *model_timeout;
    lv_obj_t *model_tokens;
    lv_obj_t *model_status;
    lv_obj_t *model_apply;
    lv_obj_t *net_ssid;
    lv_obj_t *net_connected;
    lv_obj_t *net_password;
    lv_obj_t *net_status;
    lv_obj_t *health_badge;
    lv_obj_t *health_badge_label;
    lv_obj_t *health_note;
    lv_obj_t *health_network;
    lv_obj_t *health_miloco;
    lv_obj_t *health_model;
    lv_obj_t *health_tools;
    lv_obj_t *health_error;
    lv_obj_t *health_meta;
    lv_obj_t *health_refresh;
    lv_obj_t *health_refresh_label;
    time_t health_checked_at;
    lv_obj_t *keyboard;
    int model_index;
    char model_name_draft[8][64];
    char model_key_draft[8][256];
    char model_timeout_draft[8][16];
    char model_tokens_draft[8][16];
    int category;
} v9_settings_t;

static v9_settings_t g_settings;
static void health_panel_refresh(smart_home_lvgl_t *ui);

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
        lv_obj_align(g_settings.keyboard, LV_ALIGN_BOTTOM_MID, 0, -72);
    }
    for (int i = 0; i < V9_SETTINGS_CATEGORIES; i++) {
        lv_obj_t *btn = g_settings.button[i];
        lv_obj_t *label;
        if (btn) {
            label = lv_obj_get_child(btn, 1);
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
    lv_obj_t *button = lv_event_get_current_target(event);
    int index = (int)(intptr_t)lv_obj_get_user_data(button);

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

static void display_unavailable(lv_obj_t *panel, const char *icon_name,
                                const char *title, int y)
{
    lv_obj_t *icon = smart_home_lvgl_icon_create(panel, icon_name, 32, 32);
    lv_obj_t *label;
    lv_obj_t *track = lv_obj_create(panel);

    if (icon) lv_obj_set_pos(icon, 0, y - 4);
    label = smart_home_lvgl_label_create(panel, title,
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 18);
    lv_obj_set_pos(label, 46, y);
    label = smart_home_lvgl_label_create(panel, "暂不可用",
                                         SMART_HOME_UI_COLOR_TEXT_MUTED, 14);
    lv_obj_align(label, LV_ALIGN_TOP_RIGHT, 0, y);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, 628, 8);
    lv_obj_set_pos(track, 46, y + 49);
    lv_obj_set_style_radius(track, 4, 0);
    smart_home_lvgl_set_bg(track, SMART_HOME_UI_COLOR_SURFACE_ON);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

static void build_display_panel(lv_obj_t *panel, lv_obj_t *settings_target)
{
    lv_obj_t *label;
    lv_obj_t *sw;
    const char *choices[] = {"1分钟", "5分钟", "永不"};

    (void)settings_target;
    display_unavailable(panel, "asset:sun", "屏幕亮度", 0);
    display_unavailable(panel, "asset:volume", "播报音量", 102);
    label = smart_home_lvgl_label_create(panel, "自动进入屏保",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 18);
    lv_obj_set_pos(label, 0, 202);
    label = smart_home_lvgl_label_create(panel, "自动屏保暂不可用",
                                         SMART_HOME_UI_COLOR_TEXT_MUTED, 14);
    lv_obj_set_pos(label, 0, 230);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *button = lv_button_create(panel);
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, 120, 48);
        lv_obj_set_pos(button, 316 + i * 128, 204);
        lv_obj_set_style_radius(button, 12, 0);
        smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_SURFACE_ON);
        lv_obj_add_state(button, LV_STATE_DISABLED);
        label = smart_home_lvgl_label_create(button, choices[i],
                                             SMART_HOME_UI_COLOR_TEXT_MUTED, 18);
        lv_obj_center(label);
    }
    label = smart_home_lvgl_label_create(panel, "设备状态动效",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 18);
    lv_obj_set_pos(label, 0, 290);
    label = smart_home_lvgl_label_create(panel, "当前显示静态设备图标",
                                         SMART_HOME_UI_COLOR_TEXT_MUTED, 14);
    lv_obj_set_pos(label, 0, 318);
    sw = lv_switch_create(panel);
    lv_obj_set_size(sw, 54, 30);
    lv_obj_set_pos(sw, 650, 302);
    lv_obj_add_state(sw, LV_STATE_DISABLED);
}

typedef struct {
    smart_home_lvgl_t *ui;
    char ssid[33];
    char password[65];
    int probe;
    smart_home_network_status_t status;
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
    bool persistent;
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
    char saved_key[sizeof(job->config.api_key)] = "";
    char error[96];
    int ret = smart_home_model_config_validate(&job->config, error, sizeof(error));

    if (ret == AGENT_OK && job->config.api_key[0] &&
        strcmp(job->config.backend_id, "custom") != 0) {
        int found = smart_home_secrets_get_model_api_key(job->config.backend_id,
                                                         saved_key, sizeof(saved_key));
        if (found != AGENT_OK || strcmp(saved_key, job->config.api_key) != 0)
            ret = smart_home_secrets_set_api_key(SMART_HOME_SECRET_LLM,
                                                  job->config.backend_id,
                                                  job->config.api_key);
    }
    memset(saved_key, 0, sizeof(saved_key));
    if (ret == AGENT_OK)
        ret = smart_home_agent_app_apply_model_config(ui->app, &job->config);

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
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (g_settings.miloco_save_timer) {
        lv_timer_delete(g_settings.miloco_save_timer);
        g_settings.miloco_save_timer = NULL;
    }
#endif
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
#if defined(CONFIG_SMART_HOME_VOICE_ASR) || defined(CONFIG_SMART_HOME_VOICE_TTS)
    /* 语音任务持有面板控件指针，必须在 g_settings 清零前收掉。 */
    smart_home_lvgl_voice_settings_deinit(ui);
#endif
    memset(&g_settings, 0, sizeof(g_settings));
}

static void *v9_net_worker(void *argument)
{
    v9_net_job_t *job = argument;
    smart_home_network_status_t status;
    int ret;

    if (!job || !job->ui || !job->ui->app) {
        free(job);
        return NULL;
    }
    if (job->probe) {
        status = job->status;
        ret = smart_home_network_probe(&status);
    } else {
        memset(&status, 0, sizeof(status));
        ret = smart_home_network_connect_credentials(&status, job->ssid,
                                                      job->password);
    }
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
    if (g_settings.net_connected) {
        if ((status.online || status.ip_status == SMART_HOME_NETWORK_OK) &&
            status.ssid[0]) {
            lv_label_set_text_fmt(g_settings.net_connected,
                                   "当前连接：%s", status.ssid);
        } else {
            lv_label_set_text(g_settings.net_connected,
                status.online || status.ip_status == SMART_HOME_NETWORK_OK ?
                "当前连接：网络名称暂不可用" : "当前未连接 Wi-Fi");
        }
    }
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

#if defined(CONFIG_SMART_HOME_VOICE_ASR) || defined(CONFIG_SMART_HOME_VOICE_TTS)
    /* 语音服务面板的保存/测试任务在独立线程里跑，结果在这里回到 LVGL。 */
    smart_home_lvgl_voice_settings_poll(ui);
#endif

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
                model_result != AGENT_OK ? "配置保存或应用失败" :
                g_model.persistent ? "配置已保存并应用" : "配置已应用（仅本次有效）");
            lv_obj_set_style_text_color(g_settings.model_status,
                model_result == AGENT_OK ? SMART_HOME_UI_COLOR_SUCCESS :
                                          SMART_HOME_UI_COLOR_DANGER, 0);
        }
    }
    if (ready) {
        pthread_join(g_net.thread, NULL);
        g_net.active = 0;
        g_settings.health_checked_at = time(NULL);
        if (g_settings.health_refresh) {
            lv_obj_remove_state(g_settings.health_refresh, LV_STATE_DISABLED);
        }
        if (g_settings.health_refresh_label) {
            lv_label_set_text(g_settings.health_refresh_label, "刷新状态");
        }
        health_panel_refresh(ui);
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
    g_settings.net_connected = smart_home_lvgl_label_create(
        panel, "当前未连接 Wi-Fi", SMART_HOME_UI_COLOR_TEXT_SECONDARY, 16);
    lv_obj_set_pos(g_settings.net_connected, 0, 236);
    lv_obj_set_width(g_settings.net_connected, 704);
    lv_label_set_long_mode(g_settings.net_connected, LV_LABEL_LONG_WRAP);
    if (ui && ui->app) {
        char ssid[33] = "";
        char password[65] = "";
        if (smart_home_secrets_get_wifi_credentials(ssid, sizeof(ssid),
                                                    password, sizeof(password)) == AGENT_OK) {
            lv_textarea_set_text(g_settings.net_ssid, ssid);
            lv_textarea_set_text(g_settings.net_password, password);
        } else {
            const smart_home_network_status_t *status =
                &ui->app->system_status.network_status;
            if ((status->online || status->ip_status == SMART_HOME_NETWORK_OK) &&
                status->ssid[0]) lv_textarea_set_text(g_settings.net_ssid, status->ssid);
        }
        memset(password, 0, sizeof(password));
    }
    v9_net_status_refresh(ui);

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

/* 弹窗的原始尺寸。键盘弹出时面板要缩到键盘上方的可用区里，收起时再还原，
 * 两个方向都从这里取值，避免尺寸漂移。 */
#define CONFIG_MODAL_W 680
#define CONFIG_MODAL_H 420
/* 面板与键盘、屏幕顶端之间至少留出的缝隙。 */
#define CONFIG_MODAL_GAP 8

static void config_modal_restore_panel(void)
{
    lv_obj_t *panel;

    if (!g_settings.config_modal) {
        return;
    }
    panel = lv_obj_get_child(g_settings.config_modal, 0);
    if (!panel) {
        return;
    }
    lv_obj_set_size(panel, CONFIG_MODAL_W, CONFIG_MODAL_H);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_bottom(panel, 24, 0);
    lv_obj_set_scroll_dir(panel, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_scroll_to(panel, 0, 0, LV_ANIM_OFF);
}

/* 键盘浮在屏幕底部、弹窗居中，靠下的输入框和弹窗底部按钮都会落到键盘
 * 后面。聚焦时把面板缩进键盘上方的可用区，让输入框和按钮同时可见。 */
static void config_modal_reveal_input(lv_obj_t *textarea)
{
    lv_obj_t *panel;
    lv_area_t input_area;
    lv_area_t keyboard_area;

    if (!g_settings.config_modal || !g_settings.keyboard || !textarea) {
        return;
    }
    /* 只处理弹窗内的输入框；设置面板自身的输入框没有遮罩，不需要位移。 */
    {
        lv_obj_t *walk = lv_obj_get_parent(textarea);

        while (walk && walk != g_settings.config_modal) {
            walk = lv_obj_get_parent(walk);
        }
        if (!walk) {
            return;
        }
    }
    panel = lv_obj_get_child(g_settings.config_modal, 0);
    if (!panel) {
        return;
    }
    /* 键盘收起时面板回到原始尺寸并居中。 */
    if (lv_obj_has_flag(g_settings.keyboard, LV_OBJ_FLAG_HIDDEN)) {
        config_modal_restore_panel();
        return;
    }
    /* 600 高的屏上键盘上沿约 348，扣掉缝隙后可用高度只有 340，装不下 420
     * 的面板。把面板缩到可用区并允许内部滚动，否则面板底部的「保存 /
     * 返回」按钮会一直藏在键盘后面，无法点击。 */
    lv_obj_get_coords(g_settings.keyboard, &keyboard_area);
    {
        int32_t avail_top = CONFIG_MODAL_GAP;
        int32_t avail_bottom = keyboard_area.y1 - CONFIG_MODAL_GAP;
        int32_t avail_h = avail_bottom - avail_top;
        int32_t panel_h = CONFIG_MODAL_H;

        if (panel_h > avail_h) {
            panel_h = avail_h;
        }
        if (panel_h < 120) {
            panel_h = 120;   /* 键盘异常大时至少留出可操作的高度 */
        }
        lv_obj_set_height(panel, panel_h);
        lv_obj_set_style_pad_bottom(panel, 8, 0);
        lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, avail_top);
        /* 面板比原尺寸矮时内容会溢出，允许在面板内部纵向滚动，否则靠下的
         * 输入框会被裁掉且无法滚到。 */
        if (panel_h < CONFIG_MODAL_H) {
            lv_obj_set_scroll_dir(panel, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
        } else {
            lv_obj_set_scroll_dir(panel, LV_DIR_NONE);
            lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
        }
        lv_obj_update_layout(panel);
    }
    /* 面板变矮后靠下的输入框会落到滚动区外，把它滚进可见范围。 */
    lv_obj_get_coords(textarea, &input_area);
    lv_obj_get_coords(g_settings.keyboard, &keyboard_area);
    if (input_area.y2 + CONFIG_MODAL_GAP > keyboard_area.y1) {
        lv_obj_scroll_to_view(textarea, LV_ANIM_OFF);
        lv_obj_update_layout(panel);
    }
}

static void model_focus_cb(lv_event_t *event)
{
    lv_obj_t *textarea = lv_event_get_target(event);

    if (!g_settings.keyboard) {
        return;
    }
    lv_keyboard_set_textarea(g_settings.keyboard, textarea);
    lv_obj_align(g_settings.keyboard, LV_ALIGN_BOTTOM_MID, 0, -72);
    lv_obj_clear_flag(g_settings.keyboard, LV_OBJ_FLAG_HIDDEN);
    /* 键盘在构建期就挂在设置屏上，而服务配置弹窗的遮罩是运行期才建的，
     * 按子对象顺序永远排在键盘之后，会把键盘整个盖住——表现就是点输入框
     * 之后看不见键盘、也打不进字。聚焦时把键盘提到最前，弹窗只盖内容。 */
    lv_obj_move_foreground(g_settings.keyboard);
    config_modal_reveal_input(textarea);
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

    lv_obj_set_pos(label, x, y - 24);
    lv_obj_set_pos(textarea, x, y);
    lv_obj_set_size(textarea, width, 44);
    smart_home_lvgl_style_input(textarea, 17);
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
    char key[256] = "";
    const smart_home_model_config_t *active = ui && ui->app ?
        &ui->app->model_config : NULL;
    bool is_active;

    g_settings.model_index = index;
    if (!preset) {
        return;
    }
    is_active = active && strcmp(active->backend_id, preset->id) == 0;
    if (g_settings.model_name) {
        lv_textarea_set_text(g_settings.model_name,
                             g_settings.model_name_draft[index][0] ?
                             g_settings.model_name_draft[index] :
                             (is_active && active->model[0] ? active->model :
                              (preset->default_model ? preset->default_model : "")));
    }
    if (g_settings.model_key) {
        if (!g_settings.model_key_draft[index][0])
            (void)smart_home_secrets_get_model_api_key(preset->id, key, sizeof(key));
        lv_textarea_set_text(g_settings.model_key,
                             g_settings.model_key_draft[index][0] ?
                             g_settings.model_key_draft[index] : key);
        memset(key, 0, sizeof(key));
    }
    if (g_settings.model_timeout) {
        if (g_settings.model_timeout_draft[index][0]) {
            lv_textarea_set_text(g_settings.model_timeout,
                                 g_settings.model_timeout_draft[index]);
        } else {
            snprintf(buffer, sizeof(buffer), "%u",
                     is_active ? (unsigned)active->timeout_ms :
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
                     (unsigned)(ui->app->model_config.max_output_tokens ?
                         ui->app->model_config.max_output_tokens : 512));
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
#if defined(CONFIG_SMART_HOME_VOICE_ASR) || defined(CONFIG_SMART_HOME_VOICE_TTS)
    if (smart_home_lvgl_voice_settings_busy(ui)) {
        lv_label_set_text(g_settings.model_status, "请等待密钥或语音配置任务结束后重试");
        return;
    }
#endif
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
        g_model.persistent = strcmp(job->config.backend_id, "custom") != 0;
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

#if defined(CONFIG_SMART_HOME_VOICE_ASR) || defined(CONFIG_SMART_HOME_VOICE_TTS)
static void model_save_key_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    const smart_home_llm_backend_preset_t *preset;
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui || g_model.active) return;
    preset = smart_home_llm_backend_get(lv_dropdown_get_selected(g_settings.model_dd));
    if (!preset) return;
    smart_home_lvgl_save_llm_key(ui, preset->id,
        lv_textarea_get_text(g_settings.model_key), g_settings.model_status,
        g_settings.model_key);
}
#endif

static void build_model_panel(smart_home_lvgl_t *ui, lv_obj_t *panel)
{
    char options[256] = "";
    size_t count = smart_home_llm_backend_count();
    int initial = 0;

    panel_title(panel, "模型服务");
    g_settings.model_dd = lv_dropdown_create(panel);
    lv_obj_set_pos(g_settings.model_dd, 0, 68);
    lv_obj_set_size(g_settings.model_dd, 336, 44);
    smart_home_lvgl_style_dropdown(g_settings.model_dd, 17);
    {
        lv_obj_t *label = smart_home_lvgl_label_create(panel, "模型服务",
            SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
        lv_obj_set_pos(label, 0, 44);
        lv_obj_t *badge = lv_obj_create(panel);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, 154, 30);
        lv_obj_set_pos(badge, 550, -4);
        lv_obj_set_style_radius(badge, 15, 0);
        smart_home_lvgl_set_bg(badge, SMART_HOME_UI_COLOR_SURFACE_ON);
        label = smart_home_lvgl_label_create(badge, "手动输入密钥",
            SMART_HOME_UI_COLOR_TEXT_SECONDARY, 14);
        lv_obj_center(label);
    }
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

    g_settings.model_name = model_input(panel, "模型标识", 360, 68, 344,
                                        "模型名称");
    g_settings.model_key = model_input(panel, "API 密钥", 0, 146, 704,
                                       "留空沿用已保存密钥");
    lv_textarea_set_password_mode(g_settings.model_key, true);
    lv_textarea_set_max_length(g_settings.model_key,
                               sizeof(((smart_home_model_config_t *)0)->api_key) - 1u);
    g_settings.model_timeout = model_input(panel, "请求超时 / 毫秒", 0, 224,
                                           336, "30000");
    g_settings.model_tokens = model_input(panel, "最大输出 / 词元", 360, 224,
                                          344, "512");

    g_settings.model_status = smart_home_lvgl_label_create(
        panel, "", SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
    lv_obj_set_pos(g_settings.model_status, 0, 282);

    {
        lv_obj_t *button = lv_button_create(panel);
        lv_obj_t *label;
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, 168, 40);
        lv_obj_set_pos(button, 536, 312);
        lv_obj_set_ext_click_area(button, 4);
        lv_obj_set_style_radius(button, 10, 0);
        smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_PRIMARY_DARK);
        smart_home_lvgl_press_feedback(button);
        g_settings.model_apply = button;
        lv_obj_add_event_cb(button, model_apply_cb, LV_EVENT_CLICKED, ui);
        label = smart_home_lvgl_label_create(button, "保存并应用",
                                             SMART_HOME_UI_COLOR_SURFACE, 17);
        lv_obj_center(label);
    }

#if defined(CONFIG_SMART_HOME_VOICE_ASR) || defined(CONFIG_SMART_HOME_VOICE_TTS)
    {
        lv_obj_t *button = lv_button_create(panel);
        lv_obj_t *label;
        lv_obj_set_pos(button, 312, 312);
        lv_obj_set_size(button, 200, 40);
        lv_obj_add_event_cb(button, model_save_key_cb, LV_EVENT_CLICKED, ui);
        smart_home_lvgl_press_feedback(button);
        label = smart_home_lvgl_label_create(button, "保存 LLM 密钥",
                                             SMART_HOME_UI_COLOR_TEXT_PRIMARY, 16);
        lv_obj_center(label);
    }
#endif

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

static void services_stop_cb(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
}

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
static void miloco_config_open(smart_home_lvgl_t *ui);
#endif

static void config_modal_close(void)
{
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (g_settings.miloco_save_timer) {
        lv_timer_delete(g_settings.miloco_save_timer);
        g_settings.miloco_save_timer = NULL;
    }
#endif
    /* 弹窗里的输入框是遮罩的子对象，随遮罩一起销毁；键盘只存裸指针，
     * LVGL 不会在 textarea 删除时自动解绑。不在删除前先解绑，之后任何
     * 按键都会写已释放内存。 */
    if (g_settings.keyboard) {
        lv_keyboard_set_textarea(g_settings.keyboard, NULL);
        lv_obj_add_flag(g_settings.keyboard, LV_OBJ_FLAG_HIDDEN);
    }
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
    lv_obj_set_size(panel, CONFIG_MODAL_W, CONFIG_MODAL_H);
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

static void service_unavailable_open(const char *name, const char *option)
{
    lv_obj_t *panel = config_modal_begin(name);
    lv_obj_t *label = smart_home_lvgl_label_create(
        panel, "当前固件未启用此服务，暂时无法连接。",
        SMART_HOME_UI_COLOR_TEXT_PRIMARY, 18);

    lv_obj_set_pos(label, 0, 88);
    label = smart_home_lvgl_label_create(
        panel, option, SMART_HOME_UI_COLOR_TEXT_MUTED, 14);
    lv_obj_set_pos(label, 0, 128);
    lv_obj_set_width(label, 600);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
/* 连接米家家庭服务器（v9 33-mi-config）。保存后由 worker 落盘并立即轮询，
 * 本弹窗只负责采集地址/端口/令牌，扫码在下一步弹窗里完成。 */
static void miloco_config_open(smart_home_lvgl_t *ui);
static void miloco_save_poll_v9(lv_timer_t *timer)
{
    smart_home_lvgl_t *ui = lv_timer_get_user_data(timer);
    int ret = ui && ui->app ? smart_home_miloco_save_status(ui->app->miloco) :
                              AGENT_ERROR_INVALID;
    if (ret == AGENT_ERROR_BUSY) return;
    if (ret != AGENT_OK) {
        lv_timer_delete(g_settings.miloco_save_timer);
        g_settings.miloco_save_timer = NULL;
        lv_label_set_text(g_settings.miloco_modal_status, "保存失败，请重试");
        return;
    }
    config_modal_close();
    smart_home_lvgl_open_miloco_bind(ui);
}

static void miloco_save_cb_v9(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    smart_home_miloco_config_t config;
    const char *port_text;
    char *end;
    long port;
    int ret;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui ||
        !ui->app || !ui->app->miloco) {
        return;
    }
    memset(&config, 0, sizeof(config));
    snprintf(config.host, sizeof(config.host), "%s",
             lv_textarea_get_text(g_settings.miloco_host));
    snprintf(config.token, sizeof(config.token), "%s",
             lv_textarea_get_text(g_settings.miloco_token));
    port_text = lv_textarea_get_text(g_settings.miloco_port);
    port = strtol(port_text, &end, 10);
    if (end == port_text || *end != '\0' || port < 1 || port > 65535) {
        lv_label_set_text(g_settings.miloco_modal_status,
                          "端口须为 1～65535 的整数");
        return;
    }
    config.port = (uint16_t)port;
    if (!config.host[0] || !smart_home_miloco_config_valid(&config)) {
        lv_label_set_text(g_settings.miloco_modal_status, "请填写有效的服务器地址");
        memset(&config, 0, sizeof(config));
        return;
    }
    ret = smart_home_miloco_request_save(ui->app->miloco, &config);
    memset(&config, 0, sizeof(config));
    if (ret != AGENT_OK) {
        lv_label_set_text(g_settings.miloco_modal_status, "保存失败，请重试");
        return;
    }
    lv_label_set_text(g_settings.miloco_modal_status, "正在保存，请稍候…");
    if (!g_settings.miloco_save_timer)
        g_settings.miloco_save_timer = lv_timer_create(miloco_save_poll_v9, 100, ui);
    if (!g_settings.miloco_save_timer)
        lv_label_set_text(g_settings.miloco_modal_status, "保存已提交，请稍后重新打开查看");
}

static void miloco_bind_step_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);

    if (lv_event_get_code(event) == LV_EVENT_CLICKED && ui) {
        config_modal_close();
        smart_home_lvgl_open_miloco_bind(ui);
    }
}

static void miloco_config_open(smart_home_lvgl_t *ui)
{
    lv_obj_t *panel = config_modal_begin("连接米家网关");
    lv_obj_t *button;
    lv_obj_t *label;
    smart_home_miloco_config_t current;
    char port_text[8];

    memset(&current, 0, sizeof(current));
    current.port = SMART_HOME_MILOCO_DEFAULT_PORT;
    if (ui && ui->app && ui->app->miloco) {
        (void)smart_home_miloco_get_config(ui->app->miloco, &current);
    }
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)current.port);

    g_settings.miloco_host = model_input(panel, "家庭服务器地址", 0, 60, 480,
                                         "192.168.1.20");
    lv_textarea_set_text(g_settings.miloco_host, current.host);
    g_settings.miloco_port = model_input(panel, "端口", 500, 60, 120, "1810");
    lv_textarea_set_text(g_settings.miloco_port, port_text);
    g_settings.miloco_token = model_input(
        panel, "访问令牌（服务器未启用鉴权可留空）", 0, 140, 620, "token");
    lv_textarea_set_password_mode(g_settings.miloco_token, true);
    lv_textarea_set_text(g_settings.miloco_token, current.token);
    memset(&current, 0, sizeof(current));

    /* 状态行贴在按钮上方：面板被键盘压矮时若用绝对 y，会和底部按钮叠在一起。 */
    g_settings.miloco_modal_status = smart_home_lvgl_label_create(
#ifdef CONFIG_SMART_HOME_MILOCO_VOLATILE_CONFIG
        panel, "仅本次运行有效，重启后不保留",
#else
        panel, "保存完成后重启保留",
#endif
        SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
    lv_obj_align(g_settings.miloco_modal_status, LV_ALIGN_BOTTOM_LEFT, 0, -76);

    /* 次：仅扫码（地址已配置过时跳过填写）。
     * 底部对齐而非绝对 y：键盘弹出时面板会被压矮（config_modal_reveal_input），
     * 绝对坐标的按钮会跟着落到面板外，看不见也点不到。 */
    button = lv_button_create(panel);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 168, 40);
    lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, 0, -24);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, SMART_HOME_UI_COLOR_BORDER, 0);
    smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_SURFACE);
    smart_home_lvgl_press_feedback(button);
    lv_obj_add_event_cb(button, miloco_bind_step_cb, LV_EVENT_CLICKED, ui);
    label = smart_home_lvgl_label_create(button, "直接扫码绑定",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 14);
    lv_obj_center(label);

    /* 主：保存并连接。对齐方式同上。 */
    button = lv_button_create(panel);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 200, 40);
    lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, 186, -24);
    lv_obj_set_style_radius(button, 10, 0);
    smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_PRIMARY_DARK);
    smart_home_lvgl_press_feedback(button);
    lv_obj_add_event_cb(button, miloco_save_cb_v9, LV_EVENT_CLICKED, ui);
    label = smart_home_lvgl_label_create(button, "保存并扫码授权",
                                         SMART_HOME_UI_COLOR_SURFACE, 14);
    lv_obj_center(label);
}
#endif /* CONFIG_SMART_HOME_MILOCO_BRIDGE */
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
    lv_obj_set_size(g_settings.mcp_protocol, 200, 44);
    smart_home_lvgl_style_dropdown(g_settings.mcp_protocol, 17);
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
    lv_obj_set_pos(button, 0, 336);
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

static void service_action_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    lv_obj_t *button = lv_event_get_current_target(event);
    int index = (int)(intptr_t)lv_obj_get_user_data(button);

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui) {
        return;
    }

    switch (index) {
    case 0:
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
        miloco_config_open(ui);
#else
        service_unavailable_open(
            "米家网关",
            "启用 CONFIG_SMART_HOME_MILOCO_BRIDGE 并重新构建固件后，可在此配置连接。");
#endif
        break;
    case 1:
#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
        mcp_config_open(ui);
#else
        service_unavailable_open(
            "MCP 服务",
            "启用 CONFIG_SMART_HOME_MCP_BRIDGE 并重新构建固件后，可在此配置连接。");
#endif
        break;
    case 2:
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
        node_config_open(ui);
#else
        service_unavailable_open(
            "局域网 Node",
            "启用 CONFIG_SMART_HOME_NODE_GATEWAY 并重新构建固件后，可在此配置网关。");
#endif
        break;
    default:
        break;
    }
}

static void service_row(lv_obj_t *panel, smart_home_lvgl_t *ui,
                        int index, int y, const char *icon_name,
                        const char *name, const char *description,
                        const char *status, const char *action)
{
    lv_obj_t *icon = smart_home_lvgl_icon_create(panel, icon_name, 32, 32);
    lv_obj_t *label;
    lv_obj_t *pill;
    lv_obj_t *button;
    lv_obj_t *line;

    if (icon) {
        lv_obj_set_pos(icon, 0, y + 5);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    }

    label = smart_home_lvgl_label_create(
        panel, name, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 18);
    lv_obj_set_pos(label, 46, y);

    label = smart_home_lvgl_label_create(
        panel, description, SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
    lv_obj_set_pos(label, 46, y + 27);
    lv_obj_set_width(label, 330);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    pill = lv_obj_create(panel);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, 174, 30);
    lv_obj_set_pos(pill, 367, y + 4);
    lv_obj_set_style_radius(pill, 15, 0);
    lv_obj_set_style_bg_color(pill, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    label = smart_home_lvgl_label_create(
        pill, status, SMART_HOME_UI_COLOR_TEXT_SECONDARY, 14);
    lv_obj_center(label);

    button = lv_button_create(panel);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 142, 48);
    lv_obj_set_pos(button, 563, y - 1);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, SMART_HOME_UI_COLOR_BORDER, 0);
    lv_obj_set_user_data(button, (void *)(intptr_t)index);
    smart_home_lvgl_press_feedback(button);
    lv_obj_add_event_cb(button, service_action_cb, LV_EVENT_CLICKED, ui);
    label = smart_home_lvgl_label_create(
        button, action, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 16);
    lv_obj_center(label);

    line = lv_obj_create(panel);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, lv_pct(100), 1);
    lv_obj_set_pos(line, 0, y + 63);
    lv_obj_set_style_bg_color(line, SMART_HOME_UI_COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
}

static void build_services_panel(smart_home_lvgl_t *ui, lv_obj_t *panel)
{
    const char *miloco = "固件未启用";
    const char *mcp = "固件未启用";
    const char *node = "固件未启用";

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

    panel_title(panel, "设备与服务");
    service_row(panel, ui, 0, 52, "asset:devices", "米家网关",
                "连接家庭服务器，再扫码授权账号", miloco, "连接 / 状态");
    service_row(panel, ui, 1, 130, "asset:cloud-cog", "MCP 服务",
                "配置服务地址与凭据，连接并发现工具", mcp, "连接 / 状态");
    service_row(panel, ui, 2, 208, "asset:link", "局域网 Node",
                "开启本机网关，等待节点主动接入", node, "开启 / 状态");

    {
        lv_obj_t *note = lv_obj_create(panel);
        lv_obj_t *label;

        lv_obj_remove_style_all(note);
        lv_obj_set_size(note, lv_pct(100), 40);
        lv_obj_set_pos(note, 0, 305);
        lv_obj_set_style_radius(note, 10, 0);
        lv_obj_set_style_bg_color(note, SMART_HOME_UI_COLOR_SURFACE_ON, 0);
        lv_obj_set_style_bg_opa(note, LV_OPA_COVER, 0);
        label = smart_home_lvgl_label_create(
            note, "配置是否重启保留，以连接弹窗中的说明为准。",
            SMART_HOME_UI_COLOR_TEXT_SECONDARY, 13);
        lv_obj_set_pos(label, 14, 10);
        lv_obj_set_width(label, 670);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    }
}

static void health_service_row(lv_obj_t *panel, int y,
                               const char *icon_name, const char *name,
                               lv_obj_t **value)
{
    lv_obj_t *icon = smart_home_lvgl_icon_create(panel, icon_name, 18, 18);
    lv_obj_t *label = smart_home_lvgl_label_create(
        panel, name, SMART_HOME_UI_COLOR_TEXT_SECONDARY, 15);
    lv_obj_t *line = lv_obj_create(panel);

    if (icon) {
        lv_obj_set_pos(icon, 0, y + 10);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_pos(label, 32, y + 7);
    *value = smart_home_lvgl_label_create(
        panel, "未检查", SMART_HOME_UI_COLOR_TEXT_SECONDARY, 15);
    lv_obj_set_size(*value, 304, 24);
    lv_obj_set_pos(*value, 400, y + 7);
    lv_obj_set_style_text_align(*value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(*value, LV_LABEL_LONG_DOT);

    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, lv_pct(100), 1);
    lv_obj_set_pos(line, 0, y + 40);
    lv_obj_set_style_bg_color(line, SMART_HOME_UI_COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
}

static const char *health_build_version(void)
{
#ifdef AGENT_BUILD_VERSION
    return AGENT_BUILD_VERSION;
#elif defined(CONFIG_VERSION_STRING)
    return CONFIG_VERSION_STRING;
#else
    return "版本未知";
#endif
}

static void health_meta_refresh(void)
{
    struct tm checked_tm;
    char checked_text[16] = "--:--:--";
    char meta[112];

    if (g_settings.health_checked_at > 0 &&
        gmtime_r(&g_settings.health_checked_at, &checked_tm)) {
        (void)strftime(checked_text, sizeof(checked_text), "%H:%M:%S",
                       &checked_tm);
    }

    snprintf(meta, sizeof(meta), "最后检查 %s · 固件 %s", checked_text,
             health_build_version());
    if (g_settings.health_meta) {
        lv_label_set_text(g_settings.health_meta, meta);
    }
}

static void health_panel_refresh(smart_home_lvgl_t *ui)
{
    const smart_home_system_status_t *status;
    const smart_home_network_status_t *network;
    lv_color_t summary_color;
    int failures = 0;
    int pending = 0;

    if (!ui || !ui->app || !g_settings.health_badge_label) return;
    status = &ui->app->system_status;
    network = &status->network_status;

    if (network->online) {
        lv_label_set_text(g_settings.health_network, "已连接互联网");
        lv_obj_set_style_text_color(g_settings.health_network,
                                    SMART_HOME_UI_COLOR_SUCCESS, 0);
    } else if (network->ip_status == SMART_HOME_NETWORK_OK) {
        lv_label_set_text(g_settings.health_network,
                          "Wi-Fi 已连接，互联网暂不可用");
        lv_obj_set_style_text_color(g_settings.health_network,
                                    SMART_HOME_UI_COLOR_WARNING, 0);
        failures++;
    } else {
        lv_label_set_text(g_settings.health_network, "未连接互联网");
        lv_obj_set_style_text_color(g_settings.health_network,
                                    SMART_HOME_UI_COLOR_DANGER, 0);
        failures++;
    }

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (ui->app->miloco) {
        smart_home_miloco_config_t config = {0};
        int configured = smart_home_miloco_get_config(ui->app->miloco,
                                                       &config) &&
                         config.host[0] != '\0';
        int reachable = smart_home_miloco_reachable(ui->app->miloco);

        if (reachable && smart_home_miloco_bound(ui->app->miloco)) {
            size_t count = smart_home_miloco_list(ui->app->miloco, NULL, 0,
                                                   NULL);
            char value[48];

            snprintf(value, sizeof(value), "已连接 · %u 台设备",
                     (unsigned int)count);
            lv_label_set_text(g_settings.health_miloco, value);
            lv_obj_set_style_text_color(g_settings.health_miloco,
                                        SMART_HOME_UI_COLOR_SUCCESS, 0);
        } else if (reachable) {
            lv_label_set_text(g_settings.health_miloco, "已连接 · 等待授权");
            lv_obj_set_style_text_color(g_settings.health_miloco,
                                        SMART_HOME_UI_COLOR_WARNING, 0);
        } else if (configured) {
            lv_label_set_text(g_settings.health_miloco, "连接失败");
            lv_obj_set_style_text_color(g_settings.health_miloco,
                                        SMART_HOME_UI_COLOR_DANGER, 0);
            failures++;
        } else {
            lv_label_set_text(g_settings.health_miloco, "未配置");
            lv_obj_set_style_text_color(g_settings.health_miloco,
                                        SMART_HOME_UI_COLOR_TEXT_MUTED, 0);
        }
    } else {
        lv_label_set_text(g_settings.health_miloco, "未启用");
        lv_obj_set_style_text_color(g_settings.health_miloco,
                                    SMART_HOME_UI_COLOR_TEXT_MUTED, 0);
    }
#else
    lv_label_set_text(g_settings.health_miloco, "未启用");
    lv_obj_set_style_text_color(g_settings.health_miloco,
                                SMART_HOME_UI_COLOR_TEXT_MUTED, 0);
#endif

    if (status->model_status == SMART_HOME_STATUS_UNKNOWN) {
        lv_label_set_text(g_settings.health_model, "等待检查");
        lv_obj_set_style_text_color(g_settings.health_model,
                                    SMART_HOME_UI_COLOR_TEXT_MUTED, 0);
        pending++;
    } else if (status->model_status == AGENT_OK) {
        lv_label_set_text(g_settings.health_model, "配置已应用");
        lv_obj_set_style_text_color(g_settings.health_model,
                                    SMART_HOME_UI_COLOR_SUCCESS, 0);
    } else {
        lv_label_set_text(g_settings.health_model, "配置未应用");
        lv_obj_set_style_text_color(g_settings.health_model,
                                    SMART_HOME_UI_COLOR_DANGER, 0);
        failures++;
    }

    if (status->tools_status == SMART_HOME_STATUS_UNKNOWN) {
        lv_label_set_text(g_settings.health_tools, "等待检查");
        lv_obj_set_style_text_color(g_settings.health_tools,
                                    SMART_HOME_UI_COLOR_TEXT_MUTED, 0);
        pending++;
    } else if (status->tools_status == AGENT_OK) {
        lv_label_set_text(g_settings.health_tools, "已就绪");
        lv_obj_set_style_text_color(g_settings.health_tools,
                                    SMART_HOME_UI_COLOR_SUCCESS, 0);
    } else {
        lv_label_set_text(g_settings.health_tools, "不可用");
        lv_obj_set_style_text_color(g_settings.health_tools,
                                    SMART_HOME_UI_COLOR_DANGER, 0);
        failures++;
    }

    if (g_settings.health_error) {
        const char *error = status->last_error[0] ? status->last_error : "无";

        lv_label_set_text(g_settings.health_error, error);
        lv_obj_set_style_text_color(g_settings.health_error,
            status->last_error[0] ? SMART_HOME_UI_COLOR_WARNING :
                                    SMART_HOME_UI_COLOR_TEXT_MUTED, 0);
    }

    if (failures) {
        lv_label_set_text(g_settings.health_badge_label, "有服务需要处理");
        lv_label_set_text(lv_obj_get_child(g_settings.health_note, 0),
                          "网络或核心服务异常；未启用的可选服务单独标记。");
        summary_color = SMART_HOME_UI_COLOR_DANGER;
    } else if (pending) {
        lv_label_set_text(g_settings.health_badge_label, "状态待确认");
        lv_label_set_text(lv_obj_get_child(g_settings.health_note, 0),
                          "部分服务状态未知，点击刷新状态获取最新信息。");
        summary_color = SMART_HOME_UI_COLOR_WARNING;
    } else {
        lv_label_set_text(g_settings.health_badge_label, "核心服务正常");
        lv_label_set_text(lv_obj_get_child(g_settings.health_note, 0),
                          "网络与核心服务正常，未启用的可选服务单独标记。");
        summary_color = SMART_HOME_UI_COLOR_SUCCESS;
    }
    lv_obj_set_style_text_color(g_settings.health_badge_label,
                                SMART_HOME_UI_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_bg_color(g_settings.health_badge, summary_color, 0);
    lv_obj_set_style_bg_color(lv_obj_get_child(g_settings.health_badge, 0),
                              summary_color, 0);
    lv_obj_set_style_bg_color(g_settings.health_note, summary_color, 0);
    health_meta_refresh();
}

static void health_details_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        panel_show(V9_CAT_SERVICES);
    }
}

static void health_refresh_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    v9_net_job_t *job;
    pthread_attr_t attr;
    int ret;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui || !ui->app) {
        return;
    }
    if (g_net.active) {
        if (g_settings.health_note) {
            lv_label_set_text(lv_obj_get_child(g_settings.health_note, 0),
                              "网络任务正在进行，完成后可再次刷新。");
        }
        return;
    }
    job = calloc(1u, sizeof(*job));
    if (!job) {
        if (g_settings.health_badge_label)
            lv_label_set_text(g_settings.health_badge_label, "无法开始检查");
        return;
    }
    job->ui = ui;
    job->probe = 1;
    job->status = ui->app->system_status.network_status;
    g_net.owner = ui;
    g_net.result = 0;
    g_net.ready = 0;
    g_net.active = 1;
    lv_obj_add_state(g_settings.health_refresh, LV_STATE_DISABLED);
    lv_label_set_text(g_settings.health_refresh_label, "检查中…");
    lv_label_set_text(g_settings.health_badge_label, "正在检查");
    lv_label_set_text(lv_obj_get_child(g_settings.health_note, 0),
                      "正在检测网络，其余服务显示当前状态。");
    if (pthread_attr_init(&attr) != 0) {
        free(job);
        g_net.active = 0;
        lv_obj_remove_state(g_settings.health_refresh, LV_STATE_DISABLED);
        lv_label_set_text(g_settings.health_refresh_label, "刷新状态");
        health_panel_refresh(ui);
        return;
    }
    ret = pthread_attr_setstacksize(&attr, 16384u);
    if (ret == 0) ret = pthread_create(&g_net.thread, &attr, v9_net_worker,
                                       job);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        free(job);
        g_net.active = 0;
        lv_obj_remove_state(g_settings.health_refresh, LV_STATE_DISABLED);
        lv_label_set_text(g_settings.health_refresh_label, "刷新状态");
        lv_label_set_text(g_settings.health_badge_label, "无法开始检查");
        lv_label_set_text(lv_obj_get_child(g_settings.health_note, 0),
                          "无法创建网络检查任务，请稍后重试。");
        return;
    }
}

static void health_action_button(lv_obj_t *panel, int x, const char *text,
                                 lv_event_cb_t callback, void *user_data,
                                 int primary, lv_obj_t **button_out,
                                 lv_obj_t **label_out)
{
    lv_obj_t *button = lv_button_create(panel);
    lv_obj_t *label;

    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, primary ? 168 : 176, 32);
    lv_obj_set_pos(button, x, 342);
    lv_obj_set_style_radius(button, 10, 0);
    if (primary) {
        lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_color(button, SMART_HOME_UI_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, SMART_HOME_UI_COLOR_BORDER, 0);
    }
    smart_home_lvgl_press_feedback(button);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    label = smart_home_lvgl_label_create(
        button, text, primary ? SMART_HOME_UI_COLOR_BG :
                               SMART_HOME_UI_COLOR_TEXT_PRIMARY, 15);
    lv_obj_center(label);
    if (button_out) *button_out = button;
    if (label_out) *label_out = label;
}

static void build_health_panel(smart_home_lvgl_t *ui, lv_obj_t *panel)
{
    lv_obj_t *title;
    lv_obj_t *badge_dot;
    lv_obj_t *note_label;
    lv_obj_t *error_title;

    title = smart_home_lvgl_label_create(
        panel, "系统健康", SMART_HOME_UI_COLOR_TEXT_PRIMARY, 22);
    lv_obj_set_pos(title, 0, 20);

    g_settings.health_badge = lv_obj_create(panel);
    lv_obj_remove_style_all(g_settings.health_badge);
    lv_obj_set_size(g_settings.health_badge, 164, 30);
    lv_obj_set_pos(g_settings.health_badge, 540, 18);
    lv_obj_set_style_radius(g_settings.health_badge, 15, 0);
    lv_obj_set_style_bg_color(g_settings.health_badge,
                              SMART_HOME_UI_COLOR_SUCCESS, 0);
    lv_obj_set_style_bg_opa(g_settings.health_badge, LV_OPA_20, 0);
    badge_dot = lv_obj_create(g_settings.health_badge);
    lv_obj_remove_style_all(badge_dot);
    lv_obj_set_size(badge_dot, 7, 7);
    lv_obj_set_pos(badge_dot, 13, 11);
    lv_obj_set_style_radius(badge_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge_dot, SMART_HOME_UI_COLOR_SUCCESS, 0);
    lv_obj_set_style_bg_opa(badge_dot, LV_OPA_COVER, 0);
    g_settings.health_badge_label = smart_home_lvgl_label_create(
        g_settings.health_badge, "核心服务正常",
        SMART_HOME_UI_COLOR_TEXT_PRIMARY, 14);
    lv_obj_set_pos(g_settings.health_badge_label, 27, 6);

    g_settings.health_note = lv_obj_create(panel);
    lv_obj_remove_style_all(g_settings.health_note);
    lv_obj_set_size(g_settings.health_note, lv_pct(100), 38);
    lv_obj_set_pos(g_settings.health_note, 0, 66);
    lv_obj_set_style_radius(g_settings.health_note, 10, 0);
    lv_obj_set_style_bg_color(g_settings.health_note,
                              SMART_HOME_UI_COLOR_SUCCESS, 0);
    lv_obj_set_style_bg_opa(g_settings.health_note, LV_OPA_20, 0);
    note_label = smart_home_lvgl_label_create(
        g_settings.health_note,
        "网络与核心服务正常，未启用的可选服务单独标记。",
        SMART_HOME_UI_COLOR_TEXT_SECONDARY, 13);
    lv_obj_set_pos(note_label, 14, 11);
    lv_obj_set_width(note_label, 670);
    lv_label_set_long_mode(note_label, LV_LABEL_LONG_DOT);

    health_service_row(panel, 116, "asset:wifi", "家庭网络",
                       &g_settings.health_network);
    health_service_row(panel, 158, "asset:devices", "米家网关",
                       &g_settings.health_miloco);
    health_service_row(panel, 200, "asset:setting-ai", "模型服务",
                       &g_settings.health_model);
    health_service_row(panel, 242, "asset:tool", "本地工具",
                       &g_settings.health_tools);

    error_title = smart_home_lvgl_label_create(
        panel, "最近错误：", SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
    lv_obj_set_pos(error_title, 0, 292);
    g_settings.health_error = smart_home_lvgl_label_create(
        panel, "无", SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
    lv_obj_set_pos(g_settings.health_error, 78, 292);
    lv_obj_set_width(g_settings.health_error, 626);
    lv_label_set_long_mode(g_settings.health_error, LV_LABEL_LONG_DOT);

    g_settings.health_meta = smart_home_lvgl_label_create(
        panel, "", SMART_HOME_UI_COLOR_TEXT_MUTED, 12);
    lv_obj_set_pos(g_settings.health_meta, 0, 316);
    lv_obj_set_width(g_settings.health_meta, lv_pct(100));
    lv_label_set_long_mode(g_settings.health_meta, LV_LABEL_LONG_DOT);

    health_action_button(panel, 0, "服务详情", health_details_cb, ui, 0,
                         NULL, NULL);
    health_action_button(panel, 536, "刷新状态", health_refresh_cb, ui, 1,
                         &g_settings.health_refresh,
                         &g_settings.health_refresh_label);
    health_panel_refresh(ui);
}

void smart_home_lvgl_build_settings_screen_v9(smart_home_lvgl_t *ui)
{
    lv_obj_t *screen;
    lv_obj_t *title;
    lv_obj_t *left;
    lv_obj_t *right;

    if (!ui) {
        return;
    }
    memset(&g_settings, 0, sizeof(g_settings));

    screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    smart_home_lvgl_set_bg(screen, SMART_HOME_UI_COLOR_BG);
    lv_obj_set_style_text_font(screen, smart_home_lvgl_font(16), 0);
    ui->screen_settings_v9 = screen;
    g_settings.screen = screen;
    smart_home_lvgl_build_top_bar(screen, ui, "设置");

    title = smart_home_lvgl_label_create(screen, "设置",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 24);
    lv_obj_set_pos(title, 24, 76);

    left = lv_obj_create(screen);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, 208, 408);
    lv_obj_set_pos(left, 24, 112);
    smart_home_lvgl_card_style(left);
    lv_obj_set_style_radius(left, 18, 0);
    lv_obj_set_style_pad_all(left, 0, 0);

    right = lv_obj_create(screen);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, 752, 408);
    lv_obj_set_pos(right, 248, 112);
    smart_home_lvgl_card_style(right);
    lv_obj_set_style_radius(right, 18, 0);
    lv_obj_set_style_pad_all(right, 23, 0);
    lv_obj_set_style_pad_bottom(right, 7, 0);

    for (int i = 0; i < V9_SETTINGS_CATEGORIES; i++) {
        lv_obj_t *button = lv_button_create(left);
        lv_obj_t *label;

        lv_obj_remove_style_all(button);
        /* 7 个分类要装进 408 高的左栏：44 + 50 的间距不能在 11+7*54 处溢出。 */
        lv_obj_set_size(button, 184, 46);
        lv_obj_set_pos(button, 11, 8 + i * 51);
        lv_obj_set_style_radius(button, 12, 0);
        lv_obj_set_user_data(button, (void *)(intptr_t)i);
        lv_obj_add_event_cb(button, category_cb, LV_EVENT_CLICKED, NULL);
        smart_home_lvgl_press_feedback(button);
        g_settings.button[i] = button;

        lv_obj_t *icon = smart_home_lvgl_icon_create(button,
                                                     g_category_icons[i], 20, 20);
        if (icon) {
            lv_obj_set_pos(icon, 12, 13);
            lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        }
        label = smart_home_lvgl_label_create(button, g_category_names[i],
                                             SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                             16);
        lv_obj_set_pos(label, 46, 12);
    }

    g_settings.panel[0] = make_panel(right);
    g_settings.panel[1] = make_panel(right);
    g_settings.panel[2] = make_panel(right);
    g_settings.panel[3] = make_panel(right);
    g_settings.panel[4] = make_panel(right);
    g_settings.panel[5] = make_panel(right);
    g_settings.panel[6] = make_panel(right);

    build_display_panel(g_settings.panel[0], ui->screen_settings);
    build_network_panel(ui, g_settings.panel[1]);
    build_model_panel(ui, g_settings.panel[2]);
    build_tools_panel(ui, g_settings.panel[4]);
    build_services_panel(ui, g_settings.panel[5]);
    build_health_panel(ui, g_settings.panel[6]);

    g_settings.keyboard = lv_keyboard_create(screen);
    smart_home_lvgl_style_keyboard(g_settings.keyboard);
    /* 宽度与聊天页等其他页面统一（smart_home_lvgl_content_w()），高度保持
     * 180 不变；底部 -72 偏移是该页既有布局（键盘浮在设置面板下方），
     * 本次只对齐宽度，不动位置。 */
    lv_obj_set_size(g_settings.keyboard,
                    smart_home_lvgl_content_w(),
                    smart_home_lvgl_keyboard_h());
    lv_obj_align(g_settings.keyboard, LV_ALIGN_BOTTOM_MID, 0, -72);
    lv_obj_add_event_cb(g_settings.keyboard, model_keyboard_cb, LV_EVENT_ALL,
                        NULL);
    lv_obj_add_flag(g_settings.keyboard, LV_OBJ_FLAG_HIDDEN);

#if defined(CONFIG_SMART_HOME_VOICE_ASR) || defined(CONFIG_SMART_HOME_VOICE_TTS)
    smart_home_lvgl_build_voice_settings(ui, g_settings.panel[V9_CAT_VOICE],
                                         g_settings.keyboard);
#else
    panel_title(g_settings.panel[V9_CAT_VOICE], "语音服务");
    lv_obj_t *voice_hint = smart_home_lvgl_label_create(
        g_settings.panel[V9_CAT_VOICE], "语音服务未开启",
        SMART_HOME_UI_COLOR_TEXT_PRIMARY, 22);
    lv_obj_align(voice_hint, LV_ALIGN_TOP_MID, 0, 136);
    voice_hint = smart_home_lvgl_label_create(g_settings.panel[V9_CAT_VOICE],
        "当前固件未启用语音输入或语音播报。\n启用语音服务后，即可在这里配置 ASR 和 TTS。",
        SMART_HOME_UI_COLOR_TEXT_SECONDARY, 16);
    lv_obj_set_width(voice_hint, 640);
    lv_obj_set_style_text_align(voice_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(voice_hint, LV_ALIGN_TOP_MID, 0, 180);
    lv_obj_clear_flag(g_settings.panel[V9_CAT_VOICE], LV_OBJ_FLAG_SCROLLABLE);
#endif

    if (ui->v9_network_timer) {
        lv_timer_delete(ui->v9_network_timer);
    }
    ui->v9_network_timer = lv_timer_create(v9_net_timer_cb, 250, ui);

    panel_show(V9_CAT_DISPLAY);
    smart_home_lvgl_build_nav_bar(screen, ui);
    smart_home_lvgl_update_clock(ui);
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
