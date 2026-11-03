#include "ui/lvgl/smart_home_lvgl_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/ui/lvgl/smart_home_lvgl_history.c"

static int rendered, errors, visits, fail_read;
static const char *expected[] = {"old user", "历史操作：调整灯光", "old answer", "new user", "new answer"};
const char *smart_home_lvgl_tool_display_name(const char *name)
{ assert(!strcmp(name, "set_light")); return "调整灯光"; }
static agent_string_view_t view(const char *s) { return agent_string_view(s, strlen(s)); }
int smart_home_agent_app_visit_history(smart_home_agent_app_t *app,
                                       agent_session_visit_fn visit, void *context)
{
    char user[32], answer[32], tool[32];
    assert(app);
    ++visits;
    for (int i = 0; i < 2; ++i) {
        strcpy(user, i ? "old user" : "new user");
        strcpy(answer, i ? "old answer" : "new answer");
        strcpy(tool, "set_light");
        agent_tool_call_view_t call = {.name = view(tool)};
        agent_message_view_t messages[3] = {
            {.role = AGENT_MESSAGE_ROLE_USER, .content = view(user)},
            {.role = AGENT_MESSAGE_ROLE_ASSISTANT, .tool_calls = &call, .tool_call_count = i ? 1u : 0u},
            {.role = AGENT_MESSAGE_ROLE_ASSISTANT, .content = view(answer)}
        };
        const agent_session_group_view_t group = {messages, 3};
        int ret = visit(context, &group);
        if (ret) return ret;
        memset(user, 'x', sizeof(user)); memset(answer, 'x', sizeof(answer)); memset(tool, 'x', sizeof(tool));
    }
    return fail_read ? AGENT_ERROR_PARSE : AGENT_OK;
}
void smart_home_lvgl_append_msg_bubble(smart_home_lvgl_t *ui, const char *text, int is_user)
{
    assert(ui && rendered < 5 && !strcmp(text, expected[rendered]));
    assert(is_user == (rendered == 0 || rendered == 3));
    ++rendered;
}
void smart_home_lvgl_append_error_bubble(smart_home_lvgl_t *ui, const char *text)
{ assert(ui && strstr(text, "原记录已保留")); ++errors; }
void lv_label_set_text(lv_obj_t *object, const char *text) { assert(object && strstr(text, "5")); }
void lv_obj_remove_flag(lv_obj_t *object, lv_obj_flag_t flag) { (void)object; (void)flag; }
int main(void)
{
    smart_home_agent_app_t app = {0};
    smart_home_lvgl_t ui = {.app = &app, .chat_list = (lv_obj_t *)&app, .chat_status = (lv_obj_t *)&app};
    assert(!smart_home_lvgl_chat_restore(&ui) && rendered == 5 && visits == 1 && !errors);
    rendered = 0; fail_read = 1;
    assert(smart_home_lvgl_chat_restore(&ui) == AGENT_ERROR_PARSE && !rendered && errors == 1);
    ui.request_inflight = 1;
    assert(smart_home_lvgl_chat_restore(&ui) == AGENT_ERROR_BUSY && visits == 2);
    puts("PASS: real UI history copying, chronological rendering, historical tool labels, atomic read failure and busy rejection");
    return 0;
}
