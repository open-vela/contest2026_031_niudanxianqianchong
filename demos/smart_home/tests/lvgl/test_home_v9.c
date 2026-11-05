/* Exercise actual V9 pages against LVGL with snapshot-only services. */
#include "smart_home_lvgl_internal.h"
#include "../../src/smart_home_memory.h"
#include <stdio.h>
#include <string.h>
#include "../../src/config/smart_home_secrets.h"
#include "smart_home_v9_device_view.h"
#include "smart_home_lvgl_room_dialog.h"
#ifdef CONFIG_SMART_HOME_ONBOARDING
#include "smart_home_lvgl_onboarding.h"
#endif
#include "icons/smart_home_lvgl_png_icons.h"
#include "../../src/config/smart_home_backends.h"
#include <assert.h>
#include <malloc.h>
#include <stdlib.h>
#include <time.h>

static void test_host_frame_budget(smart_home_lvgl_t *ui);
static void test_screen_cycle(smart_home_lvgl_t *ui);
static void test_v9_cards(smart_home_lvgl_t *ui);
static void test_v9_scene_switcher(smart_home_lvgl_t *ui);
static void test_v9_room_catalog(smart_home_lvgl_t *ui);
static int local_state_enabled;
static uint32_t local_revision;
static void capture_v9_device_modals(smart_home_lvgl_t *ui);
static void capture_v9_settings_categories(smart_home_lvgl_t *ui);
static void capture_v9_model_menu(smart_home_lvgl_t *ui);
static void capture_v9_mi_qr(smart_home_lvgl_t *ui);
static void test_v9_shell(smart_home_lvgl_t *ui);
#ifdef CONFIG_SMART_HOME_ONBOARDING
static void test_v9_onboarding(smart_home_lvgl_t *ui);
#endif
static void test_v9_settings_clicks(smart_home_lvgl_t *ui);
static lv_obj_t *find_v9_label(lv_obj_t *root, const char *text);

static lv_obj_t *card_image(lv_obj_t *card)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(card); i++) {
        lv_obj_t *child = lv_obj_get_child(card, i);
        if (lv_obj_check_type(child, &lv_image_class)) return child;
    }
    return NULL;
}

static uint16_t pixels[1024 * 600];
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{ (void)area; (void)data; lv_display_flush_ready(display); }
static void capture_screen(lv_obj_t *screen, const char *env)
{
    const char *path = getenv(env);
    if (!path || !screen) return;
    lv_screen_load(screen);
    lv_refr_now(NULL);
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n1024 600\n255\n");
    for (size_t i = 0; i < sizeof(pixels) / sizeof(pixels[0]); i++) {
        unsigned p = pixels[i];
        uint8_t rgb[] = {((p >> 11) & 31) * 255 / 31,
                         ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}

static void capture_to(smart_home_lvgl_t *ui, const char *env)
{
    capture_screen(ui->screen_home, env);
}

static smart_home_miloco_device_t snapshot[SMART_HOME_MILOCO_MAX_DEVICES];
static size_t snapshot_count;
static uint32_t snapshot_revision = 1;
static unsigned snapshot_reads;
static unsigned network_probe_calls;
size_t smart_home_miloco_list(const smart_home_miloco_t *service,
                             smart_home_miloco_device_t *out,
                             size_t capacity, uint32_t *revision)
{
    (void)service;
    if (revision) *revision = snapshot_revision;
    if (out) snapshot_reads++;
    size_t n = capacity < snapshot_count ? capacity : snapshot_count;
    if (out) memcpy(out, snapshot, n * sizeof(*out));
    return out ? n : snapshot_count;
}

#ifdef CONFIG_SMART_HOME_NODE_GATEWAY
uint16_t smart_home_node_gateway_port(const smart_home_node_gateway_t *gateway)
{ return gateway ? 18790 : 0; }
int smart_home_node_gateway_start_with_port(smart_home_node_gateway_t **out,
    agent_t *agent, pthread_mutex_t *mutex, uint16_t port)
{ (void)agent; (void)mutex; (void)port; *out = (smart_home_node_gateway_t *)out; return AGENT_OK; }
void smart_home_node_gateway_stop(smart_home_node_gateway_t **gateway) { *gateway = NULL; }
static caddons_node_info_t node_snapshot[CAGENT_ADDONS_MAX_NODES];
static size_t node_count;
size_t smart_home_node_gateway_list(const smart_home_node_gateway_t *gateway,
    caddons_node_info_t *out, size_t capacity, uint32_t *revision)
{
    (void)gateway;
    if (revision) *revision = 1;
    size_t n = capacity < node_count ? capacity : node_count;
    if (out) memcpy(out, node_snapshot, n * sizeof(*out));
    return out ? n : node_count;
}
#endif

static int submit_result;
static int submissions;
static int scene_result;
static bool gateway_online = true;
bool smart_home_miloco_copy_device(const smart_home_miloco_t *service,
                                    const char *did, smart_home_miloco_device_t *out)
{
    (void)service;
    for (size_t i = 0; i < snapshot_count; i++) {
        if (!strcmp(snapshot[i].did, did)) { *out = snapshot[i]; return true; }
    }
    return false;
}
int smart_home_miloco_submit_control_tracked(smart_home_miloco_t *service,
    const char *did, const char *iid, const char *operation, int32_t value,
    uint32_t *request_id)
{
    if (request_id) *request_id = 1;
    return smart_home_miloco_submit_control(service, did, iid, operation, value);
}
bool smart_home_miloco_control_result(const smart_home_miloco_t *service,
                                      uint32_t id, int *result)
{ (void)service; (void)id; (void)result; return false; }
bool smart_home_miloco_control_result_detail(const smart_home_miloco_t *service,
    uint32_t id, int *result, uint32_t *observation)
{ (void)service; (void)id; (void)result; (void)observation; return false; }
void *smart_home_bulk_alloc(size_t size) { return malloc(size); }
void smart_home_bulk_free(void *ptr) { free(ptr); }
bool smart_home_miloco_reachable(const smart_home_miloco_t *service)
{ (void)service; return gateway_online; }
int smart_home_network_probe(smart_home_network_status_t *status)
{
    assert(status);
    network_probe_calls++;
    status->ip_status = SMART_HOME_NETWORK_OK;
    status->dns_status = SMART_HOME_NETWORK_OK;
    status->online = true;
    return SMART_HOME_NETWORK_OK;
}
int smart_home_miloco_submit_power(smart_home_miloco_t *service,
                                   const char *did, bool on)
{ (void)service; (void)did; (void)on; submissions++; return submit_result; }
int smart_home_miloco_submit_control(smart_home_miloco_t *service,
    const char *did, const char *iid, const char *operation, int32_t value)
{
    (void)service; (void)did; (void)iid; (void)operation; (void)value;
    submissions++;
    return submit_result;
}

/* 首页场景切换器直接调用真实场景执行入口；宿主 harness 不链接 miloco
 * 服务，这里记录调用次数与 scene id 供断言。 */
static int scene_runs;
static char last_scene[32];
int smart_home_miloco_run_scene(smart_home_miloco_t *service, const char *scene,
                                char *report, size_t report_size)
{
    (void)service;
    scene_runs++;
    snprintf(last_scene, sizeof last_scene, "%s", scene ? scene : "");
    if (report && report_size) {
        snprintf(report, report_size, "stub: %s", scene ? scene : "");
    }
    return scene_result;
}

static smart_home_miloco_weather_t weather;
bool smart_home_miloco_get_weather(const smart_home_miloco_t *service,
                                   smart_home_miloco_weather_t *out)
{ (void)service; *out = weather; return weather.valid; }
bool smart_home_miloco_bound(const smart_home_miloco_t *service)
{ (void)service; return true; }
/* 米家连接配置弹窗的桩：测试只验证 UI 装配，不落盘。 */
bool smart_home_miloco_config_valid(const smart_home_miloco_config_t *config)
{ return config && config->host[0] && config->port != 0; }
bool smart_home_miloco_get_config(const smart_home_miloco_t *service,
                                  smart_home_miloco_config_t *config)
{
    (void)service;
    if (!config) return false;
    memset(config, 0, sizeof(*config));
    snprintf(config->host, sizeof(config->host), "192.168.1.20");
    config->port = 1810;
    return true;
}
int smart_home_miloco_request_save(smart_home_miloco_t *service,
                                   const smart_home_miloco_config_t *config)
{ (void)service; (void)config; return AGENT_OK; }
int smart_home_miloco_save_status(const smart_home_miloco_t *service)
{ (void)service; return AGENT_ERROR_NOT_FOUND; }
int smart_home_secrets_get_wifi_credentials(char *ssid, size_t ssid_size,
                                            char *password, size_t password_size)
{ (void)ssid; (void)ssid_size; (void)password; (void)password_size; return AGENT_ERROR_NOT_FOUND; }
int smart_home_secrets_get_model_api_key(const char *backend, char *key, size_t size)
{ (void)backend; (void)key; (void)size; return AGENT_ERROR_NOT_FOUND; }
int smart_home_model_config_validate(const smart_home_model_config_t *config,
                                     char *error, size_t error_size)
{ (void)config; (void)error; (void)error_size; return AGENT_OK; }
int smart_home_secrets_set_api_key(smart_home_secret_service_t service,
                                  const char *backend, const char *key)
{ (void)service; (void)backend; (void)key; return AGENT_OK; }
int smart_home_miloco_reconfigure(smart_home_miloco_t *service,
                                  const smart_home_miloco_config_t *config)
{ (void)service; (void)config; return AGENT_OK; }
/* 扫码弹窗依赖的两项：secrets 回落与 QR 编码。测试只验证装配，
 * QR 用一个固定的小矩阵代替，避免链接第三方 qrcodegen。 */
int smart_home_secrets_get_miloco(char *host, size_t host_size,
                                  uint16_t *port, char *token,
                                  size_t token_size)
{
    snprintf(host, host_size, "192.168.1.20");
    if (port) *port = 1810;
    if (token && token_size) token[0] = '\0';
    return AGENT_OK;
}
/* 二维码编码用真实实现（third_party/qrcodegen），否则画出来的图
 * 与固件不一致，也无法验证扫码。桩只覆盖 secrets 回落。
 */
int smart_home_lvgl_submit_agent_job(smart_home_lvgl_t *ui, const char *text)
{ (void)ui; (void)text; return AGENT_ERROR_STATE; }
int smart_home_agent_app_clear_session(smart_home_agent_app_t *app)
{ (void)app; return AGENT_ERROR_STATE; }

static lv_point_t pointer_point;
static lv_indev_state_t pointer_state;
static void read_pointer(lv_indev_t *input, lv_indev_data_t *data)
{ (void)input; data->point = pointer_point; data->state = pointer_state; }
static void touch(lv_indev_t *input, lv_obj_t *target, lv_indev_state_t state)
{
    lv_area_t bounds;
    lv_obj_get_coords(target, &bounds);
    pointer_point.x = (bounds.x1 + bounds.x2) / 2;
    pointer_point.y = (bounds.y1 + bounds.y2) / 2;
    pointer_state = state;
    lv_tick_inc(40);
    lv_indev_read(input);
}

static void test_home(smart_home_lvgl_t *ui)
{
    snapshot_count = 0;
    ui->device_state = &ui->app->device_state;
    smart_home_lvgl_build_screensaver_screen(ui);
    /* Only one simulated light remains; real device kinds are tested below. */
    weather.valid = true;
    weather.temperature = 22;
#ifdef CONFIG_SMART_HOME_ONBOARDING
    /* 本用例验证的是首页本身，不是引导浮层：先让三项都算「已配置」，
     * 否则浮层遮罩会挡住卡片与场景按钮的触摸。引导浮层由
     * test_v9_onboarding 单独覆盖。 */
    smart_home_lvgl_onboarding_reset();
    ui->app->system_status.network_status.online = true;
    ui->app->system_status.model_status = AGENT_OK;
    gateway_online = true;
#endif
    smart_home_lvgl_build_home_screen_v9(ui);
    weather.valid = true;
    strcpy(weather.city, "Beijing");
    strcpy(weather.condition_cn, "小雨");
    strcpy(weather.condition_en, "Light rain");
    weather.temperature = 18;
    weather.humidity = 72;
    smart_home_lvgl_refresh_home(ui);
    assert(!strcmp(lv_label_get_text(ui->screensaver_temp_label), "18°"));
    assert(!strcmp(lv_label_get_text(ui->home_weather_temp_label), "18°"));
    assert(!strcmp(lv_label_get_text(ui->screensaver_condition_label), "小雨"));
    gateway_online = false;
    smart_home_lvgl_refresh_home(ui);
    assert(!strcmp(lv_label_get_text(ui->home_weather_temp_label), "18°"));
    gateway_online = true;
    weather.valid = false;
    smart_home_lvgl_refresh_home(ui);
    assert(!strcmp(lv_label_get_text(ui->screensaver_condition_label), "晴天"));
    assert(!strcmp(lv_label_get_text(ui->screensaver_temp_label), "--°"));
    smart_home_lvgl_update_clock_at(ui, 1574985600); /* board's 2019 boot date */
    assert(!strcmp(lv_label_get_text(ui->home_time_label), "12:00"));
    assert(!strcmp(lv_label_get_text(ui->screensaver_time_label), "12:00"));
    assert(!strcmp(lv_label_get_text(ui->screensaver_date_label), ""));
    struct tm moment = {.tm_year = 126, .tm_mon = 9, .tm_mday = 26,
                        .tm_hour = 15, .tm_min = 59};
    time_t utc = timegm(&moment);
    smart_home_lvgl_update_clock_at(ui, utc);
    assert(!strcmp(lv_label_get_text(ui->home_time_label), "23:59"));
    assert(strstr(lv_label_get_text(ui->screensaver_date_label), "10月26日"));
    smart_home_lvgl_update_clock_at(ui, utc + 60);
    assert(!strcmp(lv_label_get_text(ui->screensaver_time_label), "00:00"));
    assert(strstr(lv_label_get_text(ui->screensaver_date_label), "10月27日"));
    assert(!strcmp(lv_label_get_text(ui->home_date_label),
                   lv_label_get_text(ui->screensaver_date_label)));
    weather.valid = true;
    weather.temperature = 22;
    smart_home_lvgl_refresh_home(ui);
    struct tm design_time = {.tm_year = 126, .tm_mon = 9, .tm_mday = 30,
                              .tm_hour = 11, .tm_min = 39};
    smart_home_lvgl_update_clock_at(ui, timegm(&design_time));
    assert(ui->screen_home != NULL);
    assert(ui->home_weather_condition_label != NULL);
    assert(smart_home_lvgl_v9_visible_card_count() == 1);
    lv_obj_t *chandelier = find_v9_label(ui->screen_home, "吊灯");
    assert(chandelier);
    lv_obj_t *chandelier_image = card_image(lv_obj_get_parent(chandelier));
    assert(chandelier_image);
    const char *chandelier_src = lv_image_get_src(chandelier_image);
    const char *chandelier_basename = strrchr(chandelier_src, '/');
    chandelier_basename = chandelier_basename ? chandelier_basename + 1 :
                                                  chandelier_src;
    assert(!strcmp(chandelier_basename, "light-on.png"));
    assert(smart_home_lvgl_v9_select_room(3) == 0); /* 书房: empty */
    assert(smart_home_lvgl_v9_select_room(1) == 1); /* 客厅: demo light */
    assert(smart_home_lvgl_v9_select_room(0) == 1);
    assert(!find_v9_label(ui->screen_home, "本机与米家设备"));
    assert(!find_v9_label(ui->screen_home, "演示数据"));
    capture_to(ui, "SMART_HOME_V9_CAPTURE");
    capture_v9_device_modals(ui);
    test_v9_cards(ui);
    test_v9_scene_switcher(ui);
    test_v9_room_catalog(ui);

    snprintf(ui->app->system_status.network_status.ssid,
             sizeof(ui->app->system_status.network_status.ssid), "家庭 Wi-Fi");
    ui->app->system_status.network_status.backend = SMART_HOME_NETWORK_BACKEND_WIFI;
    ui->app->system_status.network_status.ip_status = SMART_HOME_NETWORK_OK;
    ui->app->system_status.network_status.online = true;
    smart_home_lvgl_build_settings_screen_v9(ui);
    smart_home_lvgl_update_clock_at(ui, timegm(&design_time));
    assert(ui->screen_settings_v9 != NULL);
    /* 分类数随「语音服务」入口增到 7（ef99272），旧断言停在 6。 */
    assert(smart_home_lvgl_v9_settings_category_count() == 7);
    test_v9_settings_clicks(ui);
    assert(smart_home_lvgl_v9_settings_select_category(1) == 1);
    capture_screen(ui->screen_settings_v9, "SMART_HOME_V9_NETWORK_CAPTURE");
    assert(smart_home_lvgl_v9_settings_select_category(2) == 2);
    capture_screen(ui->screen_settings_v9, "SMART_HOME_V9_MODEL_CAPTURE");
    assert(smart_home_lvgl_v9_settings_select_category(99) == -1);
    /* 非法分类不改选中态，回到第一类再截图，否则这张图和 model 那张重复。 */
    assert(smart_home_lvgl_v9_settings_select_category(0) == 0);
    capture_screen(ui->screen_settings_v9, "SMART_HOME_V9_SETTINGS_CAPTURE");
    capture_v9_settings_categories(ui);
    capture_v9_mi_qr(ui);
    /* Successful binding returns to the merged V9 home. */
    lv_screen_load(ui->screen_settings_v9);
    smart_home_lvgl_open_miloco_bind(ui);
    for (int i = 0; i < 4 && ui->bind_timer; ++i) {
        lv_tick_inc(1001);
        lv_timer_handler();
    }
    assert(!smart_home_lvgl_miloco_bind_is_open(ui));
    assert(lv_screen_active() == ui->screen_home && ui->active_tab == SMART_HOME_TAB_HOME);
    capture_v9_model_menu(ui);
    smart_home_lvgl_build_chat_screen(ui);
    smart_home_lvgl_update_clock_at(ui, timegm(&design_time));
    smart_home_lvgl_append_msg_bubble(ui,
        "把客厅空调调到24度，再把风速设为自动。", 1);
    smart_home_lvgl_trace_tool(ui, "set_ac", "visual-call", -1);
    smart_home_lvgl_trace_tool(ui, "set_ac", "visual-call", 1);
    smart_home_lvgl_finish_thinking(ui);
    smart_home_lvgl_append_msg_bubble(ui,
        "已将客厅空调设为制冷 24°C，\n风速已调整为自动。", 0);
    lv_obj_update_layout(ui->screen_chat);
    lv_area_t bar_bounds, input_bounds, text_bounds;
    lv_obj_get_coords(ui->chat_input_bar, &bar_bounds);
    lv_obj_get_coords(ui->chat_input, &input_bounds);
    lv_obj_get_coords(lv_textarea_get_label(ui->chat_input), &text_bounds);
    assert(abs((input_bounds.y1 + input_bounds.y2) - (bar_bounds.y1 + bar_bounds.y2)) <= 2);
    assert(abs((text_bounds.y1 + text_bounds.y2) - (input_bounds.y1 + input_bounds.y2)) <= 2);
    capture_screen(ui->screen_chat, "SMART_HOME_V9_CHAT_CAPTURE");
    test_v9_shell(ui);
    test_screen_cycle(ui);   /* 首页与设置页都还在，才能做切换稳定性回归 */
    smart_home_lvgl_settings_v9_deinit(ui);
    lv_obj_delete(ui->screen_settings_v9);
    ui->screen_settings_v9 = NULL;
    lv_obj_delete(ui->screen_chat);
    ui->screen_chat = NULL;
    ui->topbar_clock_count = 1;
    ui->topbar_wifi_icon_count = 1;
    ui->topbar_camera_icon_count = 1;

    test_host_frame_budget(ui);
    lv_timer_delete(ui->home_time_timer);
    ui->home_time_timer = NULL;
    lv_obj_delete(ui->screen_home);
    ui->screen_home = NULL;
    if (ui->miloco_timer) lv_timer_delete(ui->miloco_timer);
    if (ui->v9_poll_timer) lv_timer_delete(ui->v9_poll_timer);
    lv_obj_delete(ui->screen_screensaver);
    puts("PASS: V9 home, cached weather, placeholders and midnight date");
}

/* 与生产一致的五个后端，否则下拉菜单只有一项，无法与设计 23-model-menu 比对。 */
static const smart_home_llm_backend_preset_t g_test_presets[] = {
    { "deepseek", "DeepSeek", "api.deepseek.com", "/chat", "443",
      "deepseek-chat", 30000, 0 },
    { "mimo", "MiMo", "api.xiaomimimo.com", "/chat", "443",
      "mimo-v2.5", 30000, 0 },
    { "qwen", "Qwen", "dashscope.aliyuncs.com", "/chat", "443",
      "qwen-turbo", 30000, 0 },
    { "openai", "OpenAI", "api.openai.com", "/chat", "443",
      "gpt-4o-mini", 30000, 0 },
    { "custom", "Custom", "", "", "", "", 30000, 0 },
};
#define TEST_PRESET_COUNT (sizeof(g_test_presets) / sizeof(g_test_presets[0]))
size_t smart_home_llm_backend_count(void) { return TEST_PRESET_COUNT; }
const smart_home_llm_backend_preset_t *smart_home_llm_backend_get(size_t i)
{ return i < TEST_PRESET_COUNT ? &g_test_presets[i] : NULL; }
const smart_home_llm_backend_preset_t *smart_home_llm_backend_default(void)
{ return &g_test_presets[0]; }
int smart_home_llm_backend_index_by_id(const char *id)
{ (void)id; return 0; }
int smart_home_llm_backend_index_for_config(
    const smart_home_model_config_t *config)
{ (void)config; return 0; }
int smart_home_model_config_from_preset(smart_home_model_config_t *config,
                                        const smart_home_llm_backend_preset_t *p,
                                        int keep_api_key)
{
    if (!config || !p) return AGENT_ERROR_INVALID;
    snprintf(config->host, sizeof(config->host), "%s", p->host);
    snprintf(config->path, sizeof(config->path), "%s", p->path);
    snprintf(config->port, sizeof(config->port), "%s", p->port);
    snprintf(config->model, sizeof(config->model), "%s", p->default_model);
    config->timeout_ms = p->default_timeout_ms;
    if (!keep_api_key) config->api_key[0] = '\0';
    return AGENT_OK;
}
int smart_home_agent_app_apply_model_config(smart_home_agent_app_t *app,
                                            const smart_home_model_config_t *c)
{ (void)app; (void)c; return AGENT_OK; }

int smart_home_agent_app_try_enumerate_tools(const smart_home_agent_app_t *app,
                                             smart_home_tool_visit_fn visitor, void *user)
{
    (void)app;
    static const struct { const char *name, *description; uint16_t group; } fixtures[] = {
        {"get_home_status", "查询家庭演示设备的当前状态。", SMART_HOME_TOOL_GROUP_LOCAL},
        {"miot_device_list", "查询米家设备、来源及可用控制能力。", SMART_HOME_TOOL_GROUP_LOCAL},
        {"board_camera", "查看本机摄像头状态，按指令采集画面。", SMART_HOME_TOOL_GROUP_LOCAL},
        {"set_light", "调整演示灯光的开关与亮度。", SMART_HOME_TOOL_GROUP_LOCAL},
        {"set_timer", "设置本机倒计时提醒。", SMART_HOME_TOOL_GROUP_LOCAL},
        {"read_skill", "查询当前可用的操作说明。", SMART_HOME_TOOL_GROUP_LOCAL},
        {"node_device", "调用局域网节点提供的设备服务。", SMART_HOME_TOOL_GROUP_NODE},
        {"mcp_weather", "使用已接入的 MCP 服务查询天气。", SMART_HOME_TOOL_GROUP_MCP},
        {"mcp_search", "使用已接入的 MCP 服务搜索信息。", SMART_HOME_TOOL_GROUP_MCP},
    };
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        smart_home_tool_info_t tool = {.name = fixtures[i].name,
            .description = fixtures[i].description, .group_id = fixtures[i].group,
            .enabled = 1, .access_allowed = 1};
        int ret = visitor(&tool, user);
        if (ret != AGENT_OK) return ret;
    }
    return AGENT_OK;
}
int smart_home_agent_app_try_set_local_tool_access(smart_home_agent_app_t *app,
                                                  const char *name, int allowed)
{ return smart_home_agent_app_set_local_tool_access(app, name, allowed); }

int smart_home_agent_app_set_local_tool_access(smart_home_agent_app_t *app,
                                               const char *tool_name,
                                               int allowed)
{
    if (!app || !tool_name) return AGENT_ERROR_INVALID;
    for (size_t i = 0; i < app->local_tool_access_count; i++) {
        if (!strcmp(app->local_tool_access[i].name, tool_name)) {
            app->local_tool_access[i].allowed = allowed ? 1 : 0;
            return AGENT_OK;
        }
    }
    return AGENT_ERROR_NOT_FOUND;
}

int smart_home_network_connect_credentials(
    smart_home_network_status_t *status, const char *ssid,
    const char *password)
{
    (void)ssid; (void)password;
    if (status) {
        memset(status, 0, sizeof(*status));
        status->ip_status = SMART_HOME_NETWORK_ERR_DHCP;
    }
    return AGENT_ERROR_INVALID;
}

static void test_host_frame_budget(smart_home_lvgl_t *ui)
{
    /* Host 侧帧预算基线：不是板端性能，只用来在改动前后对比回归。
     * 每轮强制整屏失效再跑一次 lv_timer_handler()，统计单帧耗时分布。
     */
    enum { kSamples = 300 };
    static uint32_t samples[kSamples];
    struct mallinfo before;
    struct timespec started, finished;
    unsigned i;

    if (!getenv("SMART_HOME_HOST_PERF")) return;

    /* 前面的截图流程会把活动屏切到设置页并随后删除，这里先回到首页，
     * 否则 lv_timer_handler() 会渲染已释放的屏幕。
     */
    lv_screen_load(ui->screen_home);
    lv_refr_now(NULL);
    before = mallinfo();
    for (i = 0; i < kSamples; i++) {
        lv_obj_invalidate(ui->screen_home);
        clock_gettime(CLOCK_MONOTONIC, &started);
        lv_refr_now(NULL);   /* 强制走完整重绘，而不是等刷新定时器 */
        clock_gettime(CLOCK_MONOTONIC, &finished);
        samples[i] = (uint32_t)((finished.tv_sec - started.tv_sec) * 1000000u
                                + (finished.tv_nsec - started.tv_nsec) / 1000u);
    }

    /* 采样已是很小的整数毫秒，直接插入排序后取分位。 */
    for (i = 1; i < kSamples; i++) {
        uint32_t value = samples[i];
        unsigned j = i;

        while (j > 0 && samples[j - 1] > value) {
            samples[j] = samples[j - 1];
            j--;
        }
        samples[j] = value;
    }

    printf("[host-perf] scene=home samples=%u p50_us=%u p95_us=%u p99_us=%u "
           "max_us=%u heap_used=%u heap_free=%u\n",
           (unsigned)kSamples, (unsigned)samples[kSamples / 2],
           (unsigned)samples[(kSamples * 95) / 100],
           (unsigned)samples[(kSamples * 99) / 100],
           (unsigned)samples[kSamples - 1],
           (unsigned)before.uordblks, (unsigned)before.fordblks);
}

static uint32_t count_objects(lv_obj_t *root)
{
    uint32_t total = 1;
    uint32_t i;

    if (!root) return 0;
    for (i = 0; i < lv_obj_get_child_cnt(root); i++) {
        total += count_objects(lv_obj_get_child(root, i));
    }
    return total;
}

/* U40：连续页面切换不应增长对象树，也不应留下悬空控件。 */
static void test_screen_cycle(smart_home_lvgl_t *ui)
{
    uint32_t before;
    uint32_t after;
    unsigned i;

    assert(ui->screen_home && ui->screen_settings_v9);
    before = count_objects(ui->screen_home) + count_objects(ui->screen_settings_v9);
    for (i = 0; i < 200; i++) {
        lv_screen_load(ui->screen_home);
        lv_timer_handler();
        lv_screen_load(ui->screen_settings_v9);
        lv_timer_handler();
    }
    after = count_objects(ui->screen_home) + count_objects(ui->screen_settings_v9);
    assert(before == after);
    printf("PASS: 200 screen switches keep the object tree stable (%u objects)\n",
           (unsigned)after);
}
static void test_v9_views(void)
{
    smart_home_state_t state;
    smart_home_v9_device_view_t views[SMART_HOME_V9_MAX_VIEWS];
    smart_home_v9_control_t control;
    size_t count;

    smart_home_device_init(&state);
    count = smart_home_v9_collect_views(&state, NULL, views,
                                        SMART_HOME_V9_MAX_VIEWS);
    assert(count == 1);
    assert(views[0].source == SMART_HOME_V9_SOURCE_LOCAL);
    assert(views[0].kind == SMART_HOME_V9_KIND_LIGHT);
    assert((views[0].capabilities & SMART_HOME_V9_CAP_BRIGHTNESS) != 0);
    assert(smart_home_device_add(&state, "bedroom", "Test AC", SMART_HOME_DEVICE_AC, NULL) == AGENT_OK);
    count = smart_home_v9_collect_views(&state, NULL, views, SMART_HOME_V9_MAX_VIEWS);
    assert(count == 2 && views[1].kind == SMART_HOME_V9_KIND_AC);
    assert((views[1].capabilities & SMART_HOME_V9_CAP_TEMPERATURE) != 0);

    count = smart_home_v9_demo_views(views, SMART_HOME_V9_MAX_VIEWS);
    assert(count == 1);
    assert(!strcmp(views[0].name, "吊灯"));
    assert(views[0].source == SMART_HOME_V9_SOURCE_DEMO);

    memset(snapshot, 0, sizeof(snapshot));
    snapshot_count = 1;
    snapshot[0].category = SMART_HOME_MILOCO_CATEGORY_OTHER;
    snapshot[0].online = true;
    snapshot[0].power_on = true;
    snapshot[0].power_valid = true;
    snapshot[0].control_count = 1;
    strcpy(snapshot[0].controls[0].type_name, "wash-mode");
    strcpy(snapshot[0].controls[0].desc, "洗涤模式");
    count = smart_home_v9_collect_views(NULL,
                                        (const smart_home_miloco_t *)snapshot,
                                        views, SMART_HOME_V9_MAX_VIEWS);
    assert(count == 1);
    assert(views[0].kind == SMART_HOME_V9_KIND_WASHER);

    strcpy(snapshot[0].controls[0].type_name, "dock");
    strcpy(snapshot[0].controls[0].desc, "回充");
    count = smart_home_v9_collect_views(NULL,
                                        (const smart_home_miloco_t *)snapshot,
                                        views, SMART_HOME_V9_MAX_VIEWS);
    assert(count == 1);
    assert(views[0].kind == SMART_HOME_V9_KIND_ROBOT);
    snapshot_count = 0;

    smart_home_v9_control_begin(&control, &views[0], 0, 7, 1000);
    assert(control.state == SMART_HOME_V9_CONTROL_QUEUED);
    smart_home_v9_control_result(&control, AGENT_OK);
    assert(control.state == SMART_HOME_V9_CONTROL_WAIT_READBACK);
    views[0].on = 0;
    views[0].state_valid = true;
    smart_home_v9_control_snapshot(&control, &views[0], 1100, 10000);
    assert(control.state == SMART_HOME_V9_CONTROL_SUCCEEDED);

    smart_home_v9_control_begin(&control, &views[0], 0, 8, 1000);
    smart_home_v9_control_result(&control, AGENT_OK);
    views[0].on = 1;
    smart_home_v9_control_snapshot(&control, &views[0], 12000, 10000);
    assert(control.state == SMART_HOME_V9_CONTROL_TIMED_OUT);
    puts("PASS: V9 view mapping and async control readback/timeout");
}

/* The panel harness does not link the full device service; local real
 * control is exercised on target.  The V9 view/control state machine has its
 * own host test below. */
int smart_home_device_service_set_device_control(
    smart_home_device_service_t *service, int device_id, int on, int value,
    int mode, int fan_speed)
{
    (void)service; (void)device_id; (void)on; (void)value; (void)mode;
    (void)fan_speed;
    return AGENT_ERROR_INVALID;
}
int smart_home_device_service_get_device(smart_home_device_service_t *service,
                                          int id, smart_home_device_t *out)
{ (void)service; (void)id; (void)out; return AGENT_ERROR_NOT_FOUND; }

int smart_home_device_service_copy_state(smart_home_device_service_t *s, smart_home_state_t *out)
{ if (!local_state_enabled || !s->state) return AGENT_ERROR_INVALID;
  *out = *s->state; return AGENT_OK; }
/* home_v9 的视图刷新按 revision 门控（96d9dc4）。harness 用递增计数模拟
 * 变化，让每条用例都能走到真实刷新路径。 */
uint32_t smart_home_device_service_revision(smart_home_device_service_t *s)
{ (void)s; return ++local_revision; }
int smart_home_device_service_add_room(smart_home_device_service_t *s, const char *room)
{ (void)s; (void)room; return AGENT_ERROR_INVALID; }
int smart_home_device_service_delete_room(smart_home_device_service_t *s, const char *a, const char *b)
{ (void)s; (void)a; (void)b; return AGENT_ERROR_INVALID; }
int smart_home_device_service_delete_room_with_keys(smart_home_device_service_t *s,
    const char *a, const char *b, const char keys[][SMART_HOME_ROOM_KEY_SIZE], size_t n)
{ (void)s; (void)a; (void)b; (void)keys; (void)n; return AGENT_ERROR_INVALID; }
int smart_home_device_service_assign_room(smart_home_device_service_t *s, const char *key, const char *room)
{ (void)s; (void)key; (void)room; return AGENT_ERROR_INVALID; }
int smart_home_config_state_save(const cJSON *root)
{ (void)root; return AGENT_ERROR_INVALID; }

static void test_room_transaction(void)
{
    smart_home_state_t state;
    smart_home_device_t *device;
    int device_id = 0;

    smart_home_device_init(&state);
    assert(state.room_count == 2);
    assert(smart_home_room_add(&state, "balcony") == AGENT_OK);
    assert(smart_home_device_add(&state, "balcony", "Washer",
                                 SMART_HOME_DEVICE_LIGHT,
                                 &device_id) == AGENT_OK);
    assert(smart_home_room_delete_and_reassign(&state, "balcony",
                                               "living_room") == AGENT_OK);
    assert(smart_home_room_index(&state, "balcony") < 0);
    assert(state.room_count == 2);
    device = smart_home_device_find_by_id(&state, device_id);
    assert(device && !strcmp(device->room, "living_room"));

    assert(smart_home_room_delete_and_reassign(&state, "living_room",
                                               "bedroom") == AGENT_OK);
    assert(state.room_count == 1);
    assert(smart_home_room_delete_and_reassign(&state, "bedroom",
                                               "living_room") ==
           AGENT_ERROR_NOT_FOUND);
    assert(smart_home_room_delete_and_reassign(&state, "bedroom",
                                               "bedroom") ==
           AGENT_ERROR_INVALID);
    puts("PASS: room delete reassigns devices and refuses last/duplicate rooms");
}

int main(void)
{
    test_v9_views();
    test_room_transaction();
    lv_init();
    lv_display_t *display = lv_display_create(1024, 600);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, flush);
    assert(smart_home_lvgl_style_init() == 0);
    smart_home_agent_app_t app = {0};
    smart_home_lvgl_t ui = {0};
    app.miloco = (smart_home_miloco_t *)&snapshot;
    ui.app = &app;
    assert(pthread_mutex_init(&ui.pending_mutex, NULL) == 0);
    test_home(&ui);
    pthread_mutex_destroy(&ui.pending_mutex);
    return 0;
}

/* Exercise actual V9 card callbacks, including cards past the six demo kinds. */
static lv_obj_t *find_v9_carousel(lv_obj_t *root)
{
    if (lv_obj_get_width(root) == 976 && lv_obj_get_height(root) == 256)
        return root;
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_v9_carousel(lv_obj_get_child(root, i));
        if (found) return found;
    }
    return NULL;
}

/* 六类演示卡各自的设备弹窗截图，供二级菜单视觉核对。 */
static void capture_v9_device_modals(smart_home_lvgl_t *ui)
{
    static const char *const keys[6] = {
        "SMART_HOME_V9_MODAL_LIGHT_CAPTURE",
        "SMART_HOME_V9_MODAL_AC_CAPTURE",
        "SMART_HOME_V9_MODAL_WASHER_CAPTURE",
        "SMART_HOME_V9_MODAL_ROBOT_CAPTURE",
        "SMART_HOME_V9_MODAL_CAMERA_CAPTURE",
        "SMART_HOME_V9_MODAL_FAN_CAPTURE",
    };
    lv_obj_t *carousel;
    uint32_t i;
    int opened = 0;

    if (!getenv(keys[0])) return;
    lv_obj_update_layout(ui->screen_home);   /* 刚建完的视口尺寸要等布局才算出来 */
    carousel = find_v9_carousel(ui->screen_home);
    if (!carousel) return;
    for (i = 0; i < lv_obj_get_child_count(carousel); i++) {
        lv_obj_t *card = lv_obj_get_child(carousel, i);

        if (lv_obj_get_width(card) != 176 || lv_obj_get_height(card) != 244)
            continue;
        lv_obj_send_event(card, LV_EVENT_CLICKED, NULL);
        if (lv_obj_get_child_count(ui->screen_home) > 0) {
            lv_obj_t *mask = NULL;
            /* Dropdown lists are screen children too; identify the overlay
             * by its size instead of assuming it is the last child. */
            lv_obj_update_layout(ui->screen_home);
            for (uint32_t n = 0; n < lv_obj_get_child_count(ui->screen_home); n++) {
                lv_obj_t *child = lv_obj_get_child(ui->screen_home, n);
                if (lv_obj_get_width(child) == 1024 &&
                    lv_obj_get_height(child) == 600 &&
                    lv_obj_get_style_bg_opa(child, 0) == LV_OPA_30) mask = child;
            }
            assert(mask);
            lv_obj_update_layout(ui->screen_home);
            capture_to(ui, keys[opened < 6 ? opened : 5]);
            lv_obj_send_event(mask, LV_EVENT_CLICKED, NULL);
        }
        if (++opened >= 6) break;
    }
}
/* 设置页七个分类各导一张，包含未启用语音时的提示。 */
static void capture_v9_settings_categories(smart_home_lvgl_t *ui)
{
    static const char *const keys[7] = {
        "SMART_HOME_V9_CAT_DISPLAY_CAPTURE",
        "SMART_HOME_V9_CAT_NETWORK_CAPTURE",
        "SMART_HOME_V9_CAT_MODEL_CAPTURE",
        "SMART_HOME_V9_CAT_VOICE_CAPTURE",
        "SMART_HOME_V9_CAT_TOOLS_CAPTURE",
        "SMART_HOME_V9_CAT_SERVICES_CAPTURE",
        "SMART_HOME_V9_CAT_HEALTH_CAPTURE",
    };
    int i;

    for (i = 0; i < 7; i++) {
        if (!getenv(keys[i])) continue;
        lv_screen_load(ui->screen_settings_v9);
        if (smart_home_lvgl_v9_settings_select_category(i) != i) return;
        if (i == 4) {
            for (int step = 0; step < 6; ++step) {
                lv_tick_inc(45);
                lv_timer_handler();
            }
        }
        lv_obj_update_layout(ui->screen_settings_v9);
        capture_screen(ui->screen_settings_v9, keys[i]);
        if (i == 4 && getenv("SMART_HOME_V9_TOOLS_DETAIL_CAPTURE")) {
            lv_obj_t *tool = find_v9_label(ui->screen_settings_v9, "get_home_status");
            assert(tool);
            lv_obj_send_event(lv_obj_get_parent(tool), LV_EVENT_CLICKED, NULL);
            capture_screen(ui->screen_settings_v9, "SMART_HOME_V9_TOOLS_DETAIL_CAPTURE");
            lv_obj_t *title = find_v9_label(ui->screen_settings_v9, "工具详情");
            assert(title);
            lv_obj_send_event(lv_obj_get_parent(lv_obj_get_parent(title)), LV_EVENT_CLICKED, NULL);
        }
        if (i == 4 && getenv("SMART_HOME_V9_TOOLS_PAGE_CAPTURE")) {
            lv_obj_t *next = find_v9_label(ui->screen_settings_v9, "下一页");
            assert(next);
            lv_obj_send_event(lv_obj_get_parent(next), LV_EVENT_CLICKED, NULL);
            for (int step = 0; step < 4; ++step) {
                lv_tick_inc(45);
                lv_timer_handler();
            }
            capture_screen(ui->screen_settings_v9, "SMART_HOME_V9_TOOLS_PAGE_CAPTURE");
        }
    }
    (void)smart_home_lvgl_v9_settings_select_category(0);
}
static lv_obj_t *find_v9_dropdown(lv_obj_t *root)
{
    if (!root) return NULL;
    if (lv_obj_check_type(root, &lv_dropdown_class)) return root;
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_v9_dropdown(lv_obj_get_child(root, i));
        if (found) return found;
    }
    return NULL;
}

/* 模型服务页的下拉菜单展开态，用于与设计 23-model-menu 比对。 */
static void capture_v9_model_menu(smart_home_lvgl_t *ui)
{
    const char *path = getenv("SMART_HOME_V9_MODEL_MENU_CAPTURE");
    lv_obj_t *dropdown;

    if (!path || !ui->screen_settings_v9) return;
    if (smart_home_lvgl_v9_settings_select_category(2) != 2) return;
    lv_obj_update_layout(ui->screen_settings_v9);
    dropdown = find_v9_dropdown(ui->screen_settings_v9);
    if (!dropdown) return;
    {
        lv_obj_t *list = lv_dropdown_get_list(dropdown);
        lv_color_t sel;

        printf("DBG dropdown=%p list=%p opts='%s'\n", (void *)dropdown,
               (void *)list, lv_dropdown_get_options(dropdown));
        if (list) {
            sel = lv_obj_get_style_bg_color(list, LV_PART_SELECTED);
            printf("DBG list sel_bg=#%02X%02X%02X w=%d h=%d count=%u\n",
                   sel.red, sel.green, sel.blue,
                   (int)lv_obj_get_width(list), (int)lv_obj_get_height(list),
                   (unsigned)lv_obj_get_child_count(list));
        }
    }
    lv_dropdown_open(dropdown);
    {
        lv_obj_t *list = lv_dropdown_get_list(dropdown);

        printf("DBG opened list x=%d y=%d w=%d h=%d\n",
               (int)lv_obj_get_x(list), (int)lv_obj_get_y(list),
               (int)lv_obj_get_width(list), (int)lv_obj_get_height(list));
    }
    lv_obj_update_layout(ui->screen_settings_v9);
    lv_refr_now(NULL);
    capture_screen(ui->screen_settings_v9, "SMART_HOME_V9_MODEL_MENU_CAPTURE");
    lv_dropdown_close(dropdown);
    /* 下拉列表挂在 screen 下且关闭后仍存在；把它移回底部，避免后续
     * “最后一个子对象是底栏”的断言把它当成底栏。 */
    if (lv_dropdown_get_list(dropdown)) {
        lv_obj_move_to_index(lv_dropdown_get_list(dropdown), 0);
    }
    lv_obj_update_layout(ui->screen_settings_v9);
    (void)smart_home_lvgl_v9_settings_select_category(0);
}
/* 米家扫码弹窗截图（SMART_HOME_V9_MI_QR_CAPTURE），供与设计 34-mi-qr 比对。 */
static void capture_v9_mi_qr(smart_home_lvgl_t *ui)
{
    const char *path = getenv("SMART_HOME_V9_MI_QR_CAPTURE");

    if (!path || !ui || !ui->screen_settings_v9) return;
    if (smart_home_lvgl_v9_settings_select_category(5) != 5) return;
    lv_obj_update_layout(ui->screen_settings_v9);
    lv_screen_load(ui->screen_settings_v9);
    smart_home_lvgl_open_miloco_bind(ui);
    lv_obj_update_layout(ui->screen_settings_v9);
    lv_refr_now(NULL);
    capture_screen(ui->screen_settings_v9, "SMART_HOME_V9_MI_QR_CAPTURE");
    smart_home_lvgl_miloco_bind_close(ui);
    lv_obj_update_layout(ui->screen_settings_v9);
    (void)smart_home_lvgl_v9_settings_select_category(0);
}

/* 场景切换器：按钮默认显示「回家模式」，点开后菜单有四项且当前项高亮，
 * 选中「观影模式」后按钮文案/图标同步更新，并把 movie 交给场景执行入口。 */
static void test_v9_scene_switcher(smart_home_lvgl_t *ui)
{
    lv_obj_t *button;
    int found = 0;

    /* 找到房间行右侧的场景按钮：位置固定，尺寸 156x48。 */
    for (uint32_t i = 0; i < lv_obj_get_child_count(ui->screen_home); i++) {
        lv_obj_t *child = lv_obj_get_child(ui->screen_home, i);

        if (lv_obj_get_x(child) == 844 && lv_obj_get_y(child) == 206 &&
            lv_obj_get_width(child) == 156 && lv_obj_get_height(child) == 48) {
            button = child;
            found = 1;
            break;
        }
    }
    assert(found);
    lv_screen_load(ui->screen_home);
    lv_obj_update_layout(ui->screen_home);

    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, read_pointer);

    /* 默认态：文案为回家模式（子对象顺序：图标、文字、折线箭头）。 */
    lv_obj_t *label = NULL;
    for (uint32_t i = 0; i < lv_obj_get_child_count(button); i++) {
        lv_obj_t *child = lv_obj_get_child(button, i);

        if (lv_obj_check_type(child, &lv_label_class)) {
            label = child;
            break;
        }
    }
    assert(label && !strcmp(lv_label_get_text(label), "回家模式"));

    /* 展开后菜单应出现四项，且当前项用选中底色。 */
    touch(input, button, LV_INDEV_STATE_PRESSED);
    touch(input, button, LV_INDEV_STATE_RELEASED);
    lv_obj_update_layout(ui->screen_home);
    lv_obj_update_layout(lv_layer_top());
    /* 菜单挂在顶层图层（首页最后一个子对象必须留给底部导航栏）。 */
    lv_obj_t *menu = NULL;
    for (uint32_t i = 0; i < lv_obj_get_child_count(lv_layer_top()); i++) {
        lv_obj_t *child = lv_obj_get_child(lv_layer_top(), i);

        if (lv_obj_get_width(child) == 156 &&
            lv_obj_get_child_count(child) == 4) {
            menu = child;
            break;
        }
    }
    assert(menu && !lv_obj_has_flag(menu, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_child_count(menu) == 4);
    capture_screen(ui->screen_home, "SMART_HOME_V9_SCENE_MENU_CAPTURE");
    /* 默认选中第一项，且第一项的底色是选中色而不是普通卡片色。 */
    assert(!strcmp(lv_label_get_text(
               lv_obj_get_child(lv_obj_get_child(menu, 0), 1)), "回家模式"));
    assert(lv_obj_get_style_bg_opa(lv_obj_get_child(menu, 0), 0) == LV_OPA_COVER);

    int runs_before = scene_runs;
    /* 点击第二项「观影模式」。 */
    lv_obj_t *item = lv_obj_get_child(menu, 1);
    touch(input, item, LV_INDEV_STATE_PRESSED);
    touch(input, item, LV_INDEV_STATE_RELEASED);

    assert(scene_runs == runs_before + 1);
    assert(!strcmp(last_scene, "movie"));
    assert(!strcmp(lv_label_get_text(label), "观影模式"));
    /* 菜单点选后隐藏，但不销毁。 */
    assert(lv_obj_has_flag(menu, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_child(menu, 1) != NULL);
    lv_indev_delete(input);

    /* 场景执行成功会弹出结果框：点一次遮罩关闭它，与用户操作一致，
     * 也顺带验证弹窗关闭后首页只剩底部导航栏这一约定。 */
    {
        lv_obj_t *mask = lv_obj_get_child(ui->screen_home, -1);

        lv_obj_update_layout(ui->screen_home);
        assert(lv_obj_get_width(mask) == 1024);
        assert(lv_obj_get_height(mask) == 600);
        lv_obj_send_event(mask, LV_EVENT_CLICKED, NULL);
        lv_obj_update_layout(ui->screen_home);
        assert(lv_obj_get_height(lv_obj_get_child(ui->screen_home, -1)) == 64);
    }
    puts("PASS: scene switcher opens a four-item menu and runs the chosen scene");
}

static void test_v9_cards(smart_home_lvgl_t *ui)
{
    lv_obj_t *carousel;
    lv_obj_t *last_card = NULL;
    unsigned cards = 0;
    unsigned before;
    int submitted;

    memset(snapshot, 0, sizeof(snapshot));
    snapshot_count = 12;
    for (size_t i = 0; i < snapshot_count; i++) {
        snprintf(snapshot[i].did, sizeof(snapshot[i].did), "v9-light-%u",
                 (unsigned)i);
        snprintf(snapshot[i].name, sizeof(snapshot[i].name), "灯具-%u",
                 (unsigned)i);
        strcpy(snapshot[i].room, "living_room");
        snapshot[i].category = SMART_HOME_MILOCO_CATEGORY_LIGHT;
        snapshot[i].online = true;
        snapshot[i].controllable = true;
        snapshot[i].power_on = true;
        snapshot[i].power_valid = true;
    }
    smart_home_lvgl_v9_refresh_views(ui);
    lv_obj_update_layout(ui->screen_home);
    assert(smart_home_lvgl_v9_visible_card_count() == 12);
    carousel = find_v9_carousel(ui->screen_home);
    assert(carousel);
    before = lv_obj_get_child_count(ui->screen_home);
    for (uint32_t i = 0; i < lv_obj_get_child_count(carousel); i++) {
        lv_obj_t *card = lv_obj_get_child(carousel, i);
        lv_obj_t *mask;
        char expected[40];
        if (lv_obj_get_width(card) != 176 || lv_obj_get_height(card) != 244)
            continue;
        snprintf(expected, sizeof(expected), "灯具-%u", cards++);
        assert(!strcmp(lv_label_get_text(lv_obj_get_child(card, 1)), expected));
        assert(!strcmp(lv_label_get_text(lv_obj_get_child(card, 2)), ""));
        lv_obj_send_event(card, LV_EVENT_CLICKED, NULL);
        assert(lv_obj_get_child_count(ui->screen_home) == before + 1);
        mask = lv_obj_get_child(ui->screen_home, -1);
        lv_obj_send_event(mask, LV_EVENT_CLICKED, NULL);
        assert(lv_obj_get_child_count(ui->screen_home) == before);
        last_card = card;
    }
    assert(cards == 12 && last_card);
    submitted = submissions;
    lv_obj_t *sw = lv_obj_get_child(last_card, -1);
    assert(lv_obj_get_width(sw) == 54 && lv_obj_get_height(sw) == 30);
    assert(lv_obj_get_style_pad_top(sw, LV_PART_KNOB) == -4);
    lv_obj_remove_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
    assert(submissions == submitted + 1);
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(last_card, 2)), "等待确认"));
    snapshot[11].power_on = false;
    snapshot[11].power_valid = true;
    smart_home_lvgl_v9_refresh_views(ui);
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(last_card, 2)), ""));
    snapshot[11].online = false;
    smart_home_lvgl_v9_refresh_views(ui);
    assert(find_v9_label(last_card, "离线"));
    snapshot_count = 0;
    smart_home_lvgl_v9_refresh_views(ui);
    puts("PASS: all 12 V9 card modals, real switches and readback");
}

static lv_obj_t *find_v9_input(lv_obj_t *root, const char *placeholder)
{
    if (lv_obj_check_type(root, &lv_textarea_class) &&
        !strcmp(lv_textarea_get_placeholder_text(root), placeholder)) return root;
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_v9_input(lv_obj_get_child(root, i), placeholder);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_v9_label(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) &&
        strcmp(lv_label_get_text(root), text) == 0) return root;
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_v9_label(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}

static bool v9_label_contains(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) &&
        strstr(lv_label_get_text(root), text)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        if (v9_label_contains(lv_obj_get_child(root, i), text)) return true;
    }
    return false;
}

static void wait_for_network_probe(smart_home_lvgl_t *ui)
{
    const struct timespec pause = {.tv_nsec = 1000000};

    for (unsigned i = 0; i < 3000; i++) {
        lv_tick_inc(10);
        lv_timer_handler();
        if (ui->app->system_status.network_status.online) return;
        nanosleep(&pause, NULL);
    }
    assert(!"health refresh did not complete");
}

static void test_v9_room_catalog(smart_home_lvgl_t *ui)
{
    snapshot_count = 0;
    smart_home_device_init(&ui->app->device_state);
    ui->app->device_service.state = &ui->app->device_state;
    local_state_enabled = 1;
    smart_home_lvgl_v9_refresh_views(ui);
    capture_screen(ui->screen_home, "SMART_HOME_ROOMS_TWO_CAPTURE");
    snapshot_count = 12;
    memset(ui->app->device_state.devices, 0, sizeof ui->app->device_state.devices);
    assert(!smart_home_room_add(&ui->app->device_state, "书房"));
    assert(!smart_home_device_assign_room(&ui->app->device_state, "milo:v9-light-0", "书房"));
    ui->app->device_service.state = &ui->app->device_state;
    local_state_enabled = 1;
    smart_home_lvgl_v9_refresh_views(ui);
    assert(smart_home_lvgl_v9_select_room(3) == 1);
    lv_obj_t *bedroom = find_v9_label(ui->screen_home, "卧室");
    assert(bedroom);
    lv_obj_send_event(lv_obj_get_parent(bedroom), LV_EVENT_CLICKED, NULL);
    assert(smart_home_lvgl_v9_visible_card_count() == 0);
    lv_obj_t *add_label = find_v9_label(ui->screen_home, "+");
    assert(add_label);
    lv_obj_t *add = lv_obj_get_parent(add_label);
    lv_obj_t *strip = lv_obj_get_parent(add);
    /* Room labels, including the add button, share one horizontal strip. */
    lv_obj_t *last = lv_obj_get_child(strip, lv_obj_get_child_count(strip) - 2);
    lv_obj_update_layout(strip);
    assert(lv_obj_get_x(add) == lv_obj_get_x(last) + lv_obj_get_width(last) + 6);
    int old_x = lv_obj_get_x(add);
    assert(!smart_home_room_add(&ui->app->device_state, "厕所"));
    local_revision++;
    smart_home_lvgl_v9_refresh_views(ui);
    add = lv_obj_get_parent(find_v9_label(ui->screen_home, "+"));
    strip = lv_obj_get_parent(add);
    last = lv_obj_get_child(strip, lv_obj_get_child_count(strip) - 2);
    lv_obj_update_layout(strip);
    assert(find_v9_label(last, "厕所"));
    assert(lv_obj_get_x(add) == old_x + 84);
    assert(lv_obj_get_x(add) == lv_obj_get_x(last) + lv_obj_get_width(last) + 6);
    capture_screen(ui->screen_home, "SMART_HOME_ROOMS_ADDED_CAPTURE");
    ui->app->device_service.mutex_initialized = 1;
    lv_obj_send_event(add, LV_EVENT_CLICKED, NULL);
    assert(find_v9_label(ui->screen_home, "房间管理"));
    smart_home_lvgl_room_dialog_close();
    ui->app->device_service.mutex_initialized = 0;
    assert(!smart_home_room_delete_and_reassign(&ui->app->device_state, "厕所", "living_room"));
    local_revision++;
    smart_home_lvgl_v9_refresh_views(ui);
    add = lv_obj_get_parent(find_v9_label(ui->screen_home, "+"));
    lv_obj_update_layout(ui->screen_home);
    assert(lv_obj_get_x(add) == old_x);
    smart_home_lvgl_v9_select_room(3);
    assert(!smart_home_room_delete_and_reassign(&ui->app->device_state, "书房", "living_room"));
    smart_home_lvgl_v9_refresh_views(ui);
    assert(!find_v9_label(ui->screen_home, "书房"));
    assert(smart_home_lvgl_v9_visible_card_count() == 12);
    snapshot_count = 0;
    memset(ui->app->device_state.devices, 0, sizeof ui->app->device_state.devices);
    ui->app->device_service.mutex_initialized = 1;
    smart_home_lvgl_v9_refresh_views(ui);
    assert(smart_home_lvgl_v9_visible_card_count() == 0);
    ui->app->device_service.mutex_initialized = 0;
    local_state_enabled = 0;
    snapshot_count = 0;
    ui->app->device_service.state = NULL;
    smart_home_lvgl_v9_refresh_views(ui);
    puts("PASS: dynamic room catalog, actual filter click, remote assignment and deleted filter reset");
}

static void test_v9_settings_clicks(smart_home_lvgl_t *ui)
{
    lv_obj_t *caption = find_v9_label(ui->screen_settings_v9, "显示与声音");
    lv_obj_t *key = find_v9_input(ui->screen_settings_v9,
                                 "留空沿用已保存密钥");
    assert(caption && key);
    lv_obj_t *left = lv_obj_get_parent(lv_obj_get_parent(caption));
    lv_obj_t *right = lv_obj_get_parent(lv_obj_get_parent(key));
    assert(lv_obj_get_child_count(left) == 7);
    assert(lv_obj_get_child_count(right) == 7);
    assert(find_v9_label(ui->screen_settings_v9, "语音服务未开启"));
    lv_screen_load(ui->screen_settings_v9);
    lv_obj_update_layout(ui->screen_settings_v9);
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, read_pointer);
    for (int area = 0; area < 3; area++) {
        for (int i = 0; i < 7; i++) {
            lv_obj_t *button = lv_obj_get_child(left, i);
            lv_obj_t *target = area < 2 ? lv_obj_get_child(button, area) : button;
            touch(input, target, LV_INDEV_STATE_PRESSED);
            touch(input, target, LV_INDEV_STATE_RELEASED);
            for (int j = 0; j < 7; j++) {
                assert(lv_obj_has_flag(lv_obj_get_child(right, j),
                                       LV_OBJ_FLAG_HIDDEN) == (j != i));
                assert(lv_obj_get_style_bg_opa(lv_obj_get_child(left, j), 0) ==
                       (j == i ? LV_OPA_COVER : LV_OPA_TRANSP));
            }
        }
    }

    ui->app->system_status.network_status.online = true;
    ui->app->system_status.network_status.ip_status = SMART_HOME_NETWORK_OK;
    ui->app->system_status.model_status = AGENT_OK;
    ui->app->system_status.tools_status = AGENT_OK;
    ui->app->system_status.last_error[0] = '\0';
    ui->app->system_status.network_status.online = true;
    gateway_online = true;
    snapshot_count = 6;
    assert(smart_home_lvgl_v9_settings_select_category(5) == 5);
    assert(v9_label_contains(ui->screen_settings_v9,
                             "最后检查 --:--:--"));

    lv_obj_t *details = find_v9_label(ui->screen_settings_v9, "服务详情");
    assert(details);
    lv_obj_send_event(lv_obj_get_parent(details), LV_EVENT_CLICKED, NULL);
    /* 「语音服务」插在第 3 位后，工具权限后移到 4、设备与服务到 5。
     * 「服务详情」跳的是 V9_CAT_SERVICES（设备与服务），所以高亮在 5。 */
    assert(lv_obj_get_style_bg_opa(lv_obj_get_child(left, 5), 0) == LV_OPA_COVER);
    assert(lv_obj_get_style_bg_opa(lv_obj_get_child(left, 6), 0) == LV_OPA_TRANSP);
    assert(smart_home_lvgl_v9_settings_select_category(5) == 5);

    lv_obj_t *refresh_label = find_v9_label(ui->screen_settings_v9, "刷新状态");
    assert(refresh_label);
    lv_obj_t *refresh = lv_obj_get_parent(refresh_label);
    network_probe_calls = 0;
    ui->app->system_status.network_status.online = false;
    lv_obj_send_event(refresh, LV_EVENT_CLICKED, NULL);
    wait_for_network_probe(ui);
    assert(network_probe_calls == 1);
    assert(!lv_obj_has_state(refresh, LV_STATE_DISABLED));
    assert(find_v9_label(ui->screen_settings_v9, "核心服务正常"));
    assert(find_v9_label(ui->screen_settings_v9, "已连接 · 6 台设备"));
    assert(find_v9_label(ui->screen_settings_v9, "配置已应用"));
    assert(find_v9_label(ui->screen_settings_v9, "已就绪"));
    assert(find_v9_label(ui->screen_settings_v9, "已连接互联网"));
    assert(v9_label_contains(ui->screen_settings_v9, "最后检查"));

    snprintf(ui->app->system_status.last_error,
             sizeof(ui->app->system_status.last_error), "模型密钥缺失");
    ui->app->system_status.model_status = AGENT_ERROR_AUTH;
    ui->app->system_status.network_status.online = false;
    lv_obj_send_event(refresh, LV_EVENT_CLICKED, NULL);
    wait_for_network_probe(ui);
    assert(network_probe_calls == 2);
    assert(find_v9_label(ui->screen_settings_v9, "有服务需要处理"));
    assert(find_v9_label(ui->screen_settings_v9, "模型密钥缺失"));
    ui->app->system_status.model_status = AGENT_OK;
    ui->app->system_status.last_error[0] = '\0';
    ui->app->system_status.network_status.online = false;
    lv_obj_send_event(refresh, LV_EVENT_CLICKED, NULL);
    wait_for_network_probe(ui);
    assert(network_probe_calls == 3);
    assert(find_v9_label(ui->screen_settings_v9, "核心服务正常"));

    lv_indev_delete(input);
    puts("PASS: settings navigation and system health status/actions");
}


#ifdef CONFIG_SMART_HOME_ONBOARDING
/* 首次配置引导：三项未完成时应弹出浮层，跳转设置页对应分类，点「稍后再说」
 * 后不再重复弹出。宿主 harness 不链接真实 Wi-Fi / 网关，用桩控制状态。 */
static void test_v9_onboarding(smart_home_lvgl_t *ui)
{
    smart_home_onboarding_status_t status;
    lv_obj_t *modal;

    /* 三项都就绪：状态为 0，不弹。 */
    smart_home_lvgl_onboarding_reset();
    smart_home_lvgl_onboarding_status(ui, &status);
    assert(status.wifi_ready);
    assert(status.miloco_ready);
    assert(status.model_ready);
    assert(status.pending == 0);
    assert(!smart_home_lvgl_onboarding_should_present(ui));
    smart_home_lvgl_onboarding_present(ui);
    assert(!smart_home_lvgl_onboarding_visible());

    /* 三项都未配：pending=3，应弹。 */
    ui->app->system_status.network_status.online = false;
    gateway_online = false;
    ui->app->system_status.model_status = SMART_HOME_STATUS_UNKNOWN;
    smart_home_lvgl_onboarding_reset();
    smart_home_lvgl_onboarding_status(ui, &status);
    assert(!status.wifi_ready && !status.miloco_ready && !status.model_ready);
    assert(status.pending == 3);
    assert(smart_home_lvgl_onboarding_should_present(ui));

    smart_home_lvgl_onboarding_present(ui);
    assert(smart_home_lvgl_onboarding_visible());
    lv_obj_update_layout(ui->screen_home);
    capture_screen(ui->screen_home, "SMART_HOME_ONBOARDING_CAPTURE");

    /* 浮层遮罩尺寸应覆盖整屏，面板在 (92,62) 840x476。 */
    modal = lv_obj_get_child(ui->screen_home, -1);
    assert(lv_obj_get_width(modal) == 1024 && lv_obj_get_height(modal) == 600);
    lv_obj_t *panel = lv_obj_get_child(modal, 0);
    assert(lv_obj_get_x(panel) == 92 && lv_obj_get_y(panel) == 62);
    assert(lv_obj_get_width(panel) == 840 && lv_obj_get_height(panel) == 476);

    /* 面板内应有三行，第二行点击跳到「设备与服务」。 */
    lv_obj_t *rows = NULL;
    for (uint32_t i = 0; i < lv_obj_get_child_count(panel); i++) {
        lv_obj_t *c = lv_obj_get_child(panel, i);

        if (lv_obj_get_child_count(c) == 3 &&
            lv_obj_get_width(c) == 840 - 64) {
            rows = c;
            break;
        }
    }
    assert(rows);
    lv_obj_t *row = lv_obj_get_child(rows, 1);
    assert(lv_obj_get_width(row) == 840 - 64);
    assert(lv_obj_get_height(row) == 84);
    lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);
    assert(!smart_home_lvgl_onboarding_visible());
    assert(ui->active_tab == SMART_HOME_TAB_SETTINGS);

    /* 再弹一次，点「稍后再说」后本次运行不再提示。 */
    smart_home_lvgl_load_tab(ui, SMART_HOME_TAB_HOME);
    smart_home_lvgl_onboarding_reset();
    smart_home_lvgl_onboarding_present(ui);
    assert(smart_home_lvgl_onboarding_visible());
    lv_obj_update_layout(ui->screen_home);
    modal = lv_obj_get_child(ui->screen_home, -1);
    panel = lv_obj_get_child(modal, 0);
    lv_obj_t *later = lv_obj_get_child(panel, lv_obj_get_child_count(panel) - 1);
    assert(!lv_obj_has_flag(panel, LV_OBJ_FLAG_SCROLLABLE));
    lv_area_t before_press, after_press;
    lv_obj_get_coords(panel, &before_press);
    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_pointer);
    touch(pointer, later, LV_INDEV_STATE_PRESSED);
    lv_obj_update_layout(ui->screen_home);
    lv_obj_get_coords(panel, &after_press);
    assert(before_press.x1 == after_press.x1 && before_press.y1 == after_press.y1);
    assert(lv_obj_get_scroll_x(panel) == 0 && lv_obj_get_scroll_y(panel) == 0);
    assert(lv_obj_get_scroll_bottom(panel) <= 0);
    touch(pointer, later, LV_INDEV_STATE_RELEASED);
    lv_indev_delete(pointer);
    assert(!smart_home_lvgl_onboarding_visible());
    assert(!smart_home_lvgl_onboarding_should_present(ui));

    /* 复位测试环境，避免影响后续用例。 */
    ui->app->system_status.network_status.online = true;
    gateway_online = true;
    ui->app->system_status.model_status = AGENT_OK;
    smart_home_lvgl_onboarding_reset();
    smart_home_lvgl_load_tab(ui, SMART_HOME_TAB_HOME);
    puts("PASS: onboarding overlay shows pending items, jumps to settings and honours skip");
}
#endif

static void test_v9_shell(smart_home_lvgl_t *ui)
{
    lv_obj_t *screens[] = {ui->screen_home, ui->screen_chat, ui->screen_settings_v9};
    for (int tab = 0; tab < 3; tab++) {
        lv_obj_t *bar = lv_obj_get_child(screens[tab], -1);
        lv_obj_t *row = lv_obj_get_child(bar, 0);
        lv_area_t bounds;
        lv_obj_update_layout(screens[tab]);
        lv_obj_get_coords(bar, &bounds);
        assert(bounds.x1 == 0 && bounds.y1 == 536);
        assert(bounds.x2 == 1023 && bounds.y2 == 599);
        assert(!lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_get_style_bg_opa(bar, 0) == LV_OPA_COVER);
        assert(lv_obj_get_child_count(row) == 3);
        lv_screen_load(screens[tab]);
        lv_refr_now(NULL);
        /* Assert a rendered selected pill, not only an allocated object. */
        assert(pixels[552 * 1024 + 172 + tab * 248] ==
               lv_color_to_u16(SMART_HOME_UI_COLOR_SURFACE_ON));
        for (int i = 0; i < 3; i++) {
            lv_obj_t *button = lv_obj_get_child(row, i);
            lv_obj_t *content = lv_obj_get_child(button, 0);
            lv_obj_t *icon = lv_obj_get_child(content, 0);
            const lv_image_dsc_t *source = lv_image_get_src(icon);
            assert(source->header.cf == LV_COLOR_FORMAT_ARGB8888);
            assert(lv_obj_get_style_image_recolor_opa(icon, 0) == LV_OPA_TRANSP);
            assert(lv_obj_get_width(button) == 216 && lv_obj_get_height(button) == 52);
            lv_obj_send_event(content, LV_EVENT_CLICKED, NULL);
            assert(lv_screen_active() == screens[i]);
            assert(ui->active_tab == i);
        }
    }
    lv_area_t key_bounds, parent_bounds;
    lv_obj_t *key = find_v9_input(ui->screen_settings_v9,
                                 "留空沿用已保存密钥");
    assert(key);
    lv_obj_get_coords(key, &key_bounds);
    lv_obj_get_content_coords(lv_obj_get_parent(key), &parent_bounds);
    assert(key_bounds.x1 == 272 && key_bounds.y1 == 282);
    assert(lv_obj_get_width(key) == 704 && lv_obj_get_height(key) == 44);
    assert(key_bounds.x2 <= parent_bounds.x2 && key_bounds.y2 <= parent_bounds.y2);
    lv_area_t input;
    lv_obj_get_coords(ui->chat_input_bar, &input);
    assert(input.x1 == 24 && input.y1 == 464);
    assert(lv_obj_get_width(ui->chat_input_bar) == 976);
    assert(lv_obj_get_height(ui->chat_input_bar) == 56);
    assert(lv_obj_get_width(ui->chat_input) == 704);
    assert(lv_obj_get_style_text_font(ui->chat_input, 0) == smart_home_lvgl_font(17));
    assert(!find_v9_label(ui->screen_chat, "聊聊日常，也帮你照顾家"));
    assert(!find_v9_label(ui->screen_settings_v9, "连接家庭，按习惯调整"));
    lv_area_t list_bounds;
    lv_obj_get_coords(ui->chat_list, &list_bounds);
    assert(list_bounds.y1 == 124 && lv_obj_get_height(ui->chat_list) == 304);
    lv_obj_t *settings_content = lv_obj_get_parent(lv_obj_get_parent(key));
    lv_obj_get_coords(settings_content, &parent_bounds);
    assert(parent_bounds.y1 == 112 && lv_obj_get_height(settings_content) == 408);

    /* Every page shares the same compact status cluster and fallback clock. */
    smart_home_lvgl_update_clock_at(ui, 0);
    for (int i = 0; i < ui->topbar_clock_count; i++) {
        lv_obj_t *clock = ui->topbar_clock_labels[i];
        lv_obj_t *top = lv_obj_get_parent(clock);
        lv_area_t clock_bounds, previous;
        lv_obj_update_layout(top);
        lv_obj_get_coords(clock, &clock_bounds);
        assert(!strcmp(lv_label_get_text(clock), "12:00"));
        bool home = lv_obj_get_parent(top) == ui->screen_home;
        assert(clock_bounds.x2 == 1007);
        assert(lv_obj_get_width(clock) == (home ? 80 : 56));
        assert(clock_bounds.y1 == (home ? 6 : 16));
        assert(lv_obj_get_style_text_font(clock, 0) == smart_home_lvgl_font(home ? 26 : 18));
        for (int j = 0; j < 4; j++) {
            lv_obj_t *icon = lv_obj_get_child(top, 3 + j);
            lv_area_t icon_bounds;
            lv_obj_get_coords(icon, &icon_bounds);
            assert(lv_obj_get_width(icon) == 20);
            if (j > 0) assert(icon_bounds.x1 - previous.x2 - 1 == 12);
            previous = icon_bounds;
        }
        assert(clock_bounds.x1 - previous.x2 - 1 == 12);
    }
    weather.valid = false;
    smart_home_lvgl_refresh_home(ui);
    assert(!weather.valid);
    assert(!strcmp(lv_label_get_text(ui->home_weather_condition_label), "晴天"));
    /* 图标尺寸跟随徽标实际宽度，不写死 32。 */
    {
        lv_obj_t *glyph = lv_obj_get_child(ui->home_weather_badge, 0);
        const lv_image_dsc_t *src = lv_image_get_src(glyph);

        lv_area_t badge_coords;
        lv_area_t glyph_coords;

        lv_obj_get_coords(ui->home_weather_badge, &badge_coords);
        lv_obj_get_coords(glyph, &glyph_coords);
        /* 天气徽标 128px：图标必须同为 128px 且铺满（STRETCH），
         * 否则 AUTO_TRANSFORM 会按原图居中裁切，只画出中间一小块。
         */
        assert(lv_obj_get_width(ui->home_weather_badge) == 128);
        /* 水平位置：紧贴温度块右侧（设计稿小太阳同位），不是贴右边缘。 */
        assert(lv_obj_get_x(ui->home_weather_badge) == 350);
        assert(lv_obj_get_y(ui->home_weather_badge) == 58);
        assert(src && src->header.w == 128);
        assert(badge_coords.x1 == glyph_coords.x1 &&
               badge_coords.y1 == glyph_coords.y1 &&
               badge_coords.x2 == glyph_coords.x2 &&
               badge_coords.y2 == glyph_coords.y2);
        assert(lv_image_get_inner_align(glyph) == LV_IMAGE_ALIGN_STRETCH);
        assert(src == smart_home_lvgl_png_icon_get(
                    "asset:sun", lv_obj_get_width(ui->home_weather_badge)));
    }
    weather.valid = true;
    smart_home_lvgl_refresh_home(ui);
    assert(!strcmp(lv_label_get_text(ui->home_weather_condition_label), "小雨"));
    assert(lv_image_get_src(lv_obj_get_child(ui->home_weather_badge, 0)) ==
           smart_home_lvgl_png_icon_get(
               "asset:cloud-rain", lv_obj_get_width(ui->home_weather_badge)));
#ifdef CONFIG_SMART_HOME_ONBOARDING
    test_v9_onboarding(ui);
#endif
    puts("PASS: all three real navigation bars, pixel colors, tab clicks and chat input geometry");
}
