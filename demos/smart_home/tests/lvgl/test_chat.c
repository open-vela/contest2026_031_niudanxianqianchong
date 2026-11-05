/* Real LVGL widgets and V2 bridge; only the model/session boundary is mocked. */
#include "smart_home_lvgl_internal.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static atomic_int reject_bridge_allocations;
static void *bridge_calloc(size_t count, size_t size)
{
    return atomic_load(&reject_bridge_allocations) ? NULL : calloc(count, size);
}
#define calloc bridge_calloc
#include "smart_home_lvgl_agent.c"
#undef calloc

static smart_home_lvgl_t *current_ui;
static pthread_t ui_thread;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int hold_worker, entered_worker, model_calls, clear_calls, clear_status;
static int history_visits, history_status, refreshes;
static uint16_t pixels[1024 * 600];
void *smart_home_bulk_alloc(size_t size) { return malloc(size); }
void smart_home_bulk_free(void *ptr) { free(ptr); }
void smart_home_cpu_debug_log(const char *stage) { (void)stage; }
void smart_home_memory_log_region(const char *name, const void *ptr) { (void)name; (void)ptr; }
void smart_home_bulk_diag(const char *point) { (void)point; }
void smart_home_lvgl_build_top_bar(lv_obj_t *screen, smart_home_lvgl_t *ui, const char *title)
{ (void)screen; (void)ui; (void)title; }
lv_obj_t *smart_home_lvgl_build_nav_bar(lv_obj_t *screen, smart_home_lvgl_t *ui)
{ (void)screen; (void)ui; return NULL; }
void smart_home_lvgl_update_clock(smart_home_lvgl_t *ui) { (void)ui; }
void smart_home_lvgl_refresh_cards(smart_home_lvgl_t *ui)
{ assert(ui == current_ui && pthread_equal(ui_thread, pthread_self())); refreshes++; }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{ (void)area; (void)data; lv_display_flush_ready(display); }

static void emit(agent_event_type_t type, agent_error_t status,
                 const char *name, const char *id)
{
    char borrowed_name[64], borrowed_id[64];
    snprintf(borrowed_name, sizeof(borrowed_name), "%s", name ? name : "");
    snprintf(borrowed_id, sizeof(borrowed_id), "%s", id ? id : "");
    agent_event_t event = {.type = type, .status = status};
    event.tool_call.name = agent_string_view(borrowed_name, strlen(borrowed_name));
    event.tool_call.id = agent_string_view(borrowed_id, strlen(borrowed_id));
    smart_home_lvgl_event_cb(current_ui, &event);
    memset(borrowed_name, 'x', sizeof(borrowed_name));
    memset(borrowed_id, 'x', sizeof(borrowed_id));
}

int smart_home_agent_run(smart_home_agent_app_t *app, const char *input,
                         char *out, size_t capacity)
{
    assert(!pthread_equal(ui_thread, pthread_self()));
    pthread_mutex_lock(&gate);
    model_calls++;
    entered_worker = 1;
    pthread_cond_broadcast(&changed);
    while (hold_worker) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
    app->system_status.session_status = AGENT_OK;
    emit(AGENT_EVENT_MODEL_BEGIN, AGENT_OK, NULL, NULL);
    if (!strcmp(input, "tools") || !strcmp(input, "missing-result")) {
        emit(AGENT_EVENT_TOOL_BEGIN, AGENT_OK, "set_light", "private-call-1");
        if (!strcmp(input, "tools")) {
            emit(AGENT_EVENT_TOOL_END, AGENT_OK, "set_light", "private-call-1");
            emit(AGENT_EVENT_TOOL_BEGIN, AGENT_OK, "miot_device_control", "private-call-2");
            emit(AGENT_EVENT_TOOL_END, AGENT_OK, "miot_device_control", "private-call-2");
        }
    }
    agent_error_t ret = !strcmp(input, "network") ? AGENT_ERROR_IO :
                        !strcmp(input, "auth") ? AGENT_ERROR_AUTH :
                        !strcmp(input, "truncated") ? AGENT_ERROR_TRUNCATED :
                        !strcmp(input, "save-failure") ? AGENT_ERROR_IO : AGENT_OK;
    if (!strcmp(input, "save-failure")) app->system_status.session_status = AGENT_ERROR_IO;
    snprintf(out, capacity, "%s", !strcmp(input, "network") || !strcmp(input, "auth") ?
             "" : "这是回复");
    emit(AGENT_EVENT_TURN_END, ret, NULL, NULL);
    /* Completion must have been reserved before this worker was started. */
    atomic_store(&reject_bridge_allocations, 1);
    return ret;
}

int smart_home_agent_app_clear_session(smart_home_agent_app_t *app)
{ assert(app); clear_calls++; return clear_status; }
int smart_home_agent_app_visit_history(smart_home_agent_app_t *app,
                                       agent_session_visit_fn visit, void *data)
{
    assert(app);
    history_visits++;
    for (int i = 0; i < 2; i++) {
        char user[32], reply[32], name[32];
        snprintf(user, sizeof(user), "%s", i ? "旧的问题" : "新的问题");
        snprintf(reply, sizeof(reply), "%s", i ? "旧的回复" : "新的回复");
        strcpy(name, "set_light");
        agent_tool_call_view_t call = {.name = agent_string_view(name, strlen(name))};
        agent_message_view_t messages[] = {
            {.role = AGENT_MESSAGE_ROLE_USER, .content = agent_string_view(user, strlen(user))},
            {.role = AGENT_MESSAGE_ROLE_ASSISTANT, .tool_calls = &call, .tool_call_count = i ? 1u : 0u},
            {.role = AGENT_MESSAGE_ROLE_ASSISTANT, .content = agent_string_view(reply, strlen(reply))}
        };
        agent_session_group_view_t group = {messages, 3};
        agent_error_t ret = visit(data, &group);
        if (ret) return ret;
        memset(user, 'x', sizeof(user)); memset(reply, 'x', sizeof(reply));
        memset(name, 'x', sizeof(name));
    }
    return history_status;
}

static int count_text(lv_obj_t *root, const char *needle)
{
    int count = lv_obj_check_type(root, &lv_label_class) &&
                strstr(lv_label_get_text(root), needle) != NULL;
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++)
        count += count_text(lv_obj_get_child(root, i), needle);
    return count;
}
static lv_obj_t *find_button(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text))
        return lv_obj_get_parent(root);
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_button(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}
static void wait_done(smart_home_lvgl_t *ui)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (ui->request_inflight) {
        smart_home_lvgl_process_pending(ui);
        usleep(1000);
        clock_gettime(CLOCK_MONOTONIC, &now);
        assert(now.tv_sec - start.tv_sec < 5);
    }
    assert(!ui->agent_worker_active && !ui->pending_head && !ui->pending_tail);
    assert(!lv_obj_has_state(ui->chat_send_btn, LV_STATE_DISABLED));
}
static void send(smart_home_lvgl_t *ui, const char *input)
{
    atomic_store(&reject_bridge_allocations, 0);
    smart_home_lvgl_chat_send_text(ui, input);
    wait_done(ui);
}
static void clear(smart_home_lvgl_t *ui)
{
    clear_status = AGENT_OK;
    assert(smart_home_lvgl_chat_clear(ui) == AGENT_OK);
    assert(!ui->chat_trace_body && !ui->chat_trace_row && !ui->chat_trace_tool_count);
    assert(lv_obj_get_child_count(ui->chat_list) == 0);
}

static void test_bubble_layout(smart_home_lvgl_t *ui)
{
    smart_home_lvgl_append_msg_bubble(ui, "好的，已完成。", 0);
    lv_obj_update_layout(ui->screen_chat);
    lv_obj_t *row = lv_obj_get_child(ui->chat_list, -1);
    lv_obj_t *bubble = lv_obj_get_child(row, 1);
    int short_height = lv_obj_get_height(bubble);
    assert(short_height >= 44 && short_height <= 80);
    const char *long_reply =
        "客厅空调已经调整为制冷模式，目标温度设为二十四度。"
        "卧室灯光保持暖色，厨房设备继续使用原来的运行设置。"
        "如果还需要调整其他房间，可以告诉我设备名称和目标状态。"
        "这是一条用于检查自动换行和气泡高度的较长回复。";
    smart_home_lvgl_append_msg_bubble(ui, long_reply, 0);
    lv_obj_update_layout(ui->screen_chat);
    row = lv_obj_get_child(ui->chat_list, -1);
    bubble = lv_obj_get_child(row, 1);
    lv_obj_t *label = lv_obj_get_child(bubble, 0);
    lv_area_t text_bounds, content_bounds;
    lv_obj_get_coords(label, &text_bounds);
    lv_obj_get_content_coords(bubble, &content_bounds);
    assert(lv_obj_get_height(bubble) > short_height);
    assert(text_bounds.y1 >= content_bounds.y1 &&
           text_bounds.y2 <= content_bounds.y2);
    smart_home_lvgl_append_msg_bubble(ui, "一行\n二行\n三行", 0);
    lv_obj_update_layout(ui->screen_chat);
    bubble = lv_obj_get_child(lv_obj_get_child(ui->chat_list, -1), 1);
    assert(lv_obj_get_height(bubble) > short_height);
    clear(ui);
    puts("PASS: compact one-line AI bubble; wrapped and multiline replies expand without clipping");
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(1024, 600);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, flush);
    assert(smart_home_lvgl_style_init() == 0);
    smart_home_agent_app_t app = {0};
    smart_home_lvgl_t ui = {.app = &app};
    current_ui = &ui;
    ui_thread = pthread_self();
    assert(!pthread_mutex_init(&ui.pending_mutex, NULL));
    smart_home_lvgl_build_chat_screen(&ui);
    lv_screen_load(ui.screen_chat);
    test_bubble_layout(&ui);
    send(&ui, "你好");
    assert(!ui.chat_trace_row && count_text(ui.chat_list, "这是回复") == 1);
    assert(lv_obj_get_child_count(ui.chat_list) == 2);
    puts("PASS: V2 completion under allocation pressure; greeting creates no operation card");

    clear(&ui);
    send(&ui, "tools");
    assert(ui.chat_trace_tool_count == 2 && ui.chat_trace_collapsed);
    assert(!strcmp(ui.chat_trace_tools[0].id, "private-call-1"));
    assert(!strcmp(ui.chat_trace_tools[0].name, "set_light"));
    assert(ui.chat_trace_tools[0].state == 1 && ui.chat_trace_tools[1].state == 1);
    assert(count_text(ui.chat_list, "调整灯光") >= 1);
    assert(!count_text(ui.chat_list, "set_light") && !count_text(ui.chat_list, "private-call"));
    lv_obj_t *header = lv_obj_get_child(ui.chat_trace_body, 0);
    /* The trace card's first child is its header; body points to the collapsible details. */
    (void)header;
    lv_obj_t *card = lv_obj_get_parent(ui.chat_trace_body);
    lv_obj_send_event(lv_obj_get_child(card, 0), LV_EVENT_CLICKED, NULL);
    assert(!lv_obj_has_flag(ui.chat_trace_body, LV_OBJ_FLAG_HIDDEN));
    puts("PASS: V2 borrowed views copied; Chinese operations collapsed and expandable");

    clear(&ui);
    send(&ui, "missing-result");
    assert(ui.chat_trace_tools[0].state == -2);
    assert(count_text(ui.chat_list, "结果待确认") == 1);
    assert(!count_text(ui.chat_list, "Unconfirmed"));
    clear(&ui);
    send(&ui, "network");
    assert(count_text(ui.chat_list, "暂时无法连接服务") == 1);
    assert(!count_text(ui.chat_list, "Agent call failed"));
    clear(&ui);
    send(&ui, "auth");
    assert(count_text(ui.chat_list, "服务认证失败") == 1);
    clear(&ui);
    send(&ui, "truncated");
    assert(count_text(ui.chat_list, "这是回复") == 1 && count_text(ui.chat_list, "已截断") == 1);
    clear(&ui);
    send(&ui, "save-failure");
    assert(count_text(ui.chat_list, "这是回复") == 1 && count_text(ui.chat_list, "历史保存失败") == 1);
    assert(count_text(ui.chat_list, "控制可能已执行") == 1);
    puts("PASS: missing results stay unconfirmed; one Chinese error; truncation/save failure keep the answer");

    clear(&ui);
    pthread_mutex_lock(&gate);
    hold_worker = 1; entered_worker = 0;
    pthread_mutex_unlock(&gate);
    atomic_store(&reject_bridge_allocations, 0);
    smart_home_lvgl_chat_send_text(&ui, "blocked");
    pthread_mutex_lock(&gate);
    while (!entered_worker) pthread_cond_wait(&changed, &gate);
    int before_models = model_calls;
    pthread_mutex_unlock(&gate);
    assert(lv_obj_has_state(ui.chat_send_btn, LV_STATE_DISABLED));
    lv_textarea_set_text(ui.chat_input, "保留的草稿");
    unsigned before_messages = lv_obj_get_child_count(ui.chat_list);
    smart_home_lvgl_chat_send_text(&ui, "不得重复提交");
    assert(lv_obj_get_child_count(ui.chat_list) == before_messages + 1 && model_calls == before_models);
    assert(count_text(ui.chat_list, "正在处理上一条消息") == 1);
    int before_clear = clear_calls;
    assert(smart_home_lvgl_chat_clear(&ui) == AGENT_ERROR_BUSY && clear_calls == before_clear);
    assert(!strcmp(lv_textarea_get_text(ui.chat_input), "保留的草稿"));
    pthread_mutex_lock(&gate);
    hold_worker = 0; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    wait_done(&ui);
    assert(!strcmp(lv_textarea_get_text(ui.chat_input), "保留的草稿"));
    lv_obj_t *new_button = find_button(ui.screen_chat, "新对话");
    assert(new_button);
    clear_status = AGENT_ERROR_IO;
    before_messages = lv_obj_get_child_count(ui.chat_list);
    lv_obj_send_event(new_button, LV_EVENT_CLICKED, NULL);
    assert(lv_obj_get_child_count(ui.chat_list) >= before_messages);
    assert(count_text(ui.chat_list, "这是回复") == 1);
    clear_status = AGENT_OK;
    lv_obj_send_event(new_button, LV_EVENT_CLICKED, NULL);
    assert(!lv_obj_get_child_count(ui.chat_list) && !strcmp(lv_textarea_get_text(ui.chat_input), ""));
    assert(!ui.chat_trace_row && !ui.chat_trace_tool_count);
    puts("PASS: busy state protects draft/history; new-conversation success/failure follows session result");

    history_status = AGENT_OK;
    assert(smart_home_lvgl_chat_restore(&ui) == AGENT_OK);
    assert(lv_obj_get_child_count(ui.chat_list) == 5);
    assert(count_text(ui.chat_list, "历史操作：演示 · 调整灯光") == 1);
    assert(!strcmp(smart_home_lvgl_tool_display_name("board_camera"), "本机 · 摄像头操作"));
    assert(!strcmp(smart_home_lvgl_tool_display_name("miot_device_control"), "米家 · 提交设备控制"));
    assert(!strcmp(smart_home_lvgl_tool_display_name("user_memory"), "管理用户记忆"));
    assert(!strcmp(smart_home_lvgl_tool_display_name("node_bedroom_status"), "Node · 节点操作"));
    assert(!strcmp(smart_home_lvgl_tool_display_name("mcp_bridge_status"), "MCP · 服务调用"));
    assert(!count_text(ui.chat_list, "set_light"));
    assert(count_text(lv_obj_get_child(ui.chat_list, 0), "旧的问题") == 1);
    assert(count_text(lv_obj_get_child(ui.chat_list, 4), "新的回复") == 1);
    clear(&ui);
    history_status = AGENT_ERROR_PARSE;
    assert(smart_home_lvgl_chat_restore(&ui) == AGENT_ERROR_PARSE);
    assert(count_text(ui.chat_list, "旧的问题") == 0 && count_text(ui.chat_list, "原记录已保留") == 1);
    int before_visits = history_visits;
    ui.request_inflight = 1;
    assert(smart_home_lvgl_chat_restore(&ui) == AGENT_ERROR_BUSY && history_visits == before_visits);
    ui.request_inflight = 0;
    lv_obj_update_layout(ui.screen_chat);
    lv_area_t list_area, input_area, status_area;
    lv_obj_get_coords(ui.chat_list, &list_area);
    lv_obj_get_coords(ui.chat_input_bar, &input_area);
    lv_obj_get_coords(ui.chat_status, &status_area);
    assert(list_area.y2 < status_area.y1 && status_area.y2 < input_area.y1);
    puts("PASS: chronological history, copied views, Chinese historical operations, atomic read failure and reserved status layout");
    smart_home_lvgl_discard_pending(&ui);
    smart_home_bulk_free(ui.agent_worker_stack_alloc);
    pthread_mutex_destroy(&ui.pending_mutex);
    lv_obj_delete(ui.screen_chat);
    smart_home_lvgl_style_deinit();
    return 0;
}
