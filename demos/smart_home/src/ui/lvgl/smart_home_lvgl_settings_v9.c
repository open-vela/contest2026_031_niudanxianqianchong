/** V9 Settings shell: seven internal categories, summary panels and routes. */

#include "smart_home_lvgl_internal.h"
#include "images/smart_home_icons.h"
#include "../../config/smart_home_backends.h"
#include "../../config/smart_home_secrets.h"
#include "../../net/smart_home_network.h"
#include "../../smart_home_memory.h"
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
    char name[AGENT_MAX_NAME_BYTES + 1u];
    char description[AGENT_MAX_DESCRIPTION_BYTES + 1u];
    uint16_t group;
    int enabled;
    int allowed;
} v9_tool_row_t;

#define V9_TOOLS_PER_PAGE 3u
#define V9_TOOLS_STEP_MS 40u

typedef struct {
    lv_obj_t *row;
    lv_obj_t *name;
    lv_obj_t *meta;
    lv_obj_t *toggle;
    v9_tool_row_t tool;
    int valid;
} v9_tool_slot_t;

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
    lv_obj_t *mcp_service_status;
    lv_obj_t *mcp_apply;
    lv_obj_t *mcp_discover;
    smart_home_mcp_state_t mcp_observed_state;
    int mcp_state_valid;
#endif
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
    lv_obj_t *node_port;
    lv_obj_t *node_status;
    lv_obj_t *node_service_status;
    lv_obj_t *nodes_body;
    uint32_t nodes_revision;
    const smart_home_node_gateway_t *nodes_gateway;
    size_t nodes_count;
    int nodes_valid;
    uint16_t node_last_port;
#endif
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    lv_obj_t *miloco_host;
    lv_obj_t *miloco_port;
    lv_obj_t *miloco_token;
    lv_obj_t *miloco_modal_status;
    lv_obj_t *miloco_service_status;
    lv_timer_t *miloco_save_timer;
#endif
    smart_home_lvgl_t *tools_owner;
    lv_obj_t *tools_body;
    lv_obj_t *tools_status;
    v9_tool_row_t *tools;
    v9_tool_row_t *tools_scratch;
    size_t tool_count;
    size_t tools_page;
    size_t tools_cursor;
    int tools_valid;
    int tools_requested;
    int tools_rendering;
    uint32_t tools_snapshot_ms;
    uint32_t tools_row_max_ms;
    lv_timer_t *tools_timer;
    lv_obj_t *tools_refresh;
    lv_obj_t *tools_prev;
    lv_obj_t *tools_next;
    lv_obj_t *tools_page_label;
    v9_tool_slot_t tool_slots[V9_TOOLS_PER_PAGE];
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
static void tools_refresh(smart_home_lvgl_t *ui);
static lv_obj_t *config_modal_begin(const char *title);
static void services_refresh(smart_home_lvgl_t *ui);
#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
static void mcp_poll(smart_home_lvgl_t *ui);
static struct {
    pthread_t thread;
    smart_home_lvgl_t *owner;
    int active, ready, result;
    smart_home_mcp_runtime_config_t config;
} g_mcp;
#endif

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
static void miloco_service_status_refresh(smart_home_lvgl_t *ui)
{
    const char *status = "未连接";

    if (!g_settings.miloco_service_status) return;
    if (ui && ui->app && ui->app->miloco &&
        smart_home_miloco_reachable(ui->app->miloco)) {
        status = smart_home_miloco_bound(ui->app->miloco) ? "已绑定" : "待绑定";
    }
    if (strcmp(lv_label_get_text(g_settings.miloco_service_status), status))
        lv_label_set_text(g_settings.miloco_service_status, status);
}
#endif

#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
static void mcp_config_open(smart_home_lvgl_t *ui);
#endif
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
static void node_config_open(smart_home_lvgl_t *ui);
static void nodes_refresh(smart_home_lvgl_t *ui);
#endif

static void panel_show(int category)
{
    if (category < 0 || category >= V9_SETTINGS_CATEGORIES) {
        return;
    }
    g_settings.category = category;
    if (category != V9_CAT_TOOLS && g_settings.tools_timer)
        lv_timer_pause(g_settings.tools_timer);
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
    if (category == V9_CAT_TOOLS) tools_refresh(g_settings.tools_owner);
}

static void category_cb(lv_event_t *event)
{
    lv_obj_t *button = lv_event_get_current_target(event);
    int index = (int)(intptr_t)lv_obj_get_user_data(button);

    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        panel_show(index);
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
        if (index == V9_CAT_SERVICES) nodes_refresh(lv_event_get_user_data(event));
#endif
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

static void build_display_panel(lv_obj_t *panel)
{
    lv_obj_t *label;
    lv_obj_t *sw;
    const char *choices[] = {"1分钟", "5分钟", "永不"};

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
#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
    if (g_mcp.owner == ui) {
        if (g_mcp.active) pthread_join(g_mcp.thread, NULL);
        memset(&g_mcp, 0, sizeof(g_mcp));
    }
#endif
#ifdef CONFIG_SMART_HOME_KWS
    smart_home_lvgl_wake_settings_deinit(ui);
#endif
    if (g_settings.tools_timer) lv_timer_delete(g_settings.tools_timer);
    smart_home_bulk_free(g_settings.tools);
    smart_home_bulk_free(g_settings.tools_scratch);
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
#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
    mcp_poll(ui);
#endif
#ifdef CONFIG_SMART_HOME_KWS
    smart_home_lvgl_wake_settings_poll(ui);
#endif
    services_refresh(ui);
    if (g_settings.tools_timer && g_settings.category == V9_CAT_TOOLS &&
        lv_screen_active() == g_settings.screen &&
        (g_settings.tools_requested || g_settings.tools_rendering))
        lv_timer_resume(g_settings.tools_timer);
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
    if (g_settings.category == V9_CAT_SERVICES && lv_screen_active() == g_settings.screen)
        nodes_refresh(ui);
#endif
    v9_net_status_refresh(ui);
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    miloco_service_status_refresh(ui);
#endif
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

static void tools_set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

static const char *tool_permission(const v9_tool_row_t *tool)
{
    if (!tool->enabled) return "不可用";
    if (tool->group != SMART_HOME_TOOL_GROUP_LOCAL) return "接入策略管理";
    return tool->allowed ? "已允许" : "已关闭";
}

static void tool_slot_state(v9_tool_slot_t *slot)
{
    char text[96];
    const v9_tool_row_t *tool = &slot->tool;
    snprintf(text, sizeof(text), "%s · %s · 查看详情",
        smart_home_tool_source_label(smart_home_tool_source(tool->group, tool->name)),
        tool_permission(tool));
    tools_set_text(slot->meta, text);
    if (tool->enabled && tool->allowed) lv_obj_add_state(slot->toggle, LV_STATE_CHECKED);
    else lv_obj_remove_state(slot->toggle, LV_STATE_CHECKED);
    if (!tool->enabled || tool->group != SMART_HOME_TOOL_GROUP_LOCAL)
        lv_obj_add_state(slot->toggle, LV_STATE_DISABLED);
    else lv_obj_remove_state(slot->toggle, LV_STATE_DISABLED);
}

static void tool_switch_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    lv_obj_t *sw = lv_event_get_current_target(event);
    v9_tool_slot_t *slot = lv_obj_get_user_data(sw);
    int allowed, ret;

    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || !ui || !ui->app ||
        !slot || !slot->valid) return;
    lv_event_stop_bubbling(event);
    if (slot->tool.group != SMART_HOME_TOOL_GROUP_LOCAL || !slot->tool.enabled) {
        tool_slot_state(slot);
        return;
    }
    allowed = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ret = smart_home_agent_app_try_set_local_tool_access(ui->app, slot->tool.name, allowed);
    if (ret == AGENT_OK) {
        slot->tool.allowed = allowed;
        for (size_t i = 0; i < g_settings.tool_count; ++i)
            if (!strcmp(g_settings.tools[i].name, slot->tool.name))
                g_settings.tools[i].allowed = allowed;
    }
    tool_slot_state(slot);
    tools_set_text(g_settings.tools_status,
        ret == AGENT_OK ? "权限已更新 · 本次运行有效" :
        ret == AGENT_ERROR_BUSY ? "工具正在使用中，请稍后重试" : "权限更新失败");
}

static void tool_detail_cb(lv_event_t *event)
{
    v9_tool_slot_t *slot = lv_event_get_user_data(event);
    lv_obj_t *panel, *label, *body;
    char text[128];
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !slot || !slot->valid) return;
    panel = config_modal_begin("工具详情");
    label = smart_home_lvgl_label_create(panel, slot->tool.name,
        SMART_HOME_UI_COLOR_TEXT_PRIMARY, 17);
    lv_obj_set_pos(label, 0, 48);
    lv_obj_set_width(label, 632);
    snprintf(text, sizeof(text), "%s · %s",
        smart_home_tool_source_label(smart_home_tool_source(slot->tool.group, slot->tool.name)),
        tool_permission(&slot->tool));
    label = smart_home_lvgl_label_create(panel, text, SMART_HOME_UI_COLOR_TEXT_SECONDARY, 14);
    lv_obj_set_pos(label, 0, 100);
    body = lv_obj_create(panel);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, 140);
    lv_obj_set_size(body, 632, 146);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    label = smart_home_lvgl_label_create(body,
        slot->tool.description[0] ? slot->tool.description : "暂无工具说明",
        SMART_HOME_UI_COLOR_TEXT_PRIMARY, 16);
    lv_obj_set_width(label, 612);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    label = smart_home_lvgl_label_create(panel,
        slot->tool.group == SMART_HOME_TOOL_GROUP_LOCAL ?
        "关闭后，本次运行将无法调用此工具。" : "此工具的权限由接入服务管理。",
        SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
    lv_obj_set_pos(label, 0, 302);
}

typedef struct { v9_tool_row_t *rows; size_t count; } v9_tool_snapshot_t;

static int tools_collect(const smart_home_tool_info_t *tool, void *user_data)
{
    v9_tool_snapshot_t *snapshot = user_data;
    v9_tool_row_t *row;
    if (snapshot->count >= AGENT_MAX_TOOLS) return AGENT_ERROR_CAPACITY;
    row = &snapshot->rows[snapshot->count++];
    snprintf(row->name, sizeof(row->name), "%s", tool->name);
    snprintf(row->description, sizeof(row->description), "%s", tool->description);
    row->group = tool->group_id;
    row->enabled = tool->enabled;
    row->allowed = tool->access_allowed;
    return AGENT_OK;
}

static size_t tools_pages(void)
{
    return g_settings.tool_count ?
        (g_settings.tool_count + V9_TOOLS_PER_PAGE - 1u) / V9_TOOLS_PER_PAGE : 1u;
}

static void tools_navigation(void)
{
    char text[48];
    snprintf(text, sizeof(text), "%u / %u", (unsigned)g_settings.tools_page + 1u,
             (unsigned)tools_pages());
    tools_set_text(g_settings.tools_page_label, text);
    if (!g_settings.tools_page || g_settings.tools_rendering)
        lv_obj_add_state(g_settings.tools_prev, LV_STATE_DISABLED);
    else lv_obj_remove_state(g_settings.tools_prev, LV_STATE_DISABLED);
    if (g_settings.tools_page + 1u >= tools_pages() || g_settings.tools_rendering)
        lv_obj_add_state(g_settings.tools_next, LV_STATE_DISABLED);
    else lv_obj_remove_state(g_settings.tools_next, LV_STATE_DISABLED);
}

static void tools_render_begin(void)
{
    g_settings.tools_cursor = 0;
    g_settings.tools_rendering = 1;
    g_settings.tools_row_max_ms = 0;
    tools_navigation();
    /* Hide only stale slots. Unchanged cards retain their objects and labels. */
    for (size_t i = 0; i < V9_TOOLS_PER_PAGE; ++i) {
        v9_tool_slot_t *slot = &g_settings.tool_slots[i];
        size_t index = g_settings.tools_page * V9_TOOLS_PER_PAGE + i;
        if (index >= g_settings.tool_count || !slot->valid ||
            strcmp(slot->tool.name, g_settings.tools[index].name)) {
            slot->valid = 0;
            lv_obj_add_flag(slot->row, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void tools_summary(void)
{
    char text[96];
    if (!g_settings.tool_count) {
        tools_set_text(g_settings.tools_status, "暂无可用工具 · 可稍后刷新");
        return;
    }
    snprintf(text, sizeof(text), "%u 项工具 · 权限调整本次运行有效",
             (unsigned)g_settings.tool_count);
    tools_set_text(g_settings.tools_status, text);
}

/* Each timer turn either copies a small registry snapshot or binds one card.
 * Registry access is a trylock; LVGL objects stay on the UI owner thread. */
static void tools_timer_cb(lv_timer_t *timer)
{
    smart_home_lvgl_t *ui = lv_timer_get_user_data(timer);
    if (g_settings.category != V9_CAT_TOOLS || lv_screen_active() != g_settings.screen) {
        lv_timer_pause(timer);
        return;
    }
    if (g_settings.tools_requested) {
        uint32_t started = lv_tick_get();
        v9_tool_snapshot_t snapshot = {0};
        int ret;
        g_settings.tools_requested = 0;
        if (!g_settings.tools) g_settings.tools = smart_home_bulk_alloc(
            AGENT_MAX_TOOLS * sizeof(*g_settings.tools));
        if (!g_settings.tools_scratch) g_settings.tools_scratch = smart_home_bulk_alloc(
            AGENT_MAX_TOOLS * sizeof(*g_settings.tools_scratch));
        if (!g_settings.tools || !g_settings.tools_scratch) {
            tools_set_text(g_settings.tools_status, "内存不足，请稍后重试");
            ret = AGENT_ERROR_NOMEM;
        } else {
            memset(g_settings.tools_scratch, 0,
                   AGENT_MAX_TOOLS * sizeof(*g_settings.tools_scratch));
            snapshot.rows = g_settings.tools_scratch;
            ret = smart_home_agent_app_try_enumerate_tools(ui->app, tools_collect, &snapshot);
            if (ret != AGENT_OK) tools_set_text(g_settings.tools_status,
                ret == AGENT_ERROR_BUSY ? "工具正在使用中，请稍后刷新" : "工具目录读取失败");
        }
        g_settings.tools_snapshot_ms = lv_tick_elaps(started);
        lv_obj_remove_state(g_settings.tools_refresh, LV_STATE_DISABLED);
        if (ret == AGENT_OK) {
            int changed = !g_settings.tools_valid || snapshot.count != g_settings.tool_count ||
                memcmp(g_settings.tools, snapshot.rows, snapshot.count * sizeof(*snapshot.rows));
            if (changed) {
                memcpy(g_settings.tools, snapshot.rows, snapshot.count * sizeof(*snapshot.rows));
                g_settings.tool_count = snapshot.count;
                g_settings.tools_valid = 1;
                if (g_settings.tools_page >= tools_pages())
                    g_settings.tools_page = tools_pages() - 1u;
                tools_render_begin();
            }
            tools_summary();
        }
        if (!g_settings.tools_rendering) lv_timer_pause(timer);
        return;
    }
    if (g_settings.tools_rendering) {
        size_t index = g_settings.tools_page * V9_TOOLS_PER_PAGE + g_settings.tools_cursor;
        v9_tool_slot_t *slot = &g_settings.tool_slots[g_settings.tools_cursor];
        uint32_t started = lv_tick_get();
        if (index < g_settings.tool_count) {
            slot->tool = g_settings.tools[index];
            slot->valid = 1;
            tools_set_text(slot->name, slot->tool.name);
            tool_slot_state(slot);
            lv_obj_remove_flag(slot->row, LV_OBJ_FLAG_HIDDEN);
        } else {
            slot->valid = 0;
            lv_obj_add_flag(slot->row, LV_OBJ_FLAG_HIDDEN);
        }
        uint32_t elapsed = lv_tick_elaps(started);
        if (elapsed > g_settings.tools_row_max_ms) g_settings.tools_row_max_ms = elapsed;
        if (++g_settings.tools_cursor == V9_TOOLS_PER_PAGE) {
            g_settings.tools_rendering = 0;
            tools_navigation();
            printf("[v9-tools] count=%u page=%u snapshot_ms=%lu row_max_ms=%lu\n",
                (unsigned)g_settings.tool_count, (unsigned)g_settings.tools_page + 1u,
                (unsigned long)g_settings.tools_snapshot_ms,
                (unsigned long)g_settings.tools_row_max_ms);
            lv_timer_pause(timer);
        }
    } else lv_timer_pause(timer);
}

static void tools_schedule(smart_home_lvgl_t *ui)
{
    if (!g_settings.tools_timer) {
        g_settings.tools_timer = lv_timer_create(tools_timer_cb, V9_TOOLS_STEP_MS, ui);
        if (!g_settings.tools_timer) {
            g_settings.tools_requested = g_settings.tools_rendering = 0;
            tools_set_text(g_settings.tools_status, "内存不足，请稍后重试");
            lv_obj_remove_state(g_settings.tools_refresh, LV_STATE_DISABLED);
            tools_navigation();
        }
    } else {
        lv_timer_set_user_data(g_settings.tools_timer, ui);
        lv_timer_reset(g_settings.tools_timer);
        lv_timer_resume(g_settings.tools_timer);
    }
}

static void tools_refresh(smart_home_lvgl_t *ui)
{
    if (!ui || !ui->app || !g_settings.tools_body) return;
    g_settings.tools_requested = 1;
    if (!g_settings.tools_valid) tools_set_text(g_settings.tools_status, "正在读取工具目录…");
    lv_obj_add_state(g_settings.tools_refresh, LV_STATE_DISABLED);
    tools_schedule(ui);
}

static void tools_refresh_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED)
        tools_refresh(lv_event_get_user_data(event));
}

static void tools_page_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    int next = lv_event_get_current_target(event) == g_settings.tools_next;
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || g_settings.tools_rendering ||
        (!next && !g_settings.tools_page) ||
        (next && g_settings.tools_page + 1u >= tools_pages())) return;
    if (next) ++g_settings.tools_page;
    else --g_settings.tools_page;
    tools_render_begin();
    tools_schedule(ui);
}

static lv_obj_t *tools_button(lv_obj_t *panel, const char *text, int x, int y,
                               int width, lv_event_cb_t callback, smart_home_lvgl_t *ui)
{
    lv_obj_t *button = lv_button_create(panel);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 34);
    lv_obj_set_style_radius(button, 10, 0);
    smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_SURFACE_ON);
    smart_home_lvgl_press_feedback(button);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, ui);
    lv_obj_t *label = smart_home_lvgl_label_create(button, text,
        SMART_HOME_UI_COLOR_TEXT_PRIMARY, 14);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(label);
    return button;
}

static void build_tools_panel(smart_home_lvgl_t *ui, lv_obj_t *panel)
{
    g_settings.tools_owner = ui;
    panel_title(panel, "工具权限");
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    g_settings.tools_status = smart_home_lvgl_label_create(panel,
        "按来源管理工具 · 点击卡片查看说明", SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
    lv_obj_set_pos(g_settings.tools_status, 0, 40);
    g_settings.tools_refresh = tools_button(panel, "刷新目录", 604, 0, 100,
                                             tools_refresh_cb, ui);
    g_settings.tools_body = lv_obj_create(panel);
    lv_obj_remove_style_all(g_settings.tools_body);
    lv_obj_set_pos(g_settings.tools_body, 0, 76);
    lv_obj_set_size(g_settings.tools_body, 704, 234);
    lv_obj_clear_flag(g_settings.tools_body, LV_OBJ_FLAG_SCROLLABLE);
    for (size_t i = 0; i < V9_TOOLS_PER_PAGE; ++i) {
        v9_tool_slot_t *slot = &g_settings.tool_slots[i];
        slot->row = lv_obj_create(g_settings.tools_body);
        lv_obj_remove_style_all(slot->row);
        lv_obj_set_pos(slot->row, 0, (int)i * 80);
        lv_obj_set_size(slot->row, 704, 74);
        lv_obj_set_style_radius(slot->row, 12, 0);
        smart_home_lvgl_set_bg(slot->row, SMART_HOME_UI_COLOR_SURFACE_SOFT);
        lv_obj_clear_flag(slot->row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(slot->row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
        smart_home_lvgl_press_feedback(slot->row);
        lv_obj_add_event_cb(slot->row, tool_detail_cb, LV_EVENT_CLICKED, slot);
        slot->name = smart_home_lvgl_label_create(slot->row, "",
            SMART_HOME_UI_COLOR_TEXT_PRIMARY, 17);
        lv_obj_set_pos(slot->name, 16, 9);
        lv_obj_set_width(slot->name, 570);
        lv_label_set_long_mode(slot->name, LV_LABEL_LONG_DOT);
        slot->meta = smart_home_lvgl_label_create(slot->row, "",
            SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
        lv_obj_set_pos(slot->meta, 16, 40);
        lv_obj_set_width(slot->meta, 570);
        lv_label_set_long_mode(slot->meta, LV_LABEL_LONG_DOT);
        slot->toggle = lv_switch_create(slot->row);
        lv_obj_set_size(slot->toggle, 50, 28);
        lv_obj_align(slot->toggle, LV_ALIGN_RIGHT_MID, -16, 0);
        lv_obj_set_style_bg_color(slot->toggle, SMART_HOME_UI_COLOR_PRIMARY,
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_clear_flag(slot->toggle, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_user_data(slot->toggle, slot);
        lv_obj_add_event_cb(slot->toggle, tool_switch_cb, LV_EVENT_VALUE_CHANGED, ui);
    }
    lv_obj_t *label = smart_home_lvgl_label_create(panel, "点击卡片查看说明",
        SMART_HOME_UI_COLOR_TEXT_MUTED, 13);
    lv_obj_set_pos(label, 0, 337);
    g_settings.tools_prev = tools_button(panel, "上一页", 352, 326, 96, tools_page_cb, ui);
    g_settings.tools_next = tools_button(panel, "下一页", 608, 326, 96, tools_page_cb, ui);
    g_settings.tools_page_label = smart_home_lvgl_label_create(panel, "1 / 1",
        SMART_HOME_UI_COLOR_TEXT_SECONDARY, 14);
    lv_obj_set_pos(g_settings.tools_page_label, 468, 335);
    lv_obj_set_width(g_settings.tools_page_label, 120);
    lv_obj_set_style_text_align(g_settings.tools_page_label, LV_TEXT_ALIGN_CENTER, 0);
    tools_navigation();
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
#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
    g_settings.mcp_status = NULL;
    g_settings.mcp_apply = NULL;
    g_settings.mcp_discover = NULL;
    g_settings.mcp_state_valid = 0;
#endif
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
    g_settings.node_status = NULL;
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

#if !defined(CONFIG_SMART_HOME_MILOCO_BRIDGE) || !defined(CONFIG_SMART_HOME_MCP_BRIDGE) || !defined(CONFIG_SMART_HOME_NODE_GATEWAY)
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

#endif

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
static void *mcp_apply_worker(void *argument)
{
    smart_home_lvgl_t *ui = argument;
    smart_home_mcp_bridge_t *bridge = ui->app->mcp_bridge;
    int ret = AGENT_OK;
    if (!bridge) {
        ret = smart_home_mcp_bridge_start(&bridge, ui->app->agent, &ui->app->agent_mutex);
        if (ret == AGENT_OK) ui->app->mcp_bridge = bridge;
    }
    if (ret == AGENT_OK)
        ret = smart_home_mcp_bridge_reconfigure(bridge, &g_mcp.config);
    memset(&g_mcp.config, 0, sizeof(g_mcp.config));
    pthread_mutex_lock(&ui->pending_mutex);
    g_mcp.result = ret;
    g_mcp.ready = 1;
    pthread_mutex_unlock(&ui->pending_mutex);
    return NULL;
}

static void mcp_poll(smart_home_lvgl_t *ui)
{
    int ready;
    pthread_mutex_lock(&ui->pending_mutex);
    ready = g_mcp.ready;
    g_mcp.ready = 0;
    pthread_mutex_unlock(&ui->pending_mutex);
    if (!ready) return;
    pthread_join(g_mcp.thread, NULL);
    g_mcp.active = 0;
    ui->app->system_status.mcp_bridge_status = g_mcp.result;
    if (g_settings.mcp_apply) lv_obj_remove_state(g_settings.mcp_apply, LV_STATE_DISABLED);
    if (g_settings.mcp_status) lv_label_set_text(g_settings.mcp_status,
        g_mcp.result == AGENT_OK ? "配置已应用，可点击发现工具（本次运行有效）" :
        g_mcp.result == AGENT_ERROR_BUSY ? "发现进行中，请稍后重试" :
        "配置应用失败，请检查 mcp_bridge.json、工具白名单与凭据");
}

static void mcp_discover_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui || !ui->app || g_mcp.active)
        return;
    if (!ui->app->mcp_bridge) {
        lv_label_set_text(g_settings.mcp_status, "请先部署有效的 mcp_bridge.json 并应用配置");
        return;
    }
    int ret = smart_home_mcp_bridge_request_discover(ui->app->mcp_bridge);
    lv_label_set_text(g_settings.mcp_status, ret == AGENT_OK ? "正在发现工具…" : "发现请求失败，请稍后重试");
}

static void mcp_apply_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    smart_home_mcp_runtime_config_t config;
    int ret;
    char *end;
    long port;
    const char *port_text;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui || !ui->app ||
        g_mcp.active || !g_settings.mcp_server) {
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
    g_mcp.owner = ui;
    g_mcp.config = config;
    memset(&config, 0, sizeof(config));
    g_mcp.ready = 0;
    g_mcp.active = 1;
    pthread_attr_t attr;
    ret = pthread_attr_init(&attr);
    if (ret == 0) {
        ret = pthread_attr_setstacksize(&attr, 16384u);
        if (ret == 0) ret = pthread_create(&g_mcp.thread, &attr, mcp_apply_worker, ui);
        pthread_attr_destroy(&attr);
    }
    if (ret != 0) {
        g_mcp.active = 0;
        memset(&g_mcp.config, 0, sizeof(g_mcp.config));
        lv_label_set_text(g_settings.mcp_status, "无法创建配置任务");
        return;
    }
    if (g_settings.mcp_apply) lv_obj_add_state(g_settings.mcp_apply, LV_STATE_DISABLED);
    lv_label_set_text(g_settings.mcp_status, "正在应用配置…");
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
    g_settings.mcp_apply = button;
    label = smart_home_lvgl_label_create(button, "应用配置",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 13);
    lv_obj_center(label);
    button = lv_button_create(panel);
    lv_obj_set_size(button, 160, 36);
    lv_obj_set_pos(button, 180, 336);
    lv_obj_add_event_cb(button, mcp_discover_cb, LV_EVENT_CLICKED, ui);
    g_settings.mcp_discover = button;
    label = smart_home_lvgl_label_create(button, "发现 / 重试",
        SMART_HOME_UI_COLOR_TEXT_PRIMARY, 13);
    lv_obj_center(label);
    smart_home_mcp_runtime_config_t current;
    if (!g_mcp.active && ui && ui->app && ui->app->mcp_bridge &&
        smart_home_mcp_bridge_get_config(ui->app->mcp_bridge, &current) == AGENT_OK) {
        char port[8];
        snprintf(port, sizeof(port), "%u", (unsigned)current.port);
        lv_textarea_set_text(g_settings.mcp_server, current.server_id);
        lv_textarea_set_text(g_settings.mcp_host, current.host);
        lv_textarea_set_text(g_settings.mcp_port, port);
        lv_textarea_set_text(g_settings.mcp_path, current.path);
        lv_textarea_set_text(g_settings.mcp_auth, current.authorization);
        lv_dropdown_set_selected(g_settings.mcp_protocol, current.use_tls ? 0 : 1);
        memset(&current, 0, sizeof(current));
    }
    if (g_mcp.active) lv_obj_add_state(g_settings.mcp_apply, LV_STATE_DISABLED);

}
#endif

#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
static void node_toggle_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    long port;
    char *end;
    int ret;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui || !ui->app) {
        return;
    }
    if (ui->request_inflight) {
        lv_label_set_text(g_settings.node_status, "请等待当前聊天请求完成后启停网关");
        return;
    }
    if (ui->app->node_gateway) {
        smart_home_node_gateway_stop(&ui->app->node_gateway);
        ui->app->system_status.node_gateway_status = AGENT_ERROR_STATE;
        lv_label_set_text(g_settings.node_status, "已停止");
        g_settings.nodes_valid = 0;
        services_refresh(ui);
        nodes_refresh(ui);
        return;
    }
    const char *text = lv_textarea_get_text(g_settings.node_port);
    port = strtol(text, &end, 10);
    if (end == text || *end != '\0' || port <= 0 || port > 65535) {
        lv_label_set_text(g_settings.node_status, "端口不合法");
        return;
    }
    ret = smart_home_node_gateway_start_with_port(
        &ui->app->node_gateway, ui->app->agent, &ui->app->agent_mutex,
        (uint16_t)port);
    lv_label_set_text(g_settings.node_status,
                      ret == AGENT_OK ? "监听中（本次运行有效）" :
                      "开启失败，请检查 secrets.json 中的 node_gateway.shared_token");
    if (ret == AGENT_OK) g_settings.node_last_port = (uint16_t)port;
    ui->app->system_status.node_gateway_status = ret;
    g_settings.nodes_valid = 0;
    services_refresh(ui);
    nodes_refresh(ui);
}

static void nodes_refresh(smart_home_lvgl_t *ui)
{
    uint32_t revision = 0;
    size_t count = 0;
    caddons_node_info_t *nodes;
    if (!ui || !ui->app || !g_settings.nodes_body) return;
    nodes = smart_home_bulk_alloc(CAGENT_ADDONS_MAX_NODES * sizeof(*nodes));
    if (!nodes) return;
    if (ui->app->node_gateway)
        count = smart_home_node_gateway_list(ui->app->node_gateway, nodes,
                                             CAGENT_ADDONS_MAX_NODES, &revision);
    if (g_settings.nodes_valid && g_settings.nodes_revision == revision &&
        g_settings.nodes_gateway == ui->app->node_gateway && g_settings.nodes_count == count) {
        smart_home_bulk_free(nodes);
        return;
    }
    g_settings.nodes_valid = 1;
    g_settings.nodes_revision = revision;
    g_settings.nodes_gateway = ui->app->node_gateway;
    g_settings.nodes_count = count;
    lv_obj_clean(g_settings.nodes_body);
    if (!count) smart_home_lvgl_label_create(g_settings.nodes_body,
        ui->app->node_gateway ? "等待 Node 节点接入" : "Node 网关未开启",
        SMART_HOME_UI_COLOR_TEXT_MUTED, 14);
    for (size_t i = 0; i < count && i < CAGENT_ADDONS_MAX_NODES; i++) {
        char text[384];
        snprintf(text, sizeof(text), "%s · %s\nID: %s",
            nodes[i].display_name[0] ? nodes[i].display_name : nodes[i].node_id,
            nodes[i].online ? "在线" : "离线", nodes[i].node_id);
        lv_obj_t *label = smart_home_lvgl_label_create(g_settings.nodes_body, text,
            SMART_HOME_UI_COLOR_TEXT_PRIMARY, 14);
        lv_obj_set_width(label, 680);
        size_t used = (size_t)snprintf(text, sizeof(text), "可用命令：");
        for (size_t j = 0; j < nodes[i].command_count && j < CAGENT_ADDONS_MAX_NODE_COMMANDS; j++) {
            int written = snprintf(text + used, sizeof(text) - used, "%s%s",
                j ? "、" : "", nodes[i].commands[j]);
            if (written < 0 || (size_t)written >= sizeof(text) - used) break;
            used += (size_t)written;
        }
        label = smart_home_lvgl_label_create(g_settings.nodes_body, text,
            SMART_HOME_UI_COLOR_TEXT_SECONDARY, 13);
        lv_obj_set_width(label, 680);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    }
    smart_home_bulk_free(nodes);
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
    uint16_t port = ui && ui->app && ui->app->node_gateway ?
        smart_home_node_gateway_port(ui->app->node_gateway) : g_settings.node_last_port;
    if (!port) port = CONFIG_SMART_HOME_NODE_GATEWAY_PORT;
    snprintf(buffer, sizeof(buffer), "%u", (unsigned)port);
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
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    if (index == 0) g_settings.miloco_service_status = label;
#endif
#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
    if (index == 1) g_settings.mcp_service_status = label;
#endif
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
    if (index == 2) g_settings.node_service_status = label;
#endif
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

static void services_refresh(smart_home_lvgl_t *ui)
{
    if (!ui || !ui->app) return;
#ifdef CONFIG_SMART_HOME_MCP_BRIDGE
    const char *text = "未配置 / 初始化失败";
    smart_home_mcp_state_t state = SMART_HOME_MCP_STATE_IDLE;
    if (g_mcp.active) text = "正在应用配置";
    else if (ui->app->mcp_bridge &&
        smart_home_mcp_bridge_get_state(ui->app->mcp_bridge, &state, NULL) == AGENT_OK) {
        switch (state) {
        case SMART_HOME_MCP_STATE_IDLE: text = "等待发现工具"; break;
        case SMART_HOME_MCP_STATE_CONNECTING: text = "正在发现工具"; break;
        case SMART_HOME_MCP_STATE_READY: text = "工具已就绪"; break;
        default: text = "发现失败，可重试"; break;
        }
        if (g_settings.mcp_discover) {
            if (state == SMART_HOME_MCP_STATE_CONNECTING)
                lv_obj_add_state(g_settings.mcp_discover, LV_STATE_DISABLED);
            else lv_obj_remove_state(g_settings.mcp_discover, LV_STATE_DISABLED);
        }
    }
    if (g_settings.mcp_service_status &&
        strcmp(lv_label_get_text(g_settings.mcp_service_status), text))
        lv_label_set_text(g_settings.mcp_service_status, text);
    if (g_settings.mcp_status && !g_mcp.active && ui->app->mcp_bridge &&
        (!g_settings.mcp_state_valid || g_settings.mcp_observed_state != state)) {
        g_settings.mcp_state_valid = 1;
        g_settings.mcp_observed_state = state;
        if (state == SMART_HOME_MCP_STATE_READY || state == SMART_HOME_MCP_STATE_FAILED)
            lv_label_set_text(g_settings.mcp_status, text);
    }
#endif
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
    const char *node = ui->app->node_gateway ? "监听中" : "未开启";
    if (g_settings.node_service_status &&
        strcmp(lv_label_get_text(g_settings.node_service_status), node))
        lv_label_set_text(g_settings.node_service_status, node);
#endif
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
#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
    lv_obj_t *heading = smart_home_lvgl_label_create(panel, "Node 节点与命令",
        SMART_HOME_UI_COLOR_TEXT_PRIMARY, 18);
    lv_obj_set_pos(heading, 0, 370);
    g_settings.nodes_body = lv_obj_create(panel);
    lv_obj_remove_style_all(g_settings.nodes_body);
    lv_obj_set_pos(g_settings.nodes_body, 0, 410);
    lv_obj_set_size(g_settings.nodes_body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_settings.nodes_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_settings.nodes_body, 12, 0);
    nodes_refresh(ui);
#endif

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
        lv_obj_add_event_cb(button, category_cb, LV_EVENT_CLICKED, ui);
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

    build_display_panel(g_settings.panel[0]);
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
#elif defined(CONFIG_SMART_HOME_KWS)
    smart_home_lvgl_build_wake_settings(ui, g_settings.panel[V9_CAT_VOICE]);
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

    services_refresh(ui);
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
