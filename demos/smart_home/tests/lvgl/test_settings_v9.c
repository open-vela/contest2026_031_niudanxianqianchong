/* Run the real V9 callbacks with controlled, blocking service doubles. */
#include "smart_home_lvgl_settings_v9.c"
#include <assert.h>
#include <time.h>

void *smart_home_bulk_alloc(size_t size) { return malloc(size); }
void smart_home_bulk_free(void *ptr) { free(ptr); }

static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static struct {
    int entered;
    int released;
    int done;
    int result;
    pthread_t thread;
} gates[2];
static int mcp_calls;
static smart_home_mcp_runtime_config_t applied_mcp;
static smart_home_model_config_t applied_model;
static unsigned network_probe_calls;
static const smart_home_llm_backend_preset_t preset = {
    .id = "test", .name = "测试服务", .host = "example.invalid",
    .path = "/chat", .port = "443", .default_model = "test-model",
    .default_timeout_ms = 30000
};

size_t smart_home_llm_backend_count(void) { return 1; }
const smart_home_llm_backend_preset_t *smart_home_llm_backend_get(size_t index)
{ return index == 0 ? &preset : NULL; }
int smart_home_llm_backend_index_for_config(const smart_home_model_config_t *config)
{ (void)config; return 0; }
int smart_home_model_config_from_preset(smart_home_model_config_t *config,
    const smart_home_llm_backend_preset_t *value, int keep_key)
{
    (void)keep_key;
    memset(config, 0, sizeof(*config));
    snprintf(config->backend_id, sizeof(config->backend_id), "%s", value->id);
    return AGENT_OK;
}

static void wait_entered(int index)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    pthread_mutex_lock(&gate_mutex);
    while (!gates[index].entered)
        assert(pthread_cond_timedwait(&gate_cond, &gate_mutex, &deadline) == 0);
    pthread_mutex_unlock(&gate_mutex);
}

static int block_service(int index)
{
    int result;
    pthread_mutex_lock(&gate_mutex);
    gates[index].entered++;
    gates[index].thread = pthread_self();
    pthread_cond_broadcast(&gate_cond);
    while (!gates[index].released) pthread_cond_wait(&gate_cond, &gate_mutex);
    gates[index].done = 1;
    result = gates[index].result;
    pthread_mutex_unlock(&gate_mutex);
    return result;
}

static void release_service(int index)
{
    pthread_mutex_lock(&gate_mutex);
    gates[index].released = 1;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_mutex);
}

int smart_home_network_connect_credentials(smart_home_network_status_t *status,
                                           const char *ssid, const char *password)
{
    assert(!strcmp(ssid, "test-ssid"));
    assert(!strcmp(password, "test-password"));
    memset(status, 0, sizeof(*status));
    status->online = true;
    return block_service(0);
}

int smart_home_network_probe(smart_home_network_status_t *status)
{
    assert(status);
    network_probe_calls++;
    status->ip_status = SMART_HOME_NETWORK_OK;
    status->dns_status = SMART_HOME_NETWORK_OK;
    status->online = true;
    return SMART_HOME_NETWORK_OK;
}

bool smart_home_miloco_get_config(const smart_home_miloco_t *service,
                                  smart_home_miloco_config_t *config)
{
    (void)service;
    if (config) memset(config, 0, sizeof(*config));
    return false;
}

bool smart_home_miloco_reachable(const smart_home_miloco_t *service)
{ (void)service; return false; }

bool smart_home_miloco_bound(const smart_home_miloco_t *service)
{ (void)service; return false; }

size_t smart_home_miloco_list(const smart_home_miloco_t *service,
                              smart_home_miloco_device_t *devices,
                              size_t capacity, uint32_t *revision)
{
    (void)service;
    (void)devices;
    (void)capacity;
    if (revision) *revision = 0;
    return 0;
}

int smart_home_agent_app_apply_model_config(smart_home_agent_app_t *app,
                                           const smart_home_model_config_t *config)
{
    int result = block_service(1);
    pthread_mutex_lock(&app->agent_mutex);
    applied_model = *config;
    pthread_mutex_unlock(&app->agent_mutex);
    return result;
}

void smart_home_lvgl_refresh_network_indicators(smart_home_lvgl_t *ui)
{ (void)ui; }

/* 米家弹窗只用来验证键盘层级，保存路径不在本用例范围内。
 * test_voice_settings.c 会直接 include 本文件并自带这三个桩，因此用宏把
 * 定义关掉，避免重复定义；单独构建本测试时由这里提供。 */
#ifndef SMART_HOME_TEST_HAS_MILOCO_STUBS
bool smart_home_miloco_config_valid(const smart_home_miloco_config_t *config)
{ return config && config->host[0] != '\0' && config->port != 0; }
int smart_home_miloco_request_save(smart_home_miloco_t *service,
                                   const smart_home_miloco_config_t *config)
{ (void)service; (void)config; return AGENT_ERROR; }
void smart_home_lvgl_open_miloco_bind(smart_home_lvgl_t *ui) { (void)ui; }
#endif
int smart_home_mcp_bridge_reconfigure(smart_home_mcp_bridge_t *bridge,
                                     const smart_home_mcp_runtime_config_t *config)
{
    assert(bridge);
    applied_mcp = *config;
    mcp_calls++;
    return AGENT_OK;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{ (void)area; (void)pixels; lv_display_flush_ready(display); }

/* 可选：显示缓冲是 DIRECT 模式的 RGB565，直接转存成 PPM 便于人工核对。
 * 只在设置了环境变量时写文件，不参与断言。 */
static void snapshot(const uint16_t *fb, const char *var)
{
    const char *path = getenv(var);
    FILE *out;

    if (!path) return;
    out = fopen(path, "wb");
    if (!out) return;
    fprintf(out, "P6\n1024 600\n255\n");
    for (int i = 0; i < 1024 * 600; i++) {
        uint16_t v = fb[i];
        uint8_t rgb[3];

        rgb[0] = (uint8_t)(((v >> 11) & 0x1F) * 255 / 31);
        rgb[1] = (uint8_t)(((v >> 5) & 0x3F) * 255 / 63);
        rgb[2] = (uint8_t)((v & 0x1F) * 255 / 31);
        fwrite(rgb, 1, 3, out);
    }
    fclose(out);
}
static void heartbeat(lv_timer_t *timer)
{ (*(unsigned *)lv_timer_get_user_data(timer))++; }

static void wait_ready(smart_home_lvgl_t *ui, int model)
{
    const struct timespec pause = {.tv_nsec = 1000000};
    for (unsigned i = 0; i < 3000; i++) {
        int ready;
        pthread_mutex_lock(&ui->pending_mutex);
        ready = model ? g_model.ready : g_net.ready;
        pthread_mutex_unlock(&ui->pending_mutex);
        if (ready) return;
        nanosleep(&pause, NULL);
    }
    assert(!"service did not publish its result");
}

static void click(lv_obj_t *button, lv_event_cb_t callback, smart_home_lvgl_t *ui)
{
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, ui);
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
}

static void *release_pending(void *unused)
{
    const struct timespec pause = {.tv_nsec = 20000000};
    (void)unused;
    nanosleep(&pause, NULL);
    release_service(0);
    release_service(1);
    return NULL;
}

static smart_home_lvgl_t *setup(smart_home_agent_app_t *app)
{
    smart_home_lvgl_t *ui = calloc(1, sizeof(*ui));
    assert(ui && pthread_mutex_init(&ui->pending_mutex, NULL) == 0);
    ui->app = app;
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.screen = lv_obj_create(NULL);
    lv_screen_load(g_settings.screen);
    build_model_panel(ui, g_settings.screen);
    build_network_panel(ui, g_settings.screen);
    g_settings.keyboard = lv_keyboard_create(g_settings.screen);
    /* 复现生产布局：构建期就在屏上，贴底浮在导航栏上方。层级和遮挡判断
     * 都依赖这套几何，缺了它测试就成了在同尺寸对象之间比坐标。 */
    smart_home_lvgl_style_keyboard(g_settings.keyboard);
    lv_obj_set_size(g_settings.keyboard, smart_home_lvgl_content_w(),
                    smart_home_lvgl_keyboard_h());
    lv_obj_align(g_settings.keyboard, LV_ALIGN_BOTTOM_MID, 0, -72);
    lv_obj_add_flag(g_settings.keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(g_settings.net_ssid, "test-ssid");
    lv_textarea_set_text(g_settings.net_password, "test-password");
    lv_textarea_set_text(g_settings.model_name, "test-model");
    lv_textarea_set_text(g_settings.model_key, "test-key");
    lv_textarea_set_text(g_settings.model_timeout, "30000");
    lv_textarea_set_text(g_settings.model_tokens, "512");
    ui->v9_network_timer = lv_timer_create(v9_net_timer_cb, 250, ui);
    assert(lv_textarea_get_password_mode(g_settings.model_key));
    assert(lv_obj_get_width(g_settings.model_key) == 704);
    assert(lv_obj_get_width(g_settings.model_dd) == 336);
    assert(lv_obj_get_style_text_font(g_settings.model_key, 0) == smart_home_lvgl_font(17));
    assert(lv_obj_get_style_text_font(g_settings.model_key,
        LV_PART_TEXTAREA_PLACEHOLDER) == smart_home_lvgl_font(17));
    assert(lv_obj_get_style_text_font(lv_dropdown_get_list(g_settings.model_dd), 0) ==
        smart_home_lvgl_font(17));
    const uint32_t required[] = {0x6a21, 0x578b, 0x670d, 0x52a1, 0x5bc6, 0x94a5, '*'};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        lv_font_glyph_dsc_t glyph;
        assert(lv_font_get_glyph_dsc(smart_home_lvgl_font(17), &glyph, required[i], 0));
        assert(!glyph.is_placeholder);
    }
    const void *symbol = lv_dropdown_get_symbol(g_settings.model_dd);
    assert(symbol && lv_image_src_get_type(symbol) == LV_IMAGE_SRC_VARIABLE);
    return ui;
}

int main(void)
{
    static uint16_t pixels[1024 * 600];
    smart_home_agent_app_t app = {0};
    lv_obj_t *screen;
    smart_home_lvgl_t *ui;
    unsigned ticks = 0;
    pthread_t releaser;
    lv_timer_t *pulse;

    lv_init();
    lv_display_t *display = lv_display_create(1024, 600);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels),
                            LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, flush);
    assert(smart_home_lvgl_style_init() == 0);
    assert(pthread_mutex_init(&app.agent_mutex, NULL) == 0);
    app.mcp_bridge = (smart_home_mcp_bridge_t *)&app;
    ui = setup(&app);
    screen = g_settings.screen;

    /* 当前连接来自网络状态，不能随待连接 SSID 草稿一起改变。 */
    app.system_status.network_status.ip_status = SMART_HOME_NETWORK_OK;
    snprintf(app.system_status.network_status.ssid,
             sizeof(app.system_status.network_status.ssid), "家庭 Wi-Fi");
    v9_net_status_refresh(ui);
    assert(!strcmp(lv_label_get_text(g_settings.net_connected), "当前连接：家庭 Wi-Fi"));
    lv_textarea_set_text(g_settings.net_ssid, "another-network");
    v9_net_status_refresh(ui);
    assert(!strcmp(lv_label_get_text(g_settings.net_connected), "当前连接：家庭 Wi-Fi"));
    app.system_status.network_status.ip_status = SMART_HOME_NETWORK_ERR_NO_IP;
    v9_net_status_refresh(ui);
    assert(!strcmp(lv_label_get_text(g_settings.net_connected), "当前未连接 Wi-Fi"));
    app.system_status.network_status.ip_status = SMART_HOME_NETWORK_OK;
    v9_net_status_refresh(ui);
    assert(strstr(lv_label_get_text(g_settings.net_connected), "家庭 Wi-Fi"));
    app.system_status.network_status.ssid[0] = '\0';
    v9_net_status_refresh(ui);
    assert(strstr(lv_label_get_text(g_settings.net_connected), "名称暂不可用"));
    lv_textarea_set_text(g_settings.net_ssid, "test-ssid");
    puts("PASS: connected SSID is independent of connection drafts and hidden while disconnected");

    mcp_config_open(ui);
    assert(lv_dropdown_get_selected(g_settings.mcp_protocol) == 0);
    assert(lv_textarea_get_password_mode(g_settings.mcp_auth));
    lv_textarea_set_text(g_settings.mcp_server, "test-server");
    lv_textarea_set_text(g_settings.mcp_host, "example.invalid");
    lv_textarea_set_text(g_settings.mcp_path, "/mcp");
    lv_textarea_set_text(g_settings.mcp_auth, "Bearer test-token");
    lv_obj_t *apply = lv_button_create(screen);
    click(apply, mcp_apply_cb, ui);
    assert(mcp_calls == 1 && applied_mcp.use_tls && applied_mcp.port == 443);
    lv_refr_now(NULL);
    snapshot(pixels, "SMART_HOME_MCP_SHOT");
    lv_dropdown_set_selected(g_settings.mcp_protocol, 1);
    lv_textarea_set_text(g_settings.mcp_port, "80");
    lv_obj_send_event(apply, LV_EVENT_CLICKED, NULL);
    assert(mcp_calls == 2 && !applied_mcp.use_tls && applied_mcp.port == 80);
    const char *bad_ports[] = {"65537", "0", "abc", "443x", "9999999999999999999999"};
    for (size_t i = 0; i < sizeof(bad_ports) / sizeof(bad_ports[0]); i++) {
        lv_textarea_set_text(g_settings.mcp_port, bad_ports[i]);
        lv_obj_send_event(apply, LV_EVENT_CLICKED, NULL);
        assert(mcp_calls == 2);
    }
    puts("PASS: explicit MCP TLS, HTTP, port validation and masked credentials");

    /* 弹窗遮罩运行期才建，永远排在构建期就存在的键盘之后。输入框聚焦时必须
     * 把键盘提到最前，否则键盘被遮罩盖住，界面上像没弹键盘、也打不进字。 */
    assert(lv_obj_get_index(g_settings.keyboard) !=
           (int32_t)lv_obj_get_child_count(screen) - 1);
    lv_obj_send_event(g_settings.mcp_server, LV_EVENT_FOCUSED, NULL);
    assert(lv_obj_get_index(g_settings.keyboard) ==
           (int32_t)lv_obj_get_child_count(screen) - 1);
    assert(!lv_obj_has_flag(g_settings.keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_keyboard_get_textarea(g_settings.keyboard) == g_settings.mcp_server);
    /* 弹窗居中 + 键盘贴底，靠下的输入框会落进键盘区域。聚焦时必须把面板
     * 上移到输入框整体位于键盘上沿之上，否则看得见键盘却看不见输入框。 */
    {
        lv_obj_t *panel = lv_obj_get_child(g_settings.config_modal, 0);
        lv_obj_t *inputs[] = {g_settings.mcp_server, g_settings.mcp_host,
                              g_settings.mcp_port, g_settings.mcp_path,
                              g_settings.mcp_auth};

        for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
            lv_area_t input_area;
            lv_area_t keyboard_area;

            lv_obj_send_event(inputs[i], LV_EVENT_FOCUSED, NULL);
            /* lv_obj_align 只标脏，坐标要等布局刷新后才更新。 */
            lv_obj_update_layout(screen);
            lv_obj_get_coords(inputs[i], &input_area);
            lv_obj_get_coords(g_settings.keyboard, &keyboard_area);
            assert(input_area.y2 <= keyboard_area.y1);
            /* 面板不能顶出屏幕顶端，底部按钮也不能被键盘压住。 */
            assert(lv_obj_get_y(panel) >= 0);
            assert(lv_obj_get_y(panel) + lv_obj_get_height(panel) <=
                   keyboard_area.y1);
        }
        /* 内容比压缩后的面板高时必须可滚动，否则底部按钮永远够不到。 */
        if (lv_obj_get_height(panel) < 420) {
            assert(lv_obj_get_scroll_bottom(panel) > 0);
        }
        lv_refr_now(NULL);
        snapshot(pixels, "SMART_HOME_MCP_KB_SHOT");
        /* 幂等：回到第一个输入框，面板应回到同一位置，不累积偏移。 */
        {
            int32_t after_last = lv_obj_get_y(panel);

            lv_obj_send_event(g_settings.mcp_server, LV_EVENT_FOCUSED, NULL);
            lv_obj_update_layout(screen);
            assert(lv_obj_get_y(panel) == after_last);
            /* 键盘弹出时面板在键盘上方的可用区内居中，必然更靠上。 */
            assert(lv_obj_get_y(panel) < (600 - lv_obj_get_height(panel)) / 2);
        }
    }
    puts("PASS: keyboard raised above the settings modal on focus");

    /* 米家网关弹窗是影响用户的那一例，单独覆盖：三行输入框，位置比 MCP 更低。 */
    {
        lv_obj_t *panel;
        lv_area_t input_area;
        lv_area_t keyboard_area;

        miloco_config_open(ui);
        panel = lv_obj_get_child(g_settings.config_modal, 0);
        assert(panel);
        lv_obj_send_event(g_settings.miloco_host, LV_EVENT_FOCUSED, NULL);
        lv_obj_update_layout(screen);
        assert(lv_obj_get_index(g_settings.keyboard) ==
               (int32_t)lv_obj_get_child_count(screen) - 1);
        assert(lv_keyboard_get_textarea(g_settings.keyboard) ==
               g_settings.miloco_host);
        lv_obj_get_coords(g_settings.miloco_host, &input_area);
        lv_obj_get_coords(g_settings.keyboard, &keyboard_area);
        assert(input_area.y2 <= keyboard_area.y1);
        assert(lv_obj_get_y(panel) >= 0);

        /* 最下面一行的令牌框也必须露出来。 */
        lv_obj_send_event(g_settings.miloco_token, LV_EVENT_FOCUSED, NULL);
        lv_obj_update_layout(screen);
        lv_obj_get_coords(g_settings.miloco_token, &input_area);
        lv_obj_get_coords(g_settings.keyboard, &keyboard_area);
        assert(input_area.y2 <= keyboard_area.y1);
        assert(lv_obj_get_y(panel) >= 0);

        /* 面板底部的「直接扫码绑定 / 保存并扫码授权」必须也在键盘上方，
         * 否则键盘一弹出来就点不到，等于无法继续。 */
        lv_obj_get_coords(panel, &input_area);
        assert(input_area.y2 <= keyboard_area.y1);
        lv_refr_now(NULL);
        snapshot(pixels, "SMART_HOME_MILOCO_SHOT");
        puts("PASS: miloco gateway inputs stay above the keyboard");
    }

    /* 关窗先解绑：输入框是遮罩的子对象，会随遮罩一起销毁，键盘只存裸指针。 */
    config_modal_close();
    assert(!lv_keyboard_get_textarea(g_settings.keyboard));
    assert(lv_obj_has_flag(g_settings.keyboard, LV_OBJ_FLAG_HIDDEN));
    puts("PASS: closing the modal unbinds the keyboard textarea");

    /* A held Agent lock must not block the UI callback or LVGL timers. */
    pthread_mutex_lock(&app.agent_mutex);
    lv_obj_send_event(g_settings.model_apply, LV_EVENT_CLICKED, NULL);
    wait_entered(1);
    assert(!pthread_equal(pthread_self(), gates[1].thread));
    assert(g_model.active && lv_obj_has_state(g_settings.model_apply, LV_STATE_DISABLED));
    lv_obj_send_event(g_settings.model_apply, LV_EVENT_CLICKED, NULL);
    assert(gates[1].entered == 1);
    release_service(1);
    pulse = lv_timer_create(heartbeat, 1, &ticks);
    lv_tick_inc(5);
    lv_timer_handler();
    assert(ticks > 0);
    lv_timer_delete(pulse);
    pthread_mutex_unlock(&app.agent_mutex);
    wait_ready(ui, 1);
    v9_net_timer_cb(ui->v9_network_timer);
    assert(!g_model.active && !lv_obj_has_state(g_settings.model_apply, LV_STATE_DISABLED));
    assert(!strcmp(applied_model.model, "test-model"));
    assert(!strcmp(applied_model.api_key, "test-key"));
    assert(!strcmp(lv_label_get_text(g_settings.model_status), "本次配置已应用"));

    memset(gates, 0, sizeof(gates));
    gates[1].result = AGENT_ERROR_INVALID;
    lv_obj_send_event(g_settings.model_apply, LV_EVENT_CLICKED, NULL);
    wait_entered(1);
    release_service(1);
    wait_ready(ui, 1);
    v9_net_timer_cb(ui->v9_network_timer);
    assert(!strcmp(lv_label_get_text(g_settings.model_status), "配置应用失败"));
    puts("PASS: async model apply, UI heartbeat, duplicate rejection and failure feedback");

    /* Teardown must join both pending services before ui/mutex destruction. */
    memset(gates, 0, sizeof(gates));
    v9_net_connect(ui);
    lv_obj_send_event(g_settings.model_apply, LV_EVENT_CLICKED, NULL);
    wait_entered(0);
    wait_entered(1);
    assert(pthread_create(&releaser, NULL, release_pending, NULL) == 0);
    smart_home_lvgl_settings_v9_deinit(ui);
    pthread_join(releaser, NULL);
    assert(gates[0].done && gates[1].done);
    assert(!g_net.active && !g_net.owner && !g_model.active && !g_model.owner);
    assert(!ui->v9_network_timer);
    assert(pthread_mutex_destroy(&ui->pending_mutex) == 0);
    free(ui);
    lv_obj_delete(screen);

    memset(gates, 0, sizeof(gates));
    ui = setup(&app);
    screen = g_settings.screen;
    v9_net_connect(ui);
    wait_entered(0);
    release_service(0);
    wait_ready(ui, 0);
    v9_net_timer_cb(ui->v9_network_timer);
    assert(app.system_status.network_status.online && !g_net.active);
    smart_home_lvgl_settings_v9_deinit(ui);
    assert(pthread_mutex_destroy(&ui->pending_mutex) == 0);
    free(ui);
    lv_obj_delete(screen);
    assert(pthread_mutex_destroy(&app.agent_mutex) == 0);
    puts("PASS: pending network/model teardown, mutex lifetime and UI reinitialization");
    return 0;
}
