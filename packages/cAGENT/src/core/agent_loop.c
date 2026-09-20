/* SPDX-License-Identifier: Apache-2.0 */
/**
 * ReAct 推理循环执行体。
 *
 * 由 agent_run() 调用，负责实际的迭代推理：
 *   1. session add user message
 *   2. for iter < max_steps:
 *      - check cancel / deadline
 *      - build context (context_builder)
 *      - build tool schema (tool_schema)
 *      - model.complete()
 *      - parse response
 *      - if final → return
 *      - if tool_calls → execute tools → continue
 *   3. return AGENT_ERROR_LIMIT
 *
 * 工具调用在 P1 阶段顺序执行。工具级错误会以 tool result JSON 写回
 * session，让模型在下一轮自行处理。
 */

#include "agent_internal.h"

#include "../runtime/runtime.h"
#include "../types_internal.h"

#include <stddef.h>
#include <string.h>

static const char *error_code_to_str(int code)
{
    switch (code) {
    case AGENT_OK:                   return "ok";
    case AGENT_ERROR_NOMEM:          return "nomem";
    case AGENT_ERROR_INVALID:        return "invalid";
    case AGENT_ERROR_BUSY:           return "busy";
    case AGENT_ERROR_LIMIT:          return "limit";
    case AGENT_ERROR_TIMEOUT:        return "timeout";
    case AGENT_ERROR_CANCELLED:      return "cancelled";
    case AGENT_ERROR_CONTEXT_OVERFLOW: return "context_overflow";
    case AGENT_ERROR_MODEL:          return "model_error";
    case AGENT_ERROR_NETWORK:        return "network";
    case AGENT_ERROR_TOOL:           return "tool_error";
    case AGENT_ERROR_POLICY_DENIED:  return "policy_denied";
    case AGENT_ERROR_NOTFOUND:       return "not_found";
    case AGENT_ERROR_NOTSUP:         return "not_supported";
    case AGENT_ERROR_PARSE:          return "parse_error";
    default:                         return "unknown";
    }
}

static int check_run_state(agent_t *agent)
{
    uint64_t now;

    if (!agent) {
        return AGENT_ERROR_INVALID;
    }

    if (agent->cancel_requested) {
        return AGENT_ERROR_CANCELLED;
    }

    now = agent_runtime_now_ms(&agent->runtime);
    if (agent->run_deadline_ms != 0u && now > agent->run_deadline_ms) {
        return AGENT_ERROR_TIMEOUT;
    }

    return AGENT_OK;
}

static int copy_final_output(agent_response_t *response, const char *content)
{
    size_t len;

    if (!response || !response->output || response->output_size == 0u) {
        return AGENT_ERROR_INVALID;
    }

    if (!content) {
        content = "";
    }

    len = strlen(content);
    if (len >= response->output_size) {
        response->output[0] = '\0';
        return AGENT_ERROR_LIMIT;
    }

    memcpy(response->output, content, len);
    /* UTF-8 字符边界回退：末字节非 ASCII 即整体丢弃该（不完整）
     * 多字节字符，避免非法序列随会话历史进入后续请求。 */
    while (len > 0 && ((unsigned char)response->output[len - 1] & 0x80) != 0u) {
        len--;
    }
    response->output[len] = '\0';
    return AGENT_OK;
}

static bool is_budget_error(int ret)
{
    return ret == AGENT_ERROR_LIMIT || ret == AGENT_ERROR_CONTEXT_OVERFLOW;
}

static size_t align_up_size(size_t value)
{
    const size_t align = sizeof(void *);

    return (value + align - 1u) & ~(align - 1u);
}

static size_t estimate_request_arena_size(void)
{
    return (size_t)CAGENT_SYSTEM_CONTEXT_BUFFER_SIZE +
           (size_t)CAGENT_TOOL_SCHEMA_BUFFER_SIZE +
           (size_t)CAGENT_MESSAGES_BUFFER_SIZE;
}

static int arena_begin(agent_t *agent, size_t required_size)
{
    size_t arena_size = (size_t)CAGENT_REQUEST_ARENA_SIZE;

    if (!agent) {
        return AGENT_ERROR_INVALID;
    }

    memset(&agent->request_arena, 0, sizeof(agent->request_arena));
    if (arena_size < required_size) {
        return AGENT_ERROR_LIMIT;
    }

    agent->request_arena.buffer =
        (unsigned char *)agent_runtime_malloc(&agent->runtime, arena_size);
    if (!agent->request_arena.buffer) {
        return AGENT_ERROR_NOMEM;
    }

    agent->request_arena.size = arena_size;
    return AGENT_OK;
}

static void arena_end(agent_t *agent)
{
    if (!agent) {
        return;
    }

    agent->stats.last_run_arena_peak_bytes = agent->request_arena.peak;
    if (agent->request_arena.peak > agent->stats.arena_peak_bytes) {
        agent->stats.arena_peak_bytes = agent->request_arena.peak;
    }

    if (agent->request_arena.buffer) {
        agent_runtime_free(&agent->runtime, agent->request_arena.buffer);
    }
    memset(&agent->request_arena, 0, sizeof(agent->request_arena));
}

static void *arena_alloc(agent_t *agent, size_t size)
{
    size_t aligned_used;
    size_t next_used;
    void *ptr;

    if (!agent || !agent->request_arena.buffer || size == 0u) {
        return NULL;
    }

    aligned_used = align_up_size(agent->request_arena.used);
    if (size > agent->request_arena.size - aligned_used) {
        return NULL;
    }

    next_used = aligned_used + size;
    ptr = agent->request_arena.buffer + aligned_used;
    agent->request_arena.used = next_used;
    if (next_used > agent->request_arena.peak) {
        agent->request_arena.peak = next_used;
    }

    memset(ptr, 0, size);
    return ptr;
}

static int allocate_loop_buffers(agent_t *agent,
                                 char **context,
                                 char **tools,
                                 char **messages)
{
    if (!agent || !context || !tools || !messages) {
        return AGENT_ERROR_INVALID;
    }

    *context = (char *)arena_alloc(agent, CAGENT_SYSTEM_CONTEXT_BUFFER_SIZE);
    *tools = (char *)arena_alloc(agent, CAGENT_TOOL_SCHEMA_BUFFER_SIZE);
    *messages = (char *)arena_alloc(agent, CAGENT_MESSAGES_BUFFER_SIZE);
    if (!*context || !*tools || !*messages) {
        *context = NULL;
        *tools = NULL;
        *messages = NULL;
        return AGENT_ERROR_LIMIT;
    }

    return AGENT_OK;
}

int agent_loop_run(agent_t *agent,
                   const agent_request_t *request,
                   agent_response_t *response)
{
    agent_session_t *session;
    agent_message_t user_message;
    char *context = NULL;
    char *tools_json = NULL;
    char *messages_json = NULL;
    uint32_t iter;
    int ret;

    if (!agent || !request || !response) {
        return AGENT_ERROR_INVALID;
    }

    if (!agent->model) {
        return AGENT_ERROR_INVALID;
    }

    ret = check_run_state(agent);
    if (ret != AGENT_OK) {
        return ret;
    }

    ret = arena_begin(agent, estimate_request_arena_size());
    if (ret != AGENT_OK) {
        return ret;
    }

    ret = allocate_loop_buffers(agent, &context, &tools_json, &messages_json);
    if (ret != AGENT_OK) {
        arena_end(agent);
        return ret;
    }

    session = agent_session_find_or_create(agent, request->session_id);
    if (!session) {
        arena_end(agent);
        return AGENT_ERROR_LIMIT;
    }

    memset(&user_message, 0, sizeof(user_message));
    user_message.role = AGENT_MESSAGE_ROLE_USER;
    user_message.content = request->input;
    ret = agent_session_append(agent, request->session_id, &user_message);
    if (ret == AGENT_ERROR_INVALID) {
        int cleanup_ret = agent_session_drop_unfinished_tail(agent, session);
        if (cleanup_ret == AGENT_OK) {
            ret = agent_session_append(agent, request->session_id, &user_message);
        }
    }
    if (ret != AGENT_OK) {
        arena_end(agent);
        return ret;
    }

    for (iter = 0u; iter < agent->limits.max_steps; iter++) {
        agent_model_request_t model_request;
        agent_model_response_t model_response;
        uint32_t budget_retry;

        ret = check_run_state(agent);
        if (ret != AGENT_OK) {
            break;
        }

        agent->stats.iterations++;
        agent_event_emit(agent,
                         AGENT_EVENT_ITERATION_START,
                         request,
                         iter + 1u,
                         AGENT_OK,
                         "step",
                         NULL,
                         NULL);

        for (budget_retry = 0u;
             budget_retry <= CAGENT_MAX_SESSION_MESSAGES;
             budget_retry++) {
            ret = agent_context_build(agent,
                                      request,
                                      context,
                                      CAGENT_SYSTEM_CONTEXT_BUFFER_SIZE,
                                      NULL);
            if (ret != AGENT_OK) {
                break;
            }

            ret = agent_tool_schema_build(agent,
                                          tools_json,
                                          CAGENT_TOOL_SCHEMA_BUFFER_SIZE,
                                          NULL);
            if (ret != AGENT_OK) {
                break;
            }

            ret = agent_session_build_model_messages(agent,
                                                     session,
                                                     messages_json,
                                                     CAGENT_MESSAGES_BUFFER_SIZE,
                                                     NULL);
            if (is_budget_error(ret)) {
                int evict_ret = agent_session_evict_oldest_turn(agent, session);
                if (evict_ret == AGENT_OK) {
                    continue;
                }
            }
            if (ret != AGENT_OK) {
                break;
            }

            memset(&model_request, 0, sizeof(model_request));
            model_request.context = context;
            model_request.tools_json = tools_json;
            model_request.messages_json = messages_json;
            model_request.session_id = request->session_id ? request->session_id
                                                           : AGENT_SESSION_DEFAULT_ID;
            model_request.input = request->input;
            model_request.trace_id = request->trace_id;
            model_request.timeout_ms = agent->limits.per_model_timeout_ms;
            model_request.max_output_tokens = agent->limits.max_output_tokens;

            memset(&model_response, 0, sizeof(model_response));
            agent_event_emit(agent,
                             AGENT_EVENT_MODEL_REQUEST,
                             request,
                             iter + 1u,
                             AGENT_OK,
                             "request",
                             NULL,
                             NULL);
            ret = agent_model_complete(agent->model,
                                       &agent->runtime,
                                       &model_request,
                                       &model_response);
            agent->stats.model_calls++;
            agent_event_emit(agent,
                             AGENT_EVENT_MODEL_RESPONSE,
                             request,
                             iter + 1u,
                             ret,
                             error_code_to_str(ret),
                             NULL,
                             NULL);
            if (is_budget_error(ret)) {
                int evict_ret = agent_session_evict_oldest_turn(agent, session);
                if (evict_ret == AGENT_OK) {
                    continue;
                }
            }
            break;
        }
        if (ret != AGENT_OK) {
            break;
        }
        ret = check_run_state(agent);
        if (ret != AGENT_OK) {
            break;
        }
        if (model_response.status != AGENT_OK) {
            ret = model_response.status ? model_response.status : AGENT_ERROR_MODEL;
            break;
        }

        if (model_response.tool_call_count > 0u) {
            size_t i;

            if (!model_response.tool_calls) {
                ret = AGENT_ERROR_INVALID;
                break;
            }

            ret = agent_session_add_assistant_tool_calls(agent,
                                                        session,
                                                        model_response.tool_calls,
                                                        model_response.tool_call_count);
            if (ret != AGENT_OK) {
                break;
            }

            for (i = 0u; i < model_response.tool_call_count; i++) {
                agent_tool_result_t tool_result;
                const agent_tool_call_t *call = &model_response.tool_calls[i];
                int tool_status;

                ret = check_run_state(agent);
                if (ret != AGENT_OK) {
                    break;
                }

                agent_event_emit(agent,
                                 AGENT_EVENT_TOOL_CALL,
                                 request,
                                 iter + 1u,
                                 AGENT_OK,
                                 "call",
                                 call->name,
                                 call->id);

                memset(&tool_result, 0, sizeof(tool_result));
                tool_status = agent_tool_execute(agent, call, &tool_result);

                agent_event_emit(agent,
                                 AGENT_EVENT_TOOL_RESULT,
                                 request,
                                 iter + 1u,
                                 tool_status,
                                 tool_result.error_message,
                                 call->name,
                                 call->id);

                ret = check_run_state(agent);
                if (ret != AGENT_OK) {
                    break;
                }

                ret = agent_session_add_tool(agent,
                                             session,
                                             call->id,
                                             tool_result.content_json);
                if (ret != AGENT_OK) {
                    break;
                }

                /* 快照最后一次成功的工具结果：若后续模型请求失败
                 * （网络/超时），response->output 仍带着它返回——
                 * 调用方的错误气泡可见"设备已动"，而不是空白。
                 * 正常成功路径会被 copy_final_output 覆盖。 */
                if (tool_status == AGENT_OK && response->output &&
                    response->output_size > 0u) {
                    snprintf(response->output, response->output_size,
                             "工具 %s 已执行：%.160s",
                             call->name ? call->name : "?",
                             tool_result.content_json ?
                                 tool_result.content_json : "");
                }
            }

            if (ret != AGENT_OK) {
                break;
            }
            continue;
        }

        ret = agent_session_add_assistant(agent, session, model_response.content);
        if (ret != AGENT_OK) {
            break;
        }

        ret = copy_final_output(response, model_response.content);
        break;
    }

    if (iter >= agent->limits.max_steps && ret == AGENT_OK) {
        ret = AGENT_ERROR_LIMIT;
    } else if (iter >= agent->limits.max_steps && ret == 0) {
        ret = AGENT_ERROR_LIMIT;
    }

    if (ret == AGENT_OK) {
        arena_end(agent);
        return AGENT_OK;
    }

    agent_session_drop_unfinished_tail(agent, session);
    arena_end(agent);
    return ret ? ret : AGENT_ERROR_LIMIT;
}
