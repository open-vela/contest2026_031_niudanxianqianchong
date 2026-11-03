/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Synchronous driver: retain facts, rebuild projections, never retry device effects. */
#include "core/core_internal.h"
#include "core/text_internal.h"
#include "model/model_internal.h"
#include "runtime/runtime_internal.h"
#include <string.h>

typedef struct {
    agent_t* agent;
    agent_request_t request;
    agent_limits_t limits;
    agent_cancel_token_t cancel;
    agent_run_summary_t summary;
    uint64_t started, deadline;
    agent_error_t session_status;
} run_t;

typedef struct {
    run_t* run;
    agent_arena_t arena;
    size_t start, arguments, text_size, count;
    uint64_t deadline;
    agent_error_t status;
    agent_tool_call_view_t calls[AGENT_MAX_MODEL_TOOL_CALLS ? AGENT_MAX_MODEL_TOOL_CALLS : 1u];
} model_output_t;

static void increment(uint32_t* value)
{
    if (*value != UINT32_MAX)
        ++*value;
}

static uint64_t deadline_at(uint64_t now, uint32_t duration, uint64_t outer)
{
    uint64_t result;
    if (!duration)
        return outer;
    result = duration > UINT64_MAX - now ? UINT64_MAX : now + duration;
    return !outer || result < outer ? result : outer;
}

static agent_error_t poll(run_t* run, uint64_t deadline)
{
    if (agent_cancel_token_is_set(&run->cancel))
        return AGENT_ERROR_CANCELLED;
    if (deadline && agent_runtime_now_ms(&run->agent->config.runtime) >= deadline)
        return AGENT_ERROR_TIMEOUT;
    return AGENT_OK;
}

static bool publish_cancel(run_t* run, bool active)
{
    const agent_sync_t* sync = &run->agent->config.runtime.cancel_sync;
    bool requested;
    if (sync->enter)
        sync->enter(sync->context);
    requested = run->cancel.requested;
    run->agent->active_cancel = active ? &run->cancel : NULL;
    if (sync->leave)
        sync->leave(sync->context);
    return requested;
}

static void emit(run_t* run, agent_event_type_t type, agent_error_t status,
                 const agent_tool_call_view_t* call)
{
    agent_event_t event = {0};
    event.type = type;
    event.timestamp_ms = agent_runtime_now_ms(&run->agent->config.runtime);
    event.session_id = run->request.session_id;
    event.trace_id = run->request.trace_id;
    event.status = status;
    event.summary = run->summary;
    if (call)
        event.tool_call = *call;
    agent_core_emit(run->agent, &event);
}

static agent_error_t collect_text(void* context, agent_string_view_t text)
{
    model_output_t* output = context;
    if (output->status != AGENT_OK)
        return output->status;
    output->status = poll(output->run, output->deadline);
    if (output->status != AGENT_OK)
        return output->status;
    if ((text.size && !text.data) ||
        agent_bytes_overlap(text.data, text.size, output->arena.base + output->start,
                            output->arena.capacity - output->start))
        return output->status = AGENT_ERROR_MODEL_PARSE;
    if (text.size > AGENT_MAX_MODEL_OUTPUT_BYTES - output->text_size)
        return output->status = AGENT_ERROR_LIMIT;
    if (text.size)
        memcpy(output->arena.base + output->start + output->text_size, text.data, text.size);
    output->text_size += text.size;
    return AGENT_OK;
}

static agent_error_t copy_field(model_output_t* output, agent_string_view_t text,
                                agent_string_view_t* result)
{
    void* memory;
    agent_error_t status = agent_arena_take(&output->arena, text.size, 1u, &memory);
    if (status != AGENT_OK)
        return status;
    memcpy(memory, text.data, text.size);
    *result = agent_string_view(memory, text.size);
    return AGENT_OK;
}

static agent_error_t collect_call(void* context, const agent_tool_call_view_t* call)
{
    model_output_t* output = context;
    agent_tool_call_view_t* copied;
    size_t i;
    if (output->status != AGENT_OK)
        return output->status;
    output->status = poll(output->run, output->deadline);
    if (output->status != AGENT_OK)
        return output->status;
    if (!call)
        return output->status = AGENT_ERROR_MODEL_PARSE;
    if (call->id.size > AGENT_MAX_IDENTIFIER_BYTES ||
        call->arguments_json.size > AGENT_MAX_ARGUMENTS_BYTES ||
        output->count == AGENT_MAX_MODEL_TOOL_CALLS)
        return output->status = AGENT_ERROR_LIMIT;
    if (!call->id.size || !call->arguments_json.size || !agent_tool_valid_name(call->name) ||
        agent_text_validate(call->id) != AGENT_OK ||
        agent_text_validate(call->arguments_json) != AGENT_OK)
        return output->status = AGENT_ERROR_MODEL_PARSE;
    {
        const agent_string_view_t fields[] = {call->id, call->name, call->arguments_json};
        for (i = 0u; i < 3u; ++i)
            if (agent_bytes_overlap(fields[i].data, fields[i].size,
                                    output->arena.base + output->start,
                                    output->arena.capacity - output->start))
                return output->status = AGENT_ERROR_MODEL_PARSE;
    }
    for (i = 0u; i < output->count; ++i)
        if (call->id.size == output->calls[i].id.size &&
            !memcmp(call->id.data, output->calls[i].id.data, call->id.size))
            return output->status = AGENT_ERROR_MODEL_PARSE;
    copied = &output->calls[output->count];
    output->status = copy_field(output, call->id, &copied->id);
    if (output->status == AGENT_OK)
        output->status = copy_field(output, call->name, &copied->name);
    if (output->status == AGENT_OK)
        output->status = copy_field(output, call->arguments_json, &copied->arguments_json);
    if (output->status == AGENT_OK)
        ++output->count;
    return output->status;
}

/* Compact the copied response into the released projection; no borrowed provider data survives. */
static agent_error_t retain_output(model_output_t* output, agent_message_view_t* message)
{
    agent_arena_t* arena = &output->run->agent->scratch;
    size_t i, payload = output->arena.used - output->arguments;
    size_t target = arena->used;
    void* memory;
    agent_error_t status;
    for (i = 0u; i < output->count; ++i)
    {
        agent_string_view_t* fields[] = {&output->calls[i].id, &output->calls[i].name,
                                         &output->calls[i].arguments_json};
        size_t j;
        for (j = 0u; j < 3u; ++j)
            fields[j]->data =
                (char*)arena->base + target + output->text_size +
                ((const unsigned char*)fields[j]->data - arena->base - output->arguments);
    }
    memmove(arena->base + output->start + output->text_size, arena->base + output->arguments,
            payload);
    memmove(arena->base + target, arena->base + output->start, output->text_size + payload);
    arena->used = target + output->text_size + payload;
    memset(message, 0, sizeof(*message));
    message->role = AGENT_MESSAGE_ROLE_ASSISTANT;
    message->content = agent_string_view((char*)arena->base + target, output->text_size);
    if (output->count)
    {
        status = agent_arena_take(arena, output->count * sizeof(output->calls[0]),
                                  AGENT_ALIGNOF(agent_tool_call_view_t), &memory);
        if (status != AGENT_OK)
            return status;
        memcpy(memory, output->calls, output->count * sizeof(output->calls[0]));
        message->tool_calls = memory;
        message->tool_call_count = output->count;
    }
    return AGENT_OK;
}

static agent_error_t model_step(run_t* run, agent_context_turn_t* context,
                                agent_session_turn_t* session, agent_message_view_t* message)
{
    agent_context_projection_t projection;
    model_output_t output = {0};
    agent_model_sink_t sink = {collect_text, collect_call, &output};
    agent_error_t status, released, checked;
    size_t reserve, descriptors;
    void* memory;
    uint64_t now;
    agent_t* agent = run->agent;
    status = agent_size_multiply(AGENT_MAX_MODEL_TOOL_CALLS, sizeof(agent_tool_call_view_t),
                                 &descriptors);
    if (status == AGENT_OK)
        status =
            agent_size_add(descriptors, AGENT_ALIGNOF(agent_tool_call_view_t) - 1u, &descriptors);
    if (status == AGENT_OK)
        status = agent_size_add(descriptors,
                                AGENT_MAX_MODEL_OUTPUT_BYTES ? AGENT_MAX_MODEL_OUTPUT_BYTES : 1u,
                                &reserve);
    if (status != AGENT_OK)
        return status;
    status = agent_context_project(context, session,
                                   run->limits.max_tool_calls - run->summary.tool_calls, reserve,
                                   &projection);
    if (status != AGENT_OK)
        return status;
    output.run = run;
    output.arena = agent->scratch;
    output.arena.capacity -= descriptors;
    output.start = output.arena.used;
    status = agent_arena_take(&output.arena,
                              AGENT_MAX_MODEL_OUTPUT_BYTES ? AGENT_MAX_MODEL_OUTPUT_BYTES : 1u, 1u,
                              &memory);
    output.arguments = output.arena.used;
    if (status == AGENT_OK)
    {
        now = agent_runtime_now_ms(&agent->config.runtime);
        output.deadline = deadline_at(now, run->limits.per_model_timeout_ms, run->deadline);
        projection.request.deadline_ms = output.deadline;
        projection.request.timeout_ms = !output.deadline         ? 0u
                                        : output.deadline <= now ? 1u
                                        : output.deadline - now > UINT32_MAX
                                            ? UINT32_MAX
                                            : (uint32_t)(output.deadline - now);
        status = poll(run, output.deadline);
    }
    if (status == AGENT_OK)
    {
        increment(&run->summary.model_calls);
        increment(&agent->stats.model_calls);
        increment(&agent->stats.iterations);
        emit(run, AGENT_EVENT_MODEL_BEGIN, AGENT_OK, NULL);
        agent->in_callback = true;
        status = agent_model_dispatch(agent->model, &projection.request, &sink);
        agent->in_callback = false;
        checked = poll(run, output.deadline);
        if (checked != AGENT_OK)
            status = checked;
        else if (output.status != AGENT_OK)
            status = output.status;
        else if (status == AGENT_ERROR || status > AGENT_OK)
            status = AGENT_ERROR_MODEL_FAILED;
        if (status == AGENT_OK &&
            agent_text_validate(agent_string_view((char*)output.arena.base + output.start,
                                                  output.text_size)) != AGENT_OK)
            status = AGENT_ERROR_MODEL_PARSE;
        emit(run, AGENT_EVENT_MODEL_END, status, NULL);
    }
    if (output.arena.peak > agent->scratch.peak)
        agent->scratch.peak = output.arena.peak;
    released = agent_context_release(&projection);
    if (status == AGENT_OK)
        status = released;
    if (status == AGENT_OK)
        status = retain_output(&output, message);
    return status;
}

static void tool_begin(void* data, const agent_tool_context_t* context)
{
    run_t* run = data;
    increment(&run->summary.tool_calls);
    increment(&run->agent->stats.tool_calls);
    run->summary.tools_executed = true;
    emit(run, AGENT_EVENT_TOOL_BEGIN, AGENT_OK, &context->call);
}

static void tool_end(void* data, const agent_tool_context_t* context,
                     const agent_tool_execution_t* execution)
{
    run_t* run = data;
    increment(execution->status == AGENT_OK ? &run->summary.tool_succeeded
                                            : &run->summary.tool_failed);
    emit(run, AGENT_EVENT_TOOL_END, execution->status, &context->call);
}

static agent_error_t append(run_t* run, agent_session_turn_t* session,
                            const agent_message_view_t* message)
{
    agent_error_t status;
    run->agent->in_callback = true;
    status = agent_session_append_owned(session, message);
    run->agent->in_callback = false;
    if (status != AGENT_OK && run->session_status == AGENT_OK)
        run->session_status = status;
    return status;
}

static agent_error_t tool_batch(run_t* run, agent_session_turn_t* session,
                                const agent_message_view_t* message)
{
    size_t i, capacity, bytes;
    agent_error_t status;
    agent_t* agent = run->agent;
    agent_tool_observer_t observer = {tool_begin, tool_end, run};
    status = agent_size_add(AGENT_MAX_TOOL_OUTPUT_BYTES, 1u, &capacity);
    if (status == AGENT_OK)
        status = agent_size_multiply(message->tool_call_count, capacity, &bytes);
    if (status != AGENT_OK)
        return status;
    if (bytes > agent->scratch.capacity - agent->scratch.used)
        return AGENT_ERROR_CAPACITY;
    for (i = 0u; i < message->tool_call_count; ++i)
    {
        agent_tool_context_t context = {0};
        agent_tool_execution_t execution = {0};
        agent_message_view_t result = {0};
        void* buffer;
        size_t mark = agent->scratch.used;
        status = poll(run, run->deadline);
        if (status != AGENT_OK)
            return status;
        status = agent_arena_take(&agent->scratch, capacity, 1u, &buffer);
        if (status != AGENT_OK)
            return status;
        context.call = message->tool_calls[i];
        context.session_id = run->request.session_id;
        context.trace_id = run->request.trace_id;
        context.limits = &run->limits;
        context.cancel = &run->cancel;
        context.deadline_ms = run->deadline;
        context.request_user_data = run->request.user_data;
        status = agent_tool_invoke_observed(agent, &context,
                                            run->limits.max_tool_calls - run->summary.tool_calls,
                                            buffer, capacity, &execution, &observer);
        if (!execution.handler_called)
            increment(&run->summary.tool_denied);
        if (status == AGENT_ERROR_CANCELLED || status == AGENT_ERROR_TIMEOUT ||
            status == AGENT_ERROR_LIMIT || status == AGENT_ERROR_CAPACITY)
            return status;
        result.role = AGENT_MESSAGE_ROLE_TOOL;
        result.tool_call_id = context.call.id;
        result.content = execution.output;
        if (status != AGENT_OK)
        {
            const char* error = agent_error_str(status);
            size_t length = strlen(error);
            if (length >= capacity)
                return AGENT_ERROR_CAPACITY;
            memcpy(buffer, error, length + 1u);
            result.content = agent_string_view(buffer, length);
        }
        agent->scratch.used = mark + result.content.size + 1u;
        status = append(run, session, &result);
        if (status != AGENT_OK)
            return status;
    }
    return AGENT_OK;
}

static bool output_safe(const agent_t* agent, const agent_request_t* request, const void* data,
                        size_t bytes)
{
    size_t i;
    const agent_string_view_t fields[] = {request->input, request->session_id, request->trace_id,
                                          agent->config.system_prompt};
    if ((!data && bytes) || bytes > UINTPTR_MAX - (uintptr_t)data ||
        agent_bytes_overlap(data, bytes, agent->workspace, agent->workspace_size) ||
        agent_bytes_overlap(data, bytes, request, sizeof(*request)) ||
        (request->limits &&
         agent_bytes_overlap(data, bytes, request->limits, sizeof(*request->limits))) ||
        !agent_tool_registry_buffer_safe(agent->tools, data, bytes))
        return false;
    for (i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i)
        if (agent_bytes_overlap(data, bytes, fields[i].data, fields[i].size))
            return false;
    if (agent->skills)
        for (i = 0u; i < agent->skills->count; ++i)
        {
            const agent_skill_t* skill = &agent->skills->entries[i];
            if (agent_bytes_overlap(data, bytes, skill->name.data, skill->name.size) ||
                agent_bytes_overlap(data, bytes, skill->description.data,
                                    skill->description.size) ||
                agent_bytes_overlap(data, bytes, skill->content.data, skill->content.size))
                return false;
        }
    if (agent->contexts)
        for (i = 0u; i < agent->contexts->count; ++i)
        {
            const agent_context_entry_t* entry = &agent->contexts->entries[i];
            if (agent_bytes_overlap(data, bytes, entry->provider.name.data,
                                    entry->provider.name.size) ||
                (entry->memory &&
                 agent_bytes_overlap(data, bytes, entry->key.id.data, entry->key.id.size)))
                return false;
        }
    return true;
}

static void deliver(agent_response_t* response, agent_string_view_t final)
{
    size_t size = response->output_size ? response->output_size - 1u : 0u;
    if (size > final.size)
        size = final.size;
    while (size && size < final.size && ((unsigned char) final.data[size] & 0xc0u) == 0x80u)
        --size;
    if (size)
        memcpy(response->output, final.data, size);
    if (response->output_size)
        response->output[size] = '\0';
    response->output_written = size;
    response->output_required = final.size;
    response->output_truncated = size < final.size;
    response->delivery_status = response->output_truncated ? AGENT_ERROR_TRUNCATED : AGENT_OK;
}

agent_error_t agent_run(agent_t* agent, const agent_request_t* request, agent_response_t* response)
{
    run_t run = {0};
    agent_session_turn_t* session = NULL;
    agent_context_turn_t* context = NULL;
    agent_string_view_t final = {0};
    agent_error_t status = agent_core_require_idle(agent), finished;
    char* output;
    size_t output_size;
    uint32_t step;
    uint64_t now;
    if (status != AGENT_OK)
        return status;
    if (!request || !response || !output_safe(agent, request, response, sizeof(*response)) ||
        !output_safe(agent, request, response->output, response->output_size) ||
        agent_bytes_overlap(response, sizeof(*response), response->output, response->output_size))
        return AGENT_ERROR_INVALID;
    /* Resetting scratch must not invalidate the request's borrowed inputs. */
    if (agent_bytes_overlap(request, sizeof(*request), agent->workspace, agent->workspace_size) ||
        agent_bytes_overlap(request->input.data, request->input.size, agent->workspace,
                            agent->workspace_size) ||
        agent_bytes_overlap(request->session_id.data, request->session_id.size, agent->workspace,
                            agent->workspace_size) ||
        agent_bytes_overlap(request->trace_id.data, request->trace_id.size, agent->workspace,
                            agent->workspace_size))
        return AGENT_ERROR_INVALID;
    run.agent = agent;
    run.request = *request;
    run.limits = request->limits ? *request->limits : agent->config.limits;
    if (!run.request.session_id.size)
        run.request.session_id = agent_string_view("default", 7u);
    output = response->output;
    output_size = response->output_size;
    memset(response, 0, sizeof(*response));
    response->output = output;
    response->output_size = output_size;
    if (output_size)
        output[0] = '\0';
    if (agent->state != AGENT_CORE_READY)
        status = AGENT_ERROR_STATE;
    else if (!agent->model)
        status = AGENT_ERROR_NOT_SUPPORTED;
    else if (run.request.input.size > AGENT_MAX_INPUT_BYTES ||
             run.request.session_id.size > AGENT_MAX_IDENTIFIER_BYTES ||
             run.request.trace_id.size > AGENT_MAX_IDENTIFIER_BYTES)
        status = AGENT_ERROR_LIMIT;
    else if (!run.limits.max_steps || !run.request.input.size ||
             agent_text_validate(run.request.input) != AGENT_OK ||
             agent_text_validate(run.request.session_id) != AGENT_OK ||
             agent_text_validate(run.request.trace_id) != AGENT_OK)
        status = AGENT_ERROR_INVALID;
    if (status != AGENT_OK)
    {
        response->stats = agent->stats;
        return response->status = status;
    }
    run.request.limits = &run.limits;
    agent_cancel_token_init(&run.cancel, &agent->config.runtime.cancel_sync);
    agent->state = AGENT_CORE_ACTIVE;
    agent->scratch.used = agent->scratch.peak = 0u;
    run.started = agent_runtime_now_ms(&agent->config.runtime);
    run.deadline = deadline_at(run.started, run.limits.timeout_ms, 0u);
    publish_cancel(&run, true);
    increment(&agent->stats.runs);
    emit(&run, AGENT_EVENT_TURN_BEGIN, AGENT_OK, NULL);
    status = poll(&run, run.deadline);
    if (status == AGENT_OK)
    {
        agent->in_callback = true;
        status = agent_session_turn_open(
            &session, &agent->scratch, agent->has_session_storage ? &agent->session_storage : NULL,
            run.request.session_id, run.request.input);
        agent->in_callback = false;
        run.session_status = status;
    }
    if (status == AGENT_OK)
        status = agent_context_prepare(agent, &run.request, &run.cancel, run.deadline, &context);
    for (step = 0u; status == AGENT_OK && step < run.limits.max_steps; ++step)
    {
        agent_message_view_t message;
        status = poll(&run, run.deadline);
        if (status == AGENT_OK)
            status = model_step(&run, context, session, &message);
        if (status != AGENT_OK)
            break;
        if (!message.tool_call_count)
        {
            run.summary.final_valid = true;
            final = message.content;
            status = append(&run, session, &message);
            break;
        }
        if (message.tool_call_count > run.limits.max_tool_calls - run.summary.tool_calls ||
            message.tool_call_count + 2u >
                AGENT_MAX_PROJECTED_MESSAGES - agent_session_current_count(session))
        {
            status = AGENT_ERROR_LIMIT;
            break;
        }
        status = append(&run, session, &message);
        if (status == AGENT_OK)
            status = tool_batch(&run, session, &message);
    }
    if (status == AGENT_OK && !run.summary.final_valid)
        status = AGENT_ERROR_LIMIT;
    if (status == AGENT_OK)
        status = poll(&run, run.deadline);
    if (session)
    {
        agent->in_callback = true;
        finished = agent_session_turn_finish(
            session, status == AGENT_OK ? AGENT_SESSION_TURN_COMPLETE : AGENT_SESSION_TURN_ABORTED);
        agent->in_callback = false;
        if (run.session_status == AGENT_OK)
            run.session_status = finished;
        if (status == AGENT_OK)
            status = finished;
    }
    if (status == AGENT_OK)
        status = poll(&run, run.deadline);
    if (run.summary.final_valid)
        deliver(response, final);
    if (publish_cancel(&run, false) && status == AGENT_OK)
        status = AGENT_ERROR_CANCELLED;
    now = agent_runtime_now_ms(&agent->config.runtime);
    run.summary.elapsed_ms = now >= run.started ? now - run.started : 0u;
    run.summary.scratch_peak_bytes = agent->scratch.peak;
    if (status == AGENT_OK)
        increment(&agent->stats.completed_runs);
    else if (status == AGENT_ERROR_CANCELLED)
        increment(&agent->stats.cancelled_runs);
    else
    {
        increment(&agent->stats.failed_runs);
        if (status == AGENT_ERROR_TIMEOUT)
            increment(&agent->stats.timeout_runs);
    }
    agent->stats.last_run_elapsed_ms = run.summary.elapsed_ms;
    agent->stats.last_run_scratch_peak_bytes = run.summary.scratch_peak_bytes;
    if (agent->stats.scratch_peak_bytes < run.summary.scratch_peak_bytes)
        agent->stats.scratch_peak_bytes = run.summary.scratch_peak_bytes;
    response->summary = run.summary;
    response->stats = agent->stats;
    response->status = status;
    response->session_status = run.session_status;
    emit(&run, AGENT_EVENT_TURN_END, status, NULL);
    agent->scratch.used = 0u;
    agent->state = AGENT_CORE_READY;
    return status;
}
