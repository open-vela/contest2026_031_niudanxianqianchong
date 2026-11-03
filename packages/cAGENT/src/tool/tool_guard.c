/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Synchronous admission and execution; never retries a device operation. */
#include "core/core_internal.h"
#include "policy/policy_internal.h"
#include "runtime/runtime_internal.h"
#include <string.h>

typedef struct {
    agent_t* agent;
    const agent_tool_context_t* context;
    char* data;
    size_t capacity, used;
    agent_error_t status;
} tool_output_t;

static agent_error_t poll_call(agent_t* agent, const agent_tool_context_t* context)
{
    if (agent_cancel_token_is_set(context->cancel))
        return AGENT_ERROR_CANCELLED;
    if (context->deadline_ms &&
        agent_runtime_now_ms(&agent->config.runtime) >= context->deadline_ms)
        return AGENT_ERROR_TIMEOUT;
    return AGENT_OK;
}

static agent_error_t collect_output(void* data, agent_string_view_t text)
{
    tool_output_t* output = data;
    if (output->status != AGENT_OK)
        return output->status;
    output->status = poll_call(output->agent, output->context);
    if (output->status != AGENT_OK)
        return output->status;
    if ((text.size && !text.data) ||
        agent_tool_overlaps(text.data, text.size, output->data, output->capacity))
        output->status = AGENT_ERROR_INVALID;
    else if (text.size > AGENT_MAX_TOOL_OUTPUT_BYTES - output->used)
        output->status = AGENT_ERROR_LIMIT;
    else if (text.size && memchr(text.data, '\0', text.size))
        output->status = AGENT_ERROR_INVALID;
    if (output->status != AGENT_OK)
        return output->status;
    if (text.size)
        memcpy(output->data + output->used, text.data, text.size);
    output->used += text.size;
    output->data[output->used] = '\0';
    return AGENT_OK;
}

/* Scratch is writable; persistent Core and registered metadata are not. */
static bool writable_buffer(const agent_t* agent, const agent_tool_context_t* context,
                            const void* data, size_t bytes)
{
    const agent_string_view_t fields[] = {context->call.id, context->call.name,
                                          context->call.arguments_json, context->session_id,
                                          context->trace_id};
    size_t i;
    size_t persistent = (size_t)(agent->scratch.base - (unsigned char*)agent->workspace);
    if (!data || bytes > UINTPTR_MAX - (uintptr_t)data ||
        agent_tool_overlaps(data, bytes, agent->workspace, persistent) ||
        !agent_tool_registry_buffer_safe(agent->tools, data, bytes) ||
        agent_tool_overlaps(data, bytes, context, sizeof(*context)) ||
        agent_tool_overlaps(data, bytes, context->limits, sizeof(*context->limits)) ||
        (context->cancel &&
         agent_tool_overlaps(data, bytes, context->cancel, sizeof(*context->cancel))))
        return false;
    for (i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i)
    {
        if (agent_tool_overlaps(data, bytes, fields[i].data, fields[i].size))
            return false;
    }
    return true;
}

static agent_error_t validate_identifier(agent_string_view_t id, bool required)
{
    if ((required && !id.size) || (id.size && !id.data))
        return AGENT_ERROR_INVALID;
    if (id.size > AGENT_MAX_IDENTIFIER_BYTES)
        return AGENT_ERROR_LIMIT;
    return agent_tool_text_validate(id);
}

agent_error_t agent_tool_invoke_observed(agent_t* agent, const agent_tool_context_t* context,
                                uint32_t remaining_calls, char* output, size_t capacity,
                                agent_tool_execution_t* execution, const agent_tool_observer_t* observer)
{
    const agent_tool_t* tool;
    agent_tool_context_t effective;
    agent_policy_request_t policy;
    tool_output_t collected;
    agent_text_sink_t sink;
    agent_error_t status;
    size_t required;
    uint64_t now;

    if (!agent || !context || !context->limits || !execution || !output ||
        !writable_buffer(agent, context, output, capacity) ||
        !writable_buffer(agent, context, execution, sizeof(*execution)) ||
        agent_tool_overlaps(execution, sizeof(*execution), output, capacity))
        return AGENT_ERROR_INVALID;
    if (agent->in_callback)
        return AGENT_ERROR_BUSY;
    memset(execution, 0, sizeof(*execution));
    execution->handler_status = AGENT_ERROR_NOT_SUPPORTED;
    execution->output = agent_string_view(output, 0u);
    if (capacity)
        output[0] = '\0';
    if (agent->state != AGENT_CORE_ACTIVE)
        return execution->status = AGENT_ERROR_STATE;
    if (!agent->tools)
        return execution->status = AGENT_ERROR_NOT_SUPPORTED;
    if (agent_size_add(AGENT_MAX_TOOL_OUTPUT_BYTES, 1u, &required) != AGENT_OK)
        return execution->status = AGENT_ERROR_LIMIT;
    if (capacity < required)
        return execution->status = AGENT_ERROR_CAPACITY;
    if (!agent_tool_valid_name(context->call.name))
        return execution->status = AGENT_ERROR_INVALID;
    status = validate_identifier(context->call.id, true);
    if (status == AGENT_OK)
        status = validate_identifier(context->session_id, false);
    if (status == AGENT_OK)
        status = validate_identifier(context->trace_id, false);
    if (status != AGENT_OK)
        return execution->status = status;
    tool = agent_tool_registry_find(agent->tools, context->call.name);
    if (!tool)
        return execution->status = AGENT_ERROR_NOT_FOUND;
    if (tool->flags & (AGENT_TOOL_DISABLED | AGENT_TOOL_HIDDEN))
        return execution->status = AGENT_ERROR_POLICY_DENIED;

    effective = *context;
    agent->in_callback = true;
    now = agent_runtime_now_ms(&agent->config.runtime);
    if (effective.limits->per_tool_timeout_ms)
    {
        uint64_t duration = effective.limits->per_tool_timeout_ms;
        uint64_t deadline = duration > UINT64_MAX - now ? UINT64_MAX : now + duration;
        if (!effective.deadline_ms || deadline < effective.deadline_ms)
            effective.deadline_ms = deadline;
    }
    status = poll_call(agent, &effective);
    if (status == AGENT_OK && (!remaining_calls || !effective.limits->max_tool_calls))
        status = AGENT_ERROR_LIMIT;
    if (status == AGENT_OK)
    {
        status =
            agent_tool_object_validate(effective.call.arguments_json, AGENT_MAX_ARGUMENTS_BYTES);
        if (status == AGENT_ERROR_INVALID || status == AGENT_ERROR_PARSE)
            status = AGENT_ERROR_TOOL_ARGUMENT;
    }
    if (status == AGENT_OK)
        status = poll_call(agent, &effective);
    if (status == AGENT_OK && tool->validate)
    {
        status = tool->validate(tool->user_data, &effective);
        if (status == AGENT_ERROR_INVALID || status == AGENT_ERROR_PARSE || status == AGENT_ERROR ||
            status > AGENT_OK)
            status = AGENT_ERROR_TOOL_ARGUMENT;
    }
    if (status == AGENT_OK)
        status = poll_call(agent, &effective);
    if (status == AGENT_OK)
    {
        policy.tool = tool;
        policy.context = &effective;
        policy.source = AGENT_CALL_SOURCE_MODEL;
        if ((tool->flags & AGENT_TOOL_REQUIRES_CONFIRM) ||
            agent_policy_evaluate(agent->policy, agent->policy_context, &policy) !=
                AGENT_POLICY_ALLOW)
            status = AGENT_ERROR_POLICY_DENIED;
    }
    if (status == AGENT_OK)
        status = poll_call(agent, &effective);
    if (status == AGENT_OK)
    {
        collected = (tool_output_t){agent, &effective, output, capacity, 0u, AGENT_OK};
        sink = (agent_text_sink_t){collect_output, &collected};
        execution->handler_called = true;
        if (observer && observer->begin) {
            agent->in_callback = false;
            observer->begin(observer->data, &effective);
            agent->in_callback = true;
        }
        execution->handler_status = tool->execute(tool->user_data, &effective, &sink);
        if (execution->handler_status == AGENT_ERROR || execution->handler_status > AGENT_OK)
            execution->handler_status = AGENT_ERROR_TOOL_FAILED;
        execution->output = agent_string_view(output, collected.used);
        if (collected.status == AGENT_OK)
            collected.status = agent_tool_text_validate(execution->output);
        execution->output_status = collected.status;
        status = poll_call(agent, &effective);
        if (status == AGENT_OK)
            status = collected.status;
        if (status == AGENT_OK)
            status = execution->handler_status;
    }
    agent->in_callback = false;
    execution->status = status;
    if (execution->handler_called && observer && observer->end)
        observer->end(observer->data, &effective, execution);
    return status;
}

agent_error_t agent_tool_invoke(agent_t* agent, const agent_tool_context_t* context,
                                uint32_t remaining_calls, char* output, size_t capacity,
                                agent_tool_execution_t* execution)
{
    return agent_tool_invoke_observed(agent, context, remaining_calls, output, capacity, execution, NULL);
}
