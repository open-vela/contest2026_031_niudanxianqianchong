/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 tanglinjie */
/* Turn snapshots and disposable model-call projections in caller-owned scratch. */
#include "core/core_internal.h"
#include "core/text_internal.h"
#include "memory/memory_internal.h"
#include "runtime/runtime_internal.h"
#include <string.h>

#define TEXT_BUDGET ((size_t)AGENT_MAX_CONTEXT_BYTES + 2u)
typedef char context_text_budget_must_not_wrap[(AGENT_MAX_CONTEXT_BYTES <= SIZE_MAX - 2u) ? 1 : -1];

static size_t cost(size_t bytes) { return bytes ? bytes + 2u : 0u; }

static bool terminal(agent_error_t status)
{
    return status == AGENT_ERROR_CANCELLED || status == AGENT_ERROR_TIMEOUT;
}

static void omit(agent_context_report_t* report, agent_string_view_t name, agent_error_t status)
{
    if (report->count < AGENT_CONTEXT_PARTS) {
        report->entries[report->count].name = name;
        report->entries[report->count++].status = status;
    }
}

static agent_error_t reserve(size_t* total, size_t bytes)
{
    size_t amount;
    if (agent_size_add(bytes, bytes ? 2u : 0u, &amount) != AGENT_OK ||
        amount > TEXT_BUDGET - *total) return AGENT_ERROR_CONTEXT_OVERFLOW;
    *total += amount;
    return AGENT_OK;
}

static size_t dynamic_reserve(const agent_t* agent)
{
    size_t i, bytes = 0u;
    if (agent->contexts) for (i = 0u; i < agent->contexts->count; ++i) {
        const agent_context_entry_t* e = &agent->contexts->entries[i];
        if (!e->memory && e->provider.required) bytes += cost(e->provider.max_bytes);
    }
    return bytes;
}

static bool snapshot_fits(const agent_t* agent, size_t bytes)
{
    size_t reserve_bytes = dynamic_reserve(agent), amount;
    if (agent_size_add(reserve_bytes, AGENT_MAX_CONTEXT_BYTES, &reserve_bytes) != AGENT_OK ||
        agent_size_add(reserve_bytes, 1u, &reserve_bytes) != AGENT_OK ||
        agent_size_multiply(AGENT_MAX_PROJECTED_MESSAGES, sizeof(agent_message_view_t), &amount) != AGENT_OK ||
        agent_size_add(reserve_bytes, amount, &reserve_bytes) != AGENT_OK ||
        agent_size_multiply(agent->tools ? agent->tools->count : 0u, sizeof(agent_tool_view_t), &amount) != AGENT_OK ||
        agent_size_add(reserve_bytes, amount, &reserve_bytes) != AGENT_OK ||
        agent_size_add(reserve_bytes, AGENT_ALIGNOF(agent_message_view_t) + AGENT_ALIGNOF(agent_tool_view_t), &reserve_bytes) != AGENT_OK ||
        agent_size_add(reserve_bytes, bytes, &reserve_bytes) != AGENT_OK ||
        agent_size_add(reserve_bytes, 1u, &reserve_bytes) != AGENT_OK) return false;
    return reserve_bytes <= agent->scratch.capacity - agent->scratch.used;
}

agent_error_t agent_context_prepare(agent_t* agent, const agent_request_t* request,
                                    const agent_cancel_token_t* cancel, uint64_t deadline_ms,
                                    agent_context_turn_t** result)
{
    agent_context_turn_t* turn;
    size_t mark, i, pass, required = 0u, memory_base;
    void* memory;
    agent_error_t status;
    if (!agent || !request || !result ||
        agent_bytes_overlap(result, sizeof(*result), agent->workspace, agent->workspace_size) ||
        agent_bytes_overlap(result, sizeof(*result), request, sizeof(*request)))
        return AGENT_ERROR_INVALID;
    if (agent->in_callback) return AGENT_ERROR_BUSY;
    if (agent->state != AGENT_CORE_ACTIVE) return AGENT_ERROR_STATE;
    *result = NULL;
    if (!request->input.size || request->input.size > AGENT_MAX_INPUT_BYTES ||
        request->session_id.size > AGENT_MAX_IDENTIFIER_BYTES ||
        request->trace_id.size > AGENT_MAX_IDENTIFIER_BYTES) return AGENT_ERROR_LIMIT;
    status = agent_text_validate(request->input);
    if (status == AGENT_OK) status = agent_text_validate(request->session_id);
    if (status == AGENT_OK) status = agent_text_validate(request->trace_id);
    if (status == AGENT_OK) status = agent_text_validate(agent->config.system_prompt);
    if (status != AGENT_OK) return status;
    if (!(request->limits ? request->limits->max_steps : agent->config.limits.max_steps))
        return AGENT_ERROR_INVALID;
    status = reserve(&required, agent->config.system_prompt.size);
    if (agent->skills) for (i = 0u; status == AGENT_OK && i < agent->skills->count; ++i)
        if (agent->skills->entries[i].required)
            status = reserve(&required, agent->skills->entries[i].content.size);
    if (agent->contexts) for (i = 0u; status == AGENT_OK && i < agent->contexts->count; ++i)
        if (agent->contexts->entries[i].provider.required)
            status = reserve(&required, agent->contexts->entries[i].provider.max_bytes);
    if (status != AGENT_OK) return status;
    mark = agent->scratch.used;
    status = agent_arena_take(&agent->scratch, sizeof(*turn), AGENT_ALIGNOF(agent_context_turn_t), &memory);
    if (status != AGENT_OK) return status;
    turn = memory;
    memset(turn, 0, sizeof(*turn));
    turn->owner = agent;
    turn->request = *request;
    turn->limits = request->limits ? *request->limits : agent->config.limits;
    turn->request.limits = &turn->limits;
    turn->cancel = cancel;
    turn->deadline_ms = deadline_ms;
    turn->parts[0].text = agent->config.system_prompt;
    turn->parts[0].required = true;
    turn->text_cost = cost(agent->config.system_prompt.size);
    memory_base = 1u + (agent->skills ? agent->skills->count : 0u);
    turn->part_count = memory_base + (agent->contexts ? agent->contexts->count : 0u);
    status = agent_context_poll(turn);
    if (status != AGENT_OK) goto fail;
    /* Required snapshots precede optional allocation; output order remains registry order. */
    for (pass = 0u; pass < 2u; ++pass) {
        if (agent->skills) for (i = 0u; i < agent->skills->count; ++i) {
            const agent_skill_t* s = &agent->skills->entries[i];
            if (s->required != (pass == 0u)) continue;
            if (cost(s->content.size) > TEXT_BUDGET - turn->text_cost - dynamic_reserve(agent)) {
                omit(&turn->report, s->name, AGENT_ERROR_CONTEXT_OVERFLOW);
                continue;
            }
            turn->parts[1u + i].text = s->content;
            turn->parts[1u + i].required = s->required;
            turn->parts[1u + i].name = s->name;
            turn->text_cost += cost(s->content.size);
        }
        if (agent->contexts) for (i = 0u; i < agent->contexts->count; ++i) {
            const agent_context_entry_t* e = &agent->contexts->entries[i];
            agent_string_view_t text;
            size_t checkpoint;
            agent_error_t after;
            if (!e->memory || e->provider.required != (pass == 0u)) continue;
            status = agent_context_poll(turn);
            if (status != AGENT_OK) goto fail;
            if (cost(e->provider.max_bytes) > TEXT_BUDGET - turn->text_cost - dynamic_reserve(agent))
                status = AGENT_ERROR_CONTEXT_OVERFLOW;
            else if (!e->provider.required && !snapshot_fits(agent, e->provider.max_bytes))
                status = AGENT_ERROR_CAPACITY;
            else {
                checkpoint = agent->scratch.used;
                status = agent_memory_project(agent, &e->key, &agent->scratch,
                                                e->provider.max_bytes, &text);
                if (status == AGENT_OK) status = agent_text_validate(text);
                if (status != AGENT_OK) agent_arena_rewind(&agent->scratch, checkpoint);
            }
            after = agent_context_poll(turn);
            if (after != AGENT_OK) { status = after; goto fail; }
            if (status != AGENT_OK) {
                if (e->provider.required || terminal(status)) goto fail;
                omit(&turn->report, e->provider.name, status);
                continue;
            }
            turn->parts[memory_base + i].text = text;
            turn->parts[memory_base + i].placement = e->provider.placement;
            turn->parts[memory_base + i].required = e->provider.required;
            turn->parts[memory_base + i].name = e->provider.name;
            turn->text_cost += cost(text.size);
        }
    }
    turn->end = agent->scratch.used;
    *result = turn;
    return AGENT_OK;
fail:
    agent_arena_rewind(&agent->scratch, mark);
    return status;
}

static agent_error_t poll_session(void* context)
{
    return agent_context_poll(context);
}

static agent_error_t append_system(char* buffer, size_t* used, agent_string_view_t text)
{
    size_t separator = *used && text.size ? 2u : 0u;
    if (separator > AGENT_MAX_CONTEXT_BYTES - *used ||
        text.size > AGENT_MAX_CONTEXT_BYTES - *used - separator) return AGENT_ERROR_CONTEXT_OVERFLOW;
    if (separator) { memcpy(buffer + *used, "\n\n", 2u); *used += 2u; }
    if (text.size) { memcpy(buffer + *used, text.data, text.size); *used += text.size; }
    buffer[*used] = '\0';
    return AGENT_OK;
}

static size_t instruction_cost(const agent_context_part_t* parts, size_t count)
{
    size_t i, bytes = 0u;
    for (i = 0u; i < count; ++i)
        if (parts[i].placement == AGENT_CONTEXT_INSTRUCTIONS) bytes += cost(parts[i].text.size);
    return bytes;
}

agent_error_t agent_context_project(agent_context_turn_t* turn, const agent_session_turn_t* session,
                                    uint32_t remaining_tool_calls, size_t reserve_bytes,
                                    agent_context_projection_t* result)
{
    agent_t* agent;
    agent_arena_t arena;
    agent_context_projection_t projection = {0};
    agent_message_view_t* messages;
    agent_tool_view_t* tools = NULL;
    agent_context_part_t dynamic[AGENT_MAX_CONTEXTS ? AGENT_MAX_CONTEXTS : 1u] = {0};
    size_t i, pass, bytes, text_used = 0u, refs = 0u, history_count, current_count, used_cost;
    size_t required_refs = 0u;
    char* system;
    void* memory;
    agent_error_t status;
    if (!turn || !session || !result || !(agent = turn->owner)) return AGENT_ERROR_INVALID;
    if (agent_bytes_overlap(result, sizeof(*result), agent->workspace, agent->workspace_size))
        return AGENT_ERROR_INVALID;
    if (agent->in_callback) return AGENT_ERROR_BUSY;
    if (agent->state != AGENT_CORE_ACTIVE || agent->scratch.used < turn->end) return AGENT_ERROR_STATE;
    memset(result, 0, sizeof(*result));
    status = agent_session_projection_identity(session, &agent->scratch, &turn->request,
                                                &turn->request.session_id);
    if (status != AGENT_OK) return status;
    status = agent_context_poll(turn);
    if (status != AGENT_OK) return status;
    current_count = agent_session_current_count(session);
    if (!current_count || current_count > AGENT_MAX_PROJECTED_MESSAGES) return AGENT_ERROR_STATE;
    arena = agent->scratch;
    if (reserve_bytes > arena.capacity - arena.used) return AGENT_ERROR_CAPACITY;
    arena.capacity -= reserve_bytes;
    projection.mark = arena.used;
    projection.report = turn->report;
    status = agent_size_multiply(AGENT_MAX_PROJECTED_MESSAGES, sizeof(*messages), &bytes);
    if (status == AGENT_OK) status = agent_arena_take(&arena, bytes, AGENT_ALIGNOF(agent_message_view_t), &memory);
    if (status != AGENT_OK) goto fail;
    messages = memory;
    memset(messages, 0, bytes);
    if (remaining_tool_calls && turn->limits.max_tool_calls && agent->tools) {
        status = agent_size_multiply(agent->tools->count, sizeof(*tools), &bytes);
        if (status == AGENT_OK && bytes)
            status = agent_arena_take(&arena, bytes, AGENT_ALIGNOF(agent_tool_view_t), &memory);
        if (status != AGENT_OK) goto fail;
        tools = bytes ? memory : NULL;
        status = agent_tool_registry_project(agent->tools, tools, agent->tools->count,
                                              &projection.request.tool_count);
        if (status != AGENT_OK) goto fail;
    }
    for (i = 0u; i < turn->part_count; ++i)
        if (turn->parts[i].text.size && turn->parts[i].required &&
            turn->parts[i].placement == AGENT_CONTEXT_REFERENCE) ++required_refs;
    if (agent->contexts) for (i = 0u; i < agent->contexts->count; ++i) {
        const agent_context_entry_t* e = &agent->contexts->entries[i];
        if (!e->memory && e->provider.required && e->provider.placement == AGENT_CONTEXT_REFERENCE)
            ++required_refs;
    }
    if (required_refs > AGENT_MAX_PROJECTED_MESSAGES - current_count) { status = AGENT_ERROR_LIMIT; goto fail; }
    for (i = 0u; i < turn->part_count; ++i) {
        agent_context_part_t p = turn->parts[i];
        if (!p.text.size) continue;
        if (p.placement == AGENT_CONTEXT_REFERENCE) {
            if (p.required) --required_refs;
            else if (refs + required_refs >= AGENT_MAX_PROJECTED_MESSAGES - current_count) {
                omit(&projection.report, p.name, AGENT_ERROR_LIMIT);
                continue;
            }
            messages[refs].role = AGENT_MESSAGE_ROLE_USER;
            messages[refs++].content = p.text;
        }
    }
    used_cost = turn->text_cost;
    /* Required callbacks run first; their output is still emitted in stable priority order. */
    for (pass = 0u; pass < 2u; ++pass) {
        if (!agent->contexts) break;
        for (i = 0u; i < agent->contexts->count; ++i) {
            const agent_context_entry_t* e = &agent->contexts->entries[i];
            agent_string_view_t text;
            size_t checkpoint = arena.used;
            if (e->memory || e->provider.required != (pass == 0u)) continue;
            status = agent_context_poll(turn);
            if (status != AGENT_OK) goto fail;
            if (cost(e->provider.max_bytes) > TEXT_BUDGET - used_cost)
                status = AGENT_ERROR_CONTEXT_OVERFLOW;
            else {
                size_t render = instruction_cost(turn->parts, turn->part_count) +
                                instruction_cost(dynamic, agent->contexts->count);
                if (e->provider.placement == AGENT_CONTEXT_INSTRUCTIONS) render += cost(e->provider.max_bytes);
                if (!e->provider.required && (render >= arena.capacity - arena.used ||
                    e->provider.max_bytes > arena.capacity - arena.used - render - 1u))
                    status = AGENT_ERROR_CAPACITY;
                else status = agent_arena_take(&arena, e->provider.max_bytes, 1u, &memory);
                if (status == AGENT_OK)
                    status = agent_context_collect(turn, e, memory, e->provider.max_bytes, &text);
                if (status == AGENT_OK && text.size && e->provider.placement == AGENT_CONTEXT_REFERENCE &&
                    refs >= AGENT_MAX_PROJECTED_MESSAGES - current_count) status = AGENT_ERROR_LIMIT;
            }
            if (status != AGENT_OK) {
                agent_arena_rewind(&arena, checkpoint);
                if (e->provider.required || terminal(status)) goto fail;
                omit(&projection.report, e->provider.name, status);
                continue;
            }
            agent_arena_rewind(&arena, checkpoint + text.size);
            dynamic[i].text = text;
            dynamic[i].placement = e->provider.placement;
            used_cost += cost(text.size);
            if (text.size && e->provider.placement == AGENT_CONTEXT_REFERENCE) ++refs;
        }
    }
    bytes = instruction_cost(turn->parts, turn->part_count) +
            instruction_cost(dynamic, agent->contexts ? agent->contexts->count : 0u);
    bytes = bytes ? bytes - 1u : 1u;
    status = agent_arena_take(&arena, bytes, 1u, &memory);
    if (status != AGENT_OK) goto fail;
    system = memory; system[0] = '\0';
    for (i = 0u; i < turn->part_count; ++i) {
        if (turn->parts[i].placement != AGENT_CONTEXT_INSTRUCTIONS) continue;
        status = append_system(system, &text_used, turn->parts[i].text);
        if (status != AGENT_OK) goto fail;
    }
    /* Reconstruct reference count before appending dynamic entries in presentation order. */
    refs = 0u;
    while (refs < AGENT_MAX_PROJECTED_MESSAGES && messages[refs].role) ++refs;
    if (agent->contexts) for (i = 0u; i < agent->contexts->count; ++i) {
        agent_context_part_t p = dynamic[i];
        if (!p.text.size) continue;
        if (p.placement == AGENT_CONTEXT_INSTRUCTIONS) {
            status = append_system(system, &text_used, p.text);
            if (status != AGENT_OK) goto fail;
        } else {
            messages[refs].role = AGENT_MESSAGE_ROLE_USER;
            messages[refs++].content = p.text;
        }
    }
    agent->in_callback = true;
    status = agent_session_project_into(session, &arena, turn->limits.max_history_turns,
                                         messages + refs, AGENT_MAX_PROJECTED_MESSAGES - refs,
                                         &history_count, poll_session, turn);
    agent->in_callback = false;
    if (status != AGENT_OK) goto fail;
    for (i = refs; i < refs + history_count; ++i) {
        status = agent_text_validate(messages[i].content);
        if (status != AGENT_OK) goto fail;
    }
    status = agent_context_poll(turn);
    if (status != AGENT_OK) goto fail;
    projection.request.system_prompt = agent_string_view(system, text_used);
    projection.request.messages = messages;
    projection.request.message_count = refs + history_count;
    projection.request.tools = tools;
    projection.request.session_id = turn->request.session_id;
    projection.request.trace_id = turn->request.trace_id;
    projection.request.cancel = turn->cancel;
    projection.request.deadline_ms = turn->deadline_ms;
    projection.request.timeout_ms = turn->limits.per_model_timeout_ms;
    if (turn->deadline_ms) {
        uint64_t now = agent_runtime_now_ms(&agent->config.runtime), remaining;
        if (now >= turn->deadline_ms) { status = AGENT_ERROR_TIMEOUT; goto fail; }
        remaining = turn->deadline_ms - now;
        if (!projection.request.timeout_ms || remaining < projection.request.timeout_ms)
            projection.request.timeout_ms = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
    }
    projection.request.max_output_tokens = turn->limits.max_output_tokens;
    projection.owner = agent;
    projection.end = arena.used;
    agent->scratch.used = arena.used;
    if (arena.peak > agent->scratch.peak) agent->scratch.peak = arena.peak;
    *result = projection;
    return AGENT_OK;
fail:
    if (arena.peak > agent->scratch.peak) agent->scratch.peak = arena.peak;
    return status;
}

agent_error_t agent_context_release(agent_context_projection_t* projection)
{
    agent_t* agent;
    if (!projection || !(agent = projection->owner)) return AGENT_ERROR_INVALID;
    if (agent_bytes_overlap(projection, sizeof(*projection), agent->workspace, agent->workspace_size))
        return AGENT_ERROR_INVALID;
    if (agent->in_callback) return AGENT_ERROR_BUSY;
    if (agent->state != AGENT_CORE_ACTIVE || agent->scratch.used != projection->end)
        return AGENT_ERROR_STATE;
    agent_arena_rewind(&agent->scratch, projection->mark);
    memset(projection, 0, sizeof(*projection));
    return AGENT_OK;
}
