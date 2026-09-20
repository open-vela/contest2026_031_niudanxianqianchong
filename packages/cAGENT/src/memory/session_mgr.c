/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Session 管理器 — bounded in-memory 对话历史。
 *
 * 必须保持 tool calling 消息链完整：
 *   user → assistant(tool_calls) → tool(call_id) → assistant(final)
 *
 * 淘汰策略（按完整 turn）：
 *   - turn 边界：user message → assistant(final) message
 *   - 只淘汰完整 turn，不留下孤立 assistant(tool_calls)
 *   - 尾部未完成 turn 不参与淘汰
 *   - 无完整 turn 可淘汰时返回 AGENT_ERROR_LIMIT
 *
 * 消息内容内拷贝到固定缓冲区，不持有外部指针。
 */

#include "memory_internal.h"

#include "../core/agent_internal.h"
#include "../runtime/runtime.h"
#include "../types_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

/* ── 内部辅助函数 ── */

/**
 * 拷贝字符串到固定缓冲区。
 * 缓冲区不足时返回 AGENT_ERROR_LIMIT，不静默截断。
 */
static int copy_string(char *dst,
                       size_t dst_size,
                       const char *src,
                       bool allow_null,
                       size_t *written)
{
    size_t len;

    if (!dst || dst_size == 0u) {
        return AGENT_ERROR_INVALID;
    }

    if (!src) {
        if (!allow_null) {
            return AGENT_ERROR_INVALID;
        }
        dst[0] = '\0';
        if (written) {
            *written = 0u;
        }
        return AGENT_OK;
    }

    len = strlen(src);
    if (len >= dst_size) {
        dst[0] = '\0';
        return AGENT_ERROR_LIMIT;
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
    if (written) {
        *written = len;
    }
    return AGENT_OK;
}

static const char *normalize_session_id(const char *session_id)
{
    return session_id ? session_id : AGENT_SESSION_DEFAULT_ID;
}

static bool session_id_valid(const char *session_id)
{
    const char *id = normalize_session_id(session_id);

    return strlen(id) < sizeof(((agent_session_t *)0)->id);
}

/**
 * 检查 entry 是否为 assistant final 消息。
 *
 * assistant final = role 为 ASSISTANT 且无 tool_calls。
 * 这种消息标记一个 turn 的结束。
 */
static bool entry_is_assistant_final(const agent_session_entry_t *entry)
{
    return entry && entry->role == AGENT_MESSAGE_ROLE_ASSISTANT
           && entry->tool_call_count == 0u;
}

static bool session_has_unfinished_turn(const agent_session_t *session)
{
    uint32_t i;

    if (!session || session->count == 0u) {
        return false;
    }

    for (i = session->count; i > 0u; i--) {
        const agent_session_entry_t *entry = &session->entries[i - 1u];
        if (entry_is_assistant_final(entry)) {
            return false;
        }
        if (entry->turn_boundary) {
            return true;
        }
    }

    return false;
}

static bool tool_call_answered_after(const agent_session_t *session,
                                     uint32_t assistant_index,
                                     const char *tool_call_id)
{
    uint32_t i;

    for (i = assistant_index + 1u; i < session->count; i++) {
        const agent_session_entry_t *entry = &session->entries[i];
        if (entry->turn_boundary) {
            break;
        }
        if (entry->role == AGENT_MESSAGE_ROLE_TOOL
            && strcmp(entry->tool_call_id, tool_call_id) == 0) {
            return true;
        }
    }

    return false;
}

static bool session_has_pending_tool_calls(const agent_session_t *session)
{
    uint32_t turn_start;
    uint32_t i;

    if (!session || session->count == 0u) {
        return false;
    }

    turn_start = UINT32_MAX;
    for (i = session->count; i > 0u; i--) {
        if (session->entries[i - 1u].turn_boundary) {
            turn_start = i - 1u;
            break;
        }
        if (entry_is_assistant_final(&session->entries[i - 1u])) {
            return false;
        }
    }

    if (turn_start == UINT32_MAX) {
        return false;
    }

    for (i = turn_start; i < session->count; i++) {
        const agent_session_entry_t *entry = &session->entries[i];
        uint32_t call_index;

        if (entry->role != AGENT_MESSAGE_ROLE_ASSISTANT
            || entry->tool_call_count == 0u) {
            continue;
        }

        for (call_index = 0u; call_index < entry->tool_call_count; call_index++) {
            if (!tool_call_answered_after(session,
                                          i,
                                          entry->tool_calls[call_index].id)) {
                return true;
            }
        }
    }

    return false;
}

static bool session_has_pending_tool_call(const agent_session_t *session,
                                          const char *tool_call_id)
{
    uint32_t i;

    if (!session || !tool_call_id) {
        return false;
    }

    for (i = session->count; i > 0u; i--) {
        const agent_session_entry_t *entry = &session->entries[i - 1u];
        uint32_t call_index;

        if (entry->turn_boundary || entry_is_assistant_final(entry)) {
            break;
        }

        if (entry->role == AGENT_MESSAGE_ROLE_TOOL
            && strcmp(entry->tool_call_id, tool_call_id) == 0) {
            return false;
        }

        if (entry->role != AGENT_MESSAGE_ROLE_ASSISTANT
            || entry->tool_call_count == 0u) {
            continue;
        }

        for (call_index = 0u; call_index < entry->tool_call_count; call_index++) {
            if (strcmp(entry->tool_calls[call_index].id, tool_call_id) == 0) {
                return true;
            }
        }
    }

    return false;
}

static int append_raw(char *buffer, size_t buffer_size, size_t *used, const char *text)
{
    size_t len;

    if (!buffer || !used || !text) {
        return AGENT_ERROR_INVALID;
    }

    len = strlen(text);
    if (*used + len >= buffer_size) {
        return AGENT_ERROR_LIMIT;
    }

    memcpy(buffer + *used, text, len);
    *used += len;
    buffer[*used] = '\0';
    return AGENT_OK;
}

static int append_char(char *buffer, size_t buffer_size, size_t *used, char c)
{
    if (!buffer || !used) {
        return AGENT_ERROR_INVALID;
    }

    if (*used + 1u >= buffer_size) {
        return AGENT_ERROR_LIMIT;
    }

    buffer[(*used)++] = c;
    buffer[*used] = '\0';
    return AGENT_OK;
}

/* 校验 text 处起始的 UTF-8 序列长度。
 * 返回 0 表示该字节非法（孤立续字节/过长编码/超范围），1-4 为合法
 * 序列的字节数。严格排除过长编码（C0/C1/F5+）、代理对（ED A0-9F）
 * 与超 U+10FFFF（F4 90+）。 */
static int utf8_sequence_length(const char *text)
{
    unsigned char c = (unsigned char)text[0];
    unsigned char c1;
    unsigned char c2;
    size_t need;

    if (c < 0x80u)
      {
        return 1;
      }

    if (c >= 0xC2u && c <= 0xDFu)
      {
        need = 1;
      }
    else if ((c >= 0xE1u && c <= 0xECu) ||
             (c >= 0xEEu && c <= 0xEFu) ||
             c == 0xE0u || c == 0xEDu)
      {
        need = 2;
      }
    else if (c >= 0xF0u && c <= 0xF3u)
      {
        need = 3;
      }
    else if (c == 0xF4u)
      {
        need = 3;
      }
    else
      {
        return 0;   /* 0x80-0xBF 孤立续字节 / 0xC0,0xC1,0xF5+ 非法 */
      }

    c1 = (unsigned char)text[1];
    if ((c1 & 0xC0u) != 0x80u)
      {
        return 0;
      }

    /* 排除过长（E0 80-9F）、代理对（ED A0-BF）、超界（F0 80-8F、
     * F4 90-BF）。 */
    if ((c == 0xE0u && c1 < 0xA0u) ||
        (c == 0xEDu && c1 >= 0xA0u) ||
        (c == 0xF0u && c1 < 0x90u) ||
        (c == 0xF4u && c1 >= 0x90u))
      {
        return 0;
      }

    if (need >= 2)
      {
        c2 = (unsigned char)text[2];
        if ((c2 & 0xC0u) != 0x80u)
          {
            return 0;
          }
      }

    if (need >= 3)
      {
        unsigned char c3 = (unsigned char)text[3];

        if ((c3 & 0xC0u) != 0x80u)
          {
            return 0;
          }
      }

    return (int)(need + 1u);
}

static int append_json_string(char *buffer,
                              size_t buffer_size,
                              size_t *used,
                              const char *text,
                              int *invalid_utf8)
{
    int err;

    err = append_char(buffer, buffer_size, used, '"');
    if (err != AGENT_OK) {
        return err;
    }

    if (text) {
        while (*text) {
            unsigned char c = (unsigned char)*text;
            char escaped[7];
            const char *replacement = NULL;

            switch (c) {
            case '\\':
                replacement = "\\\\";
                break;
            case '"':
                replacement = "\\\"";
                break;
            case '\b':
                replacement = "\\b";
                break;
            case '\f':
                replacement = "\\f";
                break;
            case '\n':
                replacement = "\\n";
                break;
            case '\r':
                replacement = "\\r";
                break;
            case '\t':
                replacement = "\\t";
                break;
            default:
                if (c < 0x20u) {
                    snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    replacement = escaped;
                }
                break;
            }

            if (replacement) {
                err = append_raw(buffer, buffer_size, used, replacement);
                text++;
            } else if (c >= 0x80u) {
                /* UTF-8 兜底清洗：非法字节替换为 '?' 并计数（调用方
                 * 打日志点名消息角色）。合法多字节序列整段放行。 */
                int seq = utf8_sequence_length(text);

                if (seq == 0) {
                    err = append_char(buffer, buffer_size, used, '?');
                    text++;
                    if (invalid_utf8) {
                        (*invalid_utf8)++;
                    }
                } else {
                    int i;

                    for (i = 0; i < seq && err == AGENT_OK; i++) {
                        err = append_char(buffer, buffer_size, used,
                                          *text++);
                    }
                }
            } else {
                err = append_char(buffer, buffer_size, used, *text++);
            }
            if (err != AGENT_OK) {
                return err;
            }
        }
    }

    return append_char(buffer, buffer_size, used, '"');
}

static const char *message_role_name(agent_message_role_t role)
{
    switch (role) {
    case AGENT_MESSAGE_ROLE_USER:
        return "user";
    case AGENT_MESSAGE_ROLE_ASSISTANT:
        return "assistant";
    case AGENT_MESSAGE_ROLE_TOOL:
        return "tool";
    default:
        return NULL;
    }
}

static int append_content_message(char *buffer,
                                  size_t buffer_size,
                                  size_t *used,
                                  const agent_session_entry_t *entry)
{
    const char *role;
    int err;
    int bad_utf8 = 0;

    role = message_role_name(entry->role);
    if (!role) {
        return AGENT_ERROR_INVALID;
    }

    err = append_raw(buffer, buffer_size, used, "{\"role\":");
    if (err != AGENT_OK) {
        return err;
    }
    err = append_json_string(buffer, buffer_size, used, role, NULL);
    if (err != AGENT_OK) {
        return err;
    }

    if (entry->role == AGENT_MESSAGE_ROLE_TOOL) {
        err = append_raw(buffer, buffer_size, used, ",\"tool_call_id\":");
        if (err != AGENT_OK) {
            return err;
        }
        err = append_json_string(buffer, buffer_size, used,
                             entry->tool_call_id, &bad_utf8);
        if (err != AGENT_OK) {
            return err;
        }
    }

    err = append_raw(buffer, buffer_size, used, ",\"content\":");
    if (err != AGENT_OK) {
        return err;
    }
    err = append_json_string(buffer, buffer_size, used,
                             entry->content, &bad_utf8);
    if (err != AGENT_OK) {
        return err;
    }

    if (bad_utf8 > 0) {
        /* 点名日志：该消息内容带 N 个非法字节（已替换为 ?）。
         * 历史上工具 UAF 产出的 free-list 指针字节曾以 invalid
         * unicode 拒整轮请求；此兜底保证只损字节、不废轮次。 */
        syslog(LOG_WARNING,
               "[utf8-sanitize] role=%s content invalid_bytes=%d\n",
               role, bad_utf8);
    }

    return append_char(buffer, buffer_size, used, '}');
}

static int append_assistant_tool_calls_message(char *buffer,
                                               size_t buffer_size,
                                               size_t *used,
                                               const agent_session_entry_t *entry)
{
    uint32_t i;
    int err;
    int bad_utf8 = 0;

    err = append_raw(buffer,
                     buffer_size,
                     used,
                     "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[");
    if (err != AGENT_OK) {
        return err;
    }

    for (i = 0u; i < entry->tool_call_count; i++) {
        const agent_session_tool_call_t *call = &entry->tool_calls[i];

        if (i > 0u) {
            err = append_char(buffer, buffer_size, used, ',');
            if (err != AGENT_OK) {
                return err;
            }
        }

        err = append_raw(buffer, buffer_size, used, "{\"id\":");
        if (err != AGENT_OK) {
            return err;
        }
        err = append_json_string(buffer, buffer_size, used, call->id, NULL);
        if (err != AGENT_OK) {
            return err;
        }
        err = append_raw(buffer,
                         buffer_size,
                         used,
                         ",\"type\":\"function\",\"function\":{\"name\":");
        if (err != AGENT_OK) {
            return err;
        }
        err = append_json_string(buffer, buffer_size, used, call->name, NULL);
        if (err != AGENT_OK) {
            return err;
        }
        err = append_raw(buffer, buffer_size, used, ",\"arguments\":");
        if (err != AGENT_OK) {
            return err;
        }
        err = append_json_string(buffer, buffer_size, used,
                             call->arguments_json, &bad_utf8);
        if (err != AGENT_OK) {
            return err;
        }
        err = append_raw(buffer, buffer_size, used, "}}");
        if (err != AGENT_OK) {
            return err;
        }
    }

    if (bad_utf8 > 0) {
        syslog(LOG_WARNING,
               "[utf8-sanitize] role=assistant tool_args invalid_bytes=%d\n",
               bad_utf8);
    }

    return append_raw(buffer, buffer_size, used, "]}");
}

/* ── Session 查找 / 创建 ── */

/** 按 id 查找 session，不存在返回 NULL */
agent_session_t *agent_session_find(agent_t *agent, const char *session_id)
{
    uint32_t i;

    session_id = normalize_session_id(session_id);

    if (!agent || !session_id || !session_id_valid(session_id)) {
        return NULL;
    }

    for (i = 0u; i < agent->session_count; i++) {
        if (strcmp(agent->sessions[i].id, session_id) == 0) {
            return &agent->sessions[i];
        }
    }

    return NULL;
}

/**
 * 查找或创建 session。
 *
 * 不存在时自动创建并初始化。
 * session 数组已满时返回 NULL。
 */
agent_session_t *agent_session_find_or_create(agent_t *agent, const char *session_id)
{
    agent_session_t *session;

    session_id = normalize_session_id(session_id);

    if (!agent || !session_id || !session_id_valid(session_id)) {
        return NULL;
    }

    /* 先查找已有 session */
    session = agent_session_find(agent, session_id);
    if (session) {
        return session;
    }

    /* 创建新 session */
    if (agent->session_count >= CAGENT_MAX_SESSIONS) {
        return NULL;
    }

    session = &agent->sessions[agent->session_count];
    memset(session, 0, sizeof(*session));
    if (copy_string(session->id, sizeof(session->id), session_id, false, NULL) != AGENT_OK) {
        return NULL;
    }
    agent->session_count++;

    return session;
}

/* ── 消息追加 ── */

/**
 * 确保session有空间追加消息。
 *
 * 消息满时尝试淘汰最早的完整 turn。
 * 淘汰后仍满时返回 AGENT_ERROR_LIMIT。
 */
static int ensure_space(agent_t *agent, agent_session_t *session)
{
    int err;

    if (session->count < CAGENT_MAX_SESSION_MESSAGES) {
        return AGENT_OK;
    }

    /* 满了，尝试淘汰 */
    err = agent_session_evict_oldest_turn(agent, session);
    if (err != AGENT_OK) {
        return err;
    }

    /* 淘汰后仍满（理论上不应发生，但防御性检查） */
    if (session->count >= CAGENT_MAX_SESSION_MESSAGES) {
        return AGENT_ERROR_LIMIT;
    }

    return AGENT_OK;
}

/**
 * 追加 user 消息，标记 turn_boundary。
 *
 * 每个 user 消息开启一个新 turn。
 * 消息满时自动淘汰最早的完整 turn。
 */
int agent_session_add_user(agent_t *agent,
                           agent_session_t *session,
                           const char *content)
{
    agent_session_entry_t *entry;
    int err;

    if (!agent || !session || !content) {
        return AGENT_ERROR_INVALID;
    }

    if (session_has_unfinished_turn(session)) {
        return AGENT_ERROR_INVALID;
    }

    err = ensure_space(agent, session);
    if (err != AGENT_OK) {
        return err;
    }

    entry = &session->entries[session->count];
    memset(entry, 0, sizeof(*entry));

    entry->role = AGENT_MESSAGE_ROLE_USER;
    err = copy_string(entry->content,
                      sizeof(entry->content),
                      content,
                      false,
                      &entry->content_len);
    if (err != AGENT_OK) {
        memset(entry, 0, sizeof(*entry));
        return err;
    }
    entry->timestamp_ms = agent_runtime_now_ms(&agent->runtime);
    entry->turn_boundary = true;

    session->count++;
    return AGENT_OK;
}

/**
 * 追加 assistant final 消息（无 tool_calls）。
 *
 * 标记当前 turn 完成，递增 complete_turns 计数。
 */
int agent_session_add_assistant(agent_t *agent,
                                agent_session_t *session,
                                const char *content)
{
    agent_session_entry_t *entry;
    int err;

    if (!agent || !session) {
        return AGENT_ERROR_INVALID;
    }

    if (!session_has_unfinished_turn(session)
        || session_has_pending_tool_calls(session)) {
        return AGENT_ERROR_INVALID;
    }

    err = ensure_space(agent, session);
    if (err != AGENT_OK) {
        return err;
    }

    entry = &session->entries[session->count];
    memset(entry, 0, sizeof(*entry));

    entry->role = AGENT_MESSAGE_ROLE_ASSISTANT;
    err = copy_string(entry->content,
                      sizeof(entry->content),
                      content,
                      true,
                      &entry->content_len);
    if (err != AGENT_OK) {
        memset(entry, 0, sizeof(*entry));
        return err;
    }
    entry->timestamp_ms = agent_runtime_now_ms(&agent->runtime);
    entry->turn_boundary = false;
    /* tool_call_count = 0，表示 final 回复 */

    session->count++;
    session->complete_turns++;
    return AGENT_OK;
}

/**
 * 追加 assistant tool_calls 消息。
 *
 * tool_calls 数组内拷贝到 entry，不持有外部指针。
 * call_count 超过 CAGENT_SESSION_MAX_TOOL_CALLS 时返回 AGENT_ERROR_LIMIT。
 */
int agent_session_add_assistant_tool_calls(agent_t *agent,
                                           agent_session_t *session,
                                           const agent_tool_call_t *calls,
                                           size_t call_count)
{
    agent_session_entry_t *entry;
    size_t i;
    int err;

    if (!agent || !session || !calls || call_count == 0u) {
        return AGENT_ERROR_INVALID;
    }

    if (call_count > CAGENT_SESSION_MAX_TOOL_CALLS) {
        return AGENT_ERROR_LIMIT;
    }

    if (!session_has_unfinished_turn(session)
        || session_has_pending_tool_calls(session)) {
        return AGENT_ERROR_INVALID;
    }

    err = ensure_space(agent, session);
    if (err != AGENT_OK) {
        return err;
    }

    entry = &session->entries[session->count];
    memset(entry, 0, sizeof(*entry));

    entry->role = AGENT_MESSAGE_ROLE_ASSISTANT;
    entry->timestamp_ms = agent_runtime_now_ms(&agent->runtime);
    entry->turn_boundary = false;
    entry->tool_call_count = (uint32_t)call_count;

    for (i = 0u; i < call_count; i++) {
        agent_session_tool_call_t *dst = &entry->tool_calls[i];
        err = copy_string(dst->id, sizeof(dst->id), calls[i].id, false, NULL);
        if (err != AGENT_OK) {
            memset(entry, 0, sizeof(*entry));
            return err;
        }
        err = copy_string(dst->name, sizeof(dst->name), calls[i].name, false, NULL);
        if (err != AGENT_OK) {
            memset(entry, 0, sizeof(*entry));
            return err;
        }
        err = copy_string(dst->arguments_json,
                          sizeof(dst->arguments_json),
                          calls[i].arguments_json ? calls[i].arguments_json : "{}",
                          false,
                          NULL);
        if (err != AGENT_OK) {
            memset(entry, 0, sizeof(*entry));
            return err;
        }
    }

    session->count++;
    return AGENT_OK;
}

/**
 * 追加 tool result 消息。
 *
 * tool_call_id 关联 assistant tool_calls 中的调用 id。
 */
int agent_session_add_tool(agent_t *agent,
                           agent_session_t *session,
                           const char *tool_call_id,
                           const char *content)
{
    agent_session_entry_t *entry;
    int err;

    if (!agent || !session || !tool_call_id) {
        return AGENT_ERROR_INVALID;
    }

    if (!session_has_pending_tool_call(session, tool_call_id)) {
        return AGENT_ERROR_INVALID;
    }

    err = ensure_space(agent, session);
    if (err != AGENT_OK) {
        return err;
    }

    entry = &session->entries[session->count];
    memset(entry, 0, sizeof(*entry));

    entry->role = AGENT_MESSAGE_ROLE_TOOL;
    err = copy_string(entry->content,
                      sizeof(entry->content),
                      content,
                      true,
                      &entry->content_len);
    if (err != AGENT_OK) {
        memset(entry, 0, sizeof(*entry));
        return err;
    }
    err = copy_string(entry->tool_call_id,
                      sizeof(entry->tool_call_id),
                      tool_call_id,
                      false,
                      NULL);
    if (err != AGENT_OK) {
        memset(entry, 0, sizeof(*entry));
        return err;
    }
    entry->timestamp_ms = agent_runtime_now_ms(&agent->runtime);
    entry->turn_boundary = false;

    session->count++;
    return AGENT_OK;
}

/* ── Turn 淘汰 ── */

/**
 * 淘汰最早的完整 turn。
 *
 * 淘汰规则：
 *   1. 从头部扫描，找到第一个 turn_boundary（user message）
 *   2. 从该位置继续扫描，找到对应的 assistant final 消息
 *   3. 如果找到，删除从 user 到 assistant(final) 的所有消息
 *   4. 尾部未完成 turn（user 后无 assistant final）不参与淘汰
 *   5. 无完整 turn 时返回 AGENT_ERROR_LIMIT
 *
 * 删除通过 memmove 实现，保持消息顺序。
 */
int agent_session_evict_oldest_turn(agent_t *agent, agent_session_t *session)
{
    uint32_t turn_start;
    uint32_t turn_end;
    uint32_t i;
    bool found_final;

    CAGENT_UNUSED(agent);

    if (!session || session->count == 0u) {
        return AGENT_ERROR_LIMIT;
    }

    /* 从头部找到第一个完整 turn 的边界 */
    turn_start = UINT32_MAX;
    turn_end = 0u;
    found_final = false;

    for (i = 0u; i < session->count; i++) {
        if (session->entries[i].turn_boundary) {
            /* 找到 turn 起点 */
            turn_start = i;
            /* 继续找这个 turn 的终点（assistant final） */
            turn_end = i + 1u;
            while (turn_end < session->count) {
                if (session->entries[turn_end].turn_boundary) {
                    break;
                }
                if (entry_is_assistant_final(&session->entries[turn_end])) {
                    found_final = true;
                    break;
                }
                turn_end++;
            }
            break;
        }
    }

    /* 没有找到 turn 起点，或 turn 未完成 */
    if (turn_start == UINT32_MAX || !found_final) {
        return AGENT_ERROR_LIMIT;
    }

    /* turn_end 指向 assistant final，要删除 [turn_start, turn_end] 范围 */
    /* 删除条数 = turn_end - turn_start + 1 */
    uint32_t remove_count = turn_end - turn_start + 1u;
    uint32_t remaining = session->count - turn_start - remove_count;

    if (remaining > 0u) {
        memmove(&session->entries[turn_start],
                &session->entries[turn_end + 1u],
                remaining * sizeof(agent_session_entry_t));
    }

    session->count -= remove_count;

    /* 清零尾部残留 */
    memset(&session->entries[session->count], 0,
           remove_count * sizeof(agent_session_entry_t));

    if (session->complete_turns > 0u) {
        session->complete_turns--;
    }

    return AGENT_OK;
}

/**
 * 删除尾部未完成 turn。
 *
 * 用于 agent_run 失败恢复：如果本轮已经追加 user / tool_calls / tool result，
 * 但还没有形成 assistant final，则删除从最后一个 user turn_boundary 到尾部的
 * 所有消息。已经完成的历史 turn 保留，已经发生的完整 turn 淘汰也不回滚。
 */
int agent_session_drop_unfinished_tail(agent_t *agent, agent_session_t *session)
{
    uint32_t turn_start;
    uint32_t remove_count;
    uint32_t i;

    CAGENT_UNUSED(agent);

    if (!session) {
        return AGENT_ERROR_INVALID;
    }

    if (session->count == 0u) {
        return AGENT_OK;
    }

    turn_start = UINT32_MAX;
    for (i = session->count; i > 0u; i--) {
        agent_session_entry_t *entry = &session->entries[i - 1u];

        if (entry_is_assistant_final(entry)) {
            return AGENT_OK;
        }

        if (entry->turn_boundary) {
            turn_start = i - 1u;
            break;
        }
    }

    if (turn_start == UINT32_MAX) {
        return AGENT_OK;
    }

    remove_count = session->count - turn_start;
    memset(&session->entries[turn_start],
           0,
           remove_count * sizeof(agent_session_entry_t));
    session->count = turn_start;
    return AGENT_OK;
}

/* ── 清除 ── */

/** 清除 session 所有消息 */
int agent_session_clear_internal(agent_t *agent, agent_session_t *session)
{
    CAGENT_UNUSED(agent);

    if (!session) {
        return AGENT_ERROR_INVALID;
    }

    memset(session->entries, 0, sizeof(session->entries));
    session->count = 0u;
    session->complete_turns = 0u;
    return AGENT_OK;
}

/** 清除 agent 所有 session */
int agent_session_clear_all(agent_t *agent)
{
    uint32_t i;

    if (!agent) {
        return AGENT_ERROR_INVALID;
    }

    for (i = 0u; i < agent->session_count; i++) {
        memset(agent->sessions[i].entries, 0, sizeof(agent->sessions[i].entries));
        agent->sessions[i].count = 0u;
        agent->sessions[i].complete_turns = 0u;
    }

    return AGENT_OK;
}

/* ── 公共 API 实现 ── */

/** 清除指定 session 的所有消息 */
int agent_session_clear(agent_t *agent, const char *session_id)
{
    agent_session_t *session;

    if (!agent) {
        return AGENT_ERROR_INVALID;
    }

    session = agent_session_find(agent, session_id);
    if (!session) {
        return AGENT_ERROR_NOTFOUND;
    }

    return agent_session_clear_internal(agent, session);
}

/** 查询 session 数量 */
int agent_session_count(agent_t *agent, size_t *count)
{
    if (!agent || !count) {
        return AGENT_ERROR_INVALID;
    }

    *count = (size_t)agent->session_count;
    return AGENT_OK;
}

/**
 * 追加消息到 session（公共 API 入口）。
 *
 * 根据 message->role 分发到对应的内部 add 函数。
 * 对于 ASSISTANT 角色，无法通过此接口传递 tool_calls，
 * 应用层应通过 agent_run 间接使用 session。
 */
int agent_session_append(agent_t *agent,
                         const char *session_id,
                         const agent_message_t *message)
{
    agent_session_t *session;

    if (!agent || !message) {
        return AGENT_ERROR_INVALID;
    }

    session = agent_session_find_or_create(agent, session_id);
    if (!session) {
        return AGENT_ERROR_LIMIT;
    }

    switch (message->role) {
    case AGENT_MESSAGE_ROLE_USER:
        return agent_session_add_user(agent, session, message->content);

    case AGENT_MESSAGE_ROLE_ASSISTANT:
        return agent_session_add_assistant(agent, session, message->content);

    case AGENT_MESSAGE_ROLE_TOOL:
        return agent_session_add_tool(agent, session,
                                      message->tool_call_id, message->content);

    case AGENT_MESSAGE_ROLE_SYSTEM:
        /* system 消息由 context_builder 管理，不直接追加到 session */
        return AGENT_ERROR_INVALID;

    default:
        return AGENT_ERROR_INVALID;
    }
}

int agent_session_build_model_messages(agent_t *agent,
                                       agent_session_t *session,
                                       char *buffer,
                                       size_t buffer_size,
                                       size_t *written)
{
    uint32_t i;
    size_t used = 0u;
    int err;

    CAGENT_UNUSED(agent);

    if (!session || !buffer || buffer_size == 0u) {
        return AGENT_ERROR_INVALID;
    }

    buffer[0] = '\0';

    err = append_char(buffer, buffer_size, &used, '[');
    if (err != AGENT_OK) {
        return err;
    }

    for (i = 0u; i < session->count; i++) {
        const agent_session_entry_t *entry = &session->entries[i];

        if (entry->role == AGENT_MESSAGE_ROLE_SYSTEM) {
            continue;
        }

        if (used > 1u) {
            err = append_char(buffer, buffer_size, &used, ',');
            if (err != AGENT_OK) {
                return err;
            }
        }

        if (entry->role == AGENT_MESSAGE_ROLE_ASSISTANT
            && entry->tool_call_count > 0u) {
            err = append_assistant_tool_calls_message(buffer,
                                                      buffer_size,
                                                      &used,
                                                      entry);
        } else {
            err = append_content_message(buffer, buffer_size, &used, entry);
        }
        if (err != AGENT_OK) {
            return err;
        }
    }

    err = append_char(buffer, buffer_size, &used, ']');
    if (err != AGENT_OK) {
        return err;
    }

    if (written) {
        *written = used;
    }

    return AGENT_OK;
}
