#include "smart_home_lvgl_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct history_item {
    struct history_item *next;
    int is_user;
    char text[];
} history_item_t;

static void history_free(history_item_t *item)
{
    while (item) {
        history_item_t *next = item->next;
        free(item);
        item = next;
    }
}

static int history_add(history_item_t ***tail, agent_string_view_t text,
                        int is_user, const char *prefix)
{
    size_t prefix_bytes = strlen(prefix);
    history_item_t *item;
    if (!text.size) return AGENT_OK;
    if (!text.data || text.size > SIZE_MAX - sizeof(*item) - prefix_bytes - 1u)
        return AGENT_ERROR_INVALID;
    if (memchr(text.data, '\0', text.size)) return AGENT_ERROR_PARSE;
    item = malloc(sizeof(*item) + prefix_bytes + text.size + 1u);
    if (!item) return AGENT_ERROR_NOMEM;
    item->next = NULL;
    item->is_user = is_user;
    memcpy(item->text, prefix, prefix_bytes);
    memcpy(item->text + prefix_bytes, text.data, text.size);
    item->text[prefix_bytes + text.size] = '\0';
    **tail = item;
    *tail = &item->next;
    return AGENT_OK;
}

static agent_error_t history_collect(void *context, const agent_session_group_view_t *group)
{
    history_item_t **all = context;
    history_item_t *head = NULL, **tail = &head;
    int ret = AGENT_OK;
    for (size_t i = 0u; ret == AGENT_OK && i < group->message_count; ++i) {
        const agent_message_view_t *message = &group->messages[i];
        if (message->role == AGENT_MESSAGE_ROLE_USER ||
            message->role == AGENT_MESSAGE_ROLE_ASSISTANT)
            ret = history_add(&tail, message->content,
                              message->role == AGENT_MESSAGE_ROLE_USER, "");
        for (size_t j = 0u; ret == AGENT_OK && j < message->tool_call_count; ++j)
            ret = history_add(&tail, message->tool_calls[j].name, 0, "历史工具调用：");
    }
    if (ret != AGENT_OK) {
        history_free(head);
        return (agent_error_t)ret;
    }
    *tail = *all;
    *all = head;
    return AGENT_OK;
}

int smart_home_lvgl_chat_restore(smart_home_lvgl_t *ui)
{
    history_item_t *history = NULL;
    size_t count = 0u;
    int ret;
    if (!ui || !ui->app || !ui->chat_list) return AGENT_ERROR_INVALID;
    if (ui->request_inflight) return AGENT_ERROR_BUSY;
    ret = smart_home_agent_app_visit_history(ui->app, history_collect, &history);
    if (ret == AGENT_OK) {
        for (history_item_t *item = history; item; item = item->next) {
            smart_home_lvgl_append_msg_bubble(ui, item->text, item->is_user);
            ++count;
        }
        if (count && ui->chat_status) {
            char status[64];
            snprintf(status, sizeof(status), "已恢复 %u 条历史记录", (unsigned int)count);
            lv_label_set_text(ui->chat_status, status);
            lv_obj_clear_flag(ui->chat_status, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        char error[128];
        snprintf(error, sizeof(error), "历史对话读取失败（%d），原记录已保留。", ret);
        smart_home_lvgl_append_error_bubble(ui, error);
    }
    history_free(history);
    return ret;
}
