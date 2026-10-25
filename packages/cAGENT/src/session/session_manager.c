/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Bounded current-turn transaction and model-safe history projection. */

#include "session/session_internal.h"

#include <string.h>

typedef struct {
    agent_string_view_t id;
    bool answered;
} pending_call_t;

typedef struct {
    pending_call_t pending[AGENT_MAX_MODEL_TOOL_CALLS ? AGENT_MAX_MODEL_TOOL_CALLS : 1u];
    size_t pending_count;
    bool started;
    bool complete;
} turn_state_t;

struct agent_session_turn {
    agent_arena_t* arena;
    const agent_session_storage_t* storage;
    void* storage_transaction;
    agent_string_view_t session_id;
    agent_message_view_t* current;
    size_t count;
    turn_state_t state;
    bool closed;
};

static agent_error_t check_view(agent_string_view_t view, size_t maximum)
{
    if (view.size > maximum) return AGENT_ERROR_LIMIT;
    return view.size == 0u || view.data ? AGENT_OK : AGENT_ERROR_INVALID;
}

static bool pending_answered(const turn_state_t* state)
{
    size_t i;

    for (i = 0u; i < state->pending_count; ++i) {
        if (!state->pending[i].answered) return false;
    }
    return true;
}

static bool same_view(agent_string_view_t left, agent_string_view_t right)
{
    return left.size == right.size &&
           (left.size == 0u || memcmp(left.data, right.data, left.size) == 0);
}

static bool id_used(const agent_message_view_t* messages, size_t count,
                    agent_string_view_t id)
{
    size_t i;
    size_t j;

    for (i = 0u; i < count; ++i) {
        for (j = 0u; j < messages[i].tool_call_count; ++j) {
            if (same_view(messages[i].tool_calls[j].id, id)) return true;
        }
    }
    return false;
}

static agent_error_t accept_message(turn_state_t* state,
                                    const agent_message_view_t* message)
{
    size_t i;
    size_t j;
    agent_error_t status;

    if (!message || state->complete) return AGENT_ERROR_INVALID;
    if (message->role == AGENT_MESSAGE_ROLE_USER) {
        if (state->started || message->tool_call_count || message->tool_call_id.size ||
            message->content.size == 0u)
            return AGENT_ERROR_INVALID;
        status = check_view(message->content, AGENT_MAX_INPUT_BYTES);
        if (status != AGENT_OK) return status;
        state->started = true;
        return AGENT_OK;
    }
    if (!state->started) return AGENT_ERROR_INVALID;
    if (message->role == AGENT_MESSAGE_ROLE_ASSISTANT) {
        if (message->tool_call_id.size || !pending_answered(state))
            return AGENT_ERROR_INVALID;
        status = check_view(message->content, AGENT_MAX_MODEL_OUTPUT_BYTES);
        if (status != AGENT_OK) return status;
        if (message->tool_call_count == 0u) {
            state->complete = true;
            return AGENT_OK;
        }
        if (!message->tool_calls || message->tool_call_count > AGENT_MAX_MODEL_TOOL_CALLS)
            return AGENT_ERROR_LIMIT;
        state->pending_count = message->tool_call_count;
        for (i = 0u; i < message->tool_call_count; ++i) {
            const agent_tool_call_view_t* call = &message->tool_calls[i];
            if (!call->id.data || call->id.size == 0u ||
                !call->name.data || call->name.size == 0u ||
                !call->arguments_json.data || call->arguments_json.size == 0u)
                return AGENT_ERROR_INVALID;
            status = check_view(call->id, AGENT_MAX_IDENTIFIER_BYTES);
            if (status != AGENT_OK) return status;
            status = check_view(call->name, AGENT_MAX_NAME_BYTES);
            if (status != AGENT_OK) return status;
            status = check_view(call->arguments_json, AGENT_MAX_ARGUMENTS_BYTES);
            if (status != AGENT_OK) return status;
            for (j = 0u; j < i; ++j) {
                if (same_view(state->pending[j].id, call->id)) return AGENT_ERROR_EXISTS;
            }
            state->pending[i].id = call->id;
            state->pending[i].answered = false;
        }
        return AGENT_OK;
    }
    if (message->role != AGENT_MESSAGE_ROLE_TOOL || message->tool_call_count ||
        !message->tool_call_id.data || message->tool_call_id.size == 0u)
        return AGENT_ERROR_INVALID;
    status = check_view(message->tool_call_id, AGENT_MAX_IDENTIFIER_BYTES);
    if (status != AGENT_OK) return status;
    status = check_view(message->content, AGENT_MAX_TOOL_OUTPUT_BYTES);
    if (status != AGENT_OK) return status;
    for (i = 0u; i < state->pending_count; ++i) {
        if (same_view(state->pending[i].id, message->tool_call_id)) {
            if (state->pending[i].answered) return AGENT_ERROR_EXISTS;
            state->pending[i].answered = true;
            return AGENT_OK;
        }
    }
    return AGENT_ERROR_INVALID;
}

static agent_error_t copy_view(agent_arena_t* arena, agent_string_view_t source,
                               agent_string_view_t* target)
{
    void* data;
    agent_error_t status;

    if (source.size == 0u) {
        *target = agent_string_view(NULL, 0u);
        return AGENT_OK;
    }
    if (!source.data) return AGENT_ERROR_INVALID;
    status = agent_arena_take(arena, source.size, 1u, &data);
    if (status != AGENT_OK) return status;
    memcpy(data, source.data, source.size);
    *target = agent_string_view((const char*)data, source.size);
    return AGENT_OK;
}

static agent_error_t copy_message(agent_arena_t* arena,
                                  const agent_message_view_t* source,
                                  agent_message_view_t* target)
{
    agent_tool_call_view_t* calls;
    void* memory;
    size_t bytes;
    size_t i;
    agent_error_t status;

    memset(target, 0, sizeof(*target));
    target->role = source->role;
    status = copy_view(arena, source->content, &target->content);
    if (status != AGENT_OK) return status;
    status = copy_view(arena, source->tool_call_id, &target->tool_call_id);
    if (status != AGENT_OK) return status;
    if (source->tool_call_count == 0u) return AGENT_OK;
    status = agent_size_multiply(source->tool_call_count, sizeof(*calls), &bytes);
    if (status != AGENT_OK) return status;
    status = agent_arena_take(arena, bytes, AGENT_ALIGNOF(agent_tool_call_view_t), &memory);
    if (status != AGENT_OK) return status;
    calls = memory;
    memset(calls, 0, bytes);
    for (i = 0u; i < source->tool_call_count; ++i) {
        status = copy_view(arena, source->tool_calls[i].id, &calls[i].id);
        if (status != AGENT_OK) return status;
        status = copy_view(arena, source->tool_calls[i].name, &calls[i].name);
        if (status != AGENT_OK) return status;
        status = copy_view(arena, source->tool_calls[i].arguments_json,
                           &calls[i].arguments_json);
        if (status != AGENT_OK) return status;
    }
    target->tool_calls = calls;
    target->tool_call_count = source->tool_call_count;
    return AGENT_OK;
}

agent_error_t agent_session_turn_open(agent_session_turn_t** transaction,
                                      agent_arena_t* arena,
                                      const agent_session_storage_t* storage,
                                      agent_string_view_t session_id,
                                      agent_string_view_t input)
{
    static const char default_id[] = "default";
    agent_session_turn_t* turn;
    agent_message_view_t user;
    size_t mark;
    size_t bytes;
    void* memory;
    agent_error_t status;

    if (!transaction) return AGENT_ERROR_INVALID;
    *transaction = NULL;
    if (!arena || !input.data || input.size == 0u) return AGENT_ERROR_INVALID;
    status = check_view(input, AGENT_MAX_INPUT_BYTES);
    if (status != AGENT_OK) return status;
    if (session_id.size == 0u) session_id = agent_string_view(default_id, sizeof(default_id) - 1u);
    if (!session_id.data || session_id.size > AGENT_MAX_IDENTIFIER_BYTES)
        return AGENT_ERROR_INVALID;
    mark = arena->used;
    status = agent_arena_take(arena, sizeof(*turn), AGENT_ALIGNOF(agent_session_turn_t), &memory);
    if (status != AGENT_OK) return status;
    turn = memory;
    memset(turn, 0, sizeof(*turn));
    turn->arena = arena;
    turn->storage = storage;
    status = copy_view(arena, session_id, &turn->session_id);
    if (status != AGENT_OK) goto fail;
    status = agent_size_multiply(AGENT_MAX_PROJECTED_MESSAGES,
                                 sizeof(agent_message_view_t), &bytes);
    if (status != AGENT_OK) goto fail;
    status = agent_arena_take(arena, bytes, AGENT_ALIGNOF(agent_message_view_t), &memory);
    if (status != AGENT_OK) goto fail;
    turn->current = memory;
    if (storage) {
        status = storage->ops.begin(storage->context, turn->session_id,
                                    &turn->storage_transaction);
        if (status != AGENT_OK) goto fail;
    }
    memset(&user, 0, sizeof(user));
    user.role = AGENT_MESSAGE_ROLE_USER;
    user.content = input;
    status = agent_session_append(turn, &user);
    if (status != AGENT_OK) {
        if (storage) storage->ops.finish(storage->context, turn->storage_transaction,
                                         AGENT_SESSION_TURN_ABORTED);
        goto fail;
    }
    *transaction = turn;
    return AGENT_OK;

fail:
    agent_arena_rewind(arena, mark);
    return status;
}

static bool arena_owns(const agent_arena_t* arena, const void* data, size_t bytes)
{
    uintptr_t base = (uintptr_t)arena->base, pointer = (uintptr_t)data;
    return !bytes || (pointer >= base && pointer - base <= arena->used &&
                      bytes <= arena->used - (size_t)(pointer - base));
}

static bool message_owned(const agent_arena_t* arena, const agent_message_view_t* message)
{
    size_t bytes, i;
    if (!arena_owns(arena, message->content.data, message->content.size) ||
        !arena_owns(arena, message->tool_call_id.data, message->tool_call_id.size) ||
        agent_size_multiply(message->tool_call_count, sizeof(agent_tool_call_view_t), &bytes) != AGENT_OK ||
        !arena_owns(arena, message->tool_calls, bytes)) return false;
    for (i = 0u; i < message->tool_call_count; ++i) {
        const agent_tool_call_view_t* call = &message->tool_calls[i];
        if (!arena_owns(arena, call->id.data, call->id.size) ||
            !arena_owns(arena, call->name.data, call->name.size) ||
            !arena_owns(arena, call->arguments_json.data, call->arguments_json.size)) return false;
    }
    return true;
}

static agent_error_t append_message(agent_session_turn_t* turn,
                                    const agent_message_view_t* message, bool owned)
{
    turn_state_t next;
    agent_message_view_t copied;
    size_t mark;
    agent_error_t status;

    if (!turn || turn->closed || !message) return AGENT_ERROR_INVALID;
    if (turn->count >= AGENT_MAX_PROJECTED_MESSAGES) return AGENT_ERROR_LIMIT;
    next = turn->state;
    status = accept_message(&next, message);
    if (status != AGENT_OK) return status;
    if (owned && !message_owned(turn->arena, message)) return AGENT_ERROR_INVALID;
    if (message->tool_call_count) {
        size_t i;
        for (i = 0u; i < message->tool_call_count; ++i) {
            if (id_used(turn->current, turn->count, message->tool_calls[i].id))
                return AGENT_ERROR_EXISTS;
        }
    }
    mark = turn->arena->used;
    if (owned) { copied = *message; status = AGENT_OK; }
    else status = copy_message(turn->arena, message, &copied);
    if (status != AGENT_OK) goto fail;
    if (turn->storage) {
        status = turn->storage->ops.append(turn->storage->context,
                                           turn->storage_transaction, &copied);
        if (status != AGENT_OK) goto fail;
    }
    /* The validator's pending IDs must reference the owned copies. */
    if (copied.tool_call_count) {
        size_t i;
        for (i = 0u; i < copied.tool_call_count; ++i)
            next.pending[i].id = copied.tool_calls[i].id;
    }
    turn->current[turn->count++] = copied;
    turn->state = next;
    return AGENT_OK;

fail:
    agent_arena_rewind(turn->arena, mark);
    return status;
}

agent_error_t agent_session_append(agent_session_turn_t* turn, const agent_message_view_t* message)
{
    return append_message(turn, message, false);
}

agent_error_t agent_session_append_owned(agent_session_turn_t* turn, const agent_message_view_t* message)
{
    return append_message(turn, message, true);
}

agent_error_t agent_session_turn_finish(agent_session_turn_t* turn,
                                        agent_session_turn_outcome_t outcome)
{
    agent_error_t status = AGENT_OK;

    if (!turn || turn->closed ||
        (outcome != AGENT_SESSION_TURN_COMPLETE && outcome != AGENT_SESSION_TURN_ABORTED))
        return AGENT_ERROR_INVALID;
    if (outcome == AGENT_SESSION_TURN_COMPLETE && !turn->state.complete)
        return AGENT_ERROR_STATE;
    turn->closed = true;
    if (turn->storage)
        status = turn->storage->ops.finish(turn->storage->context,
                                           turn->storage_transaction, outcome);
    return status;
}

typedef struct {
    agent_arena_t* arena;
    agent_message_view_t* messages;
    size_t count;
    size_t capacity;
    size_t candidates;
    size_t seen;
    agent_error_t (*poll)(void*);
    void* poll_context;
} projection_t;

static agent_error_t visit_group(void* context, const agent_session_group_view_t* group)
{
    projection_t* projection = context;
    turn_state_t state = {0};
    size_t mark;
    size_t i;
    agent_error_t status;

    if (projection->poll) {
        status = projection->poll(projection->poll_context);
        if (status != AGENT_OK) return status;
    }
    if (++projection->seen > projection->candidates || !group || !group->messages ||
        group->message_count == 0u) return AGENT_ERROR_PARSE;
    for (i = 0u; i < group->message_count; ++i) {
        status = accept_message(&state, &group->messages[i]);
        if (status != AGENT_OK) return AGENT_ERROR_PARSE;
        if (group->messages[i].tool_call_count) {
            size_t j;
            for (j = 0u; j < group->messages[i].tool_call_count; ++j) {
                if (id_used(group->messages, i,
                            group->messages[i].tool_calls[j].id))
                    return AGENT_ERROR_PARSE;
            }
        }
    }
    if (!state.complete) return AGENT_ERROR_PARSE;
    if (group->message_count > projection->capacity - projection->count) return AGENT_OK;
    mark = projection->arena->used;
    for (i = 0u; i < group->message_count; ++i) {
        status = copy_message(projection->arena,
                              &group->messages[group->message_count - 1u - i],
                              &projection->messages[projection->count + i]);
        if (status != AGENT_OK) {
            agent_arena_rewind(projection->arena, mark);
            return status == AGENT_ERROR_CAPACITY ? AGENT_OK : status;
        }
    }
    projection->count += group->message_count;
    return AGENT_OK;
}

size_t agent_session_current_count(const agent_session_turn_t* turn)
{
    return turn && !turn->closed ? turn->count : 0u;
}

agent_error_t agent_session_projection_identity(const agent_session_turn_t* turn,
    const agent_arena_t* owner, const agent_request_t* request, agent_string_view_t* session_id)
{
    agent_string_view_t expected;
    if (!turn || turn->closed || turn->arena != owner || !request || !session_id || !turn->count)
        return AGENT_ERROR_INVALID;
    expected = request->session_id.size ? request->session_id : agent_string_view("default", 7u);
    if (!same_view(expected, turn->session_id) || !same_view(request->input, turn->current[0].content))
        return AGENT_ERROR_INVALID;
    *session_id = turn->session_id;
    return AGENT_OK;
}

agent_error_t agent_session_project_into(const agent_session_turn_t* turn,
    agent_arena_t* arena, uint32_t max_history_turns, agent_message_view_t* messages,
    size_t capacity, size_t* count, agent_error_t (*poll)(void*), void* poll_context)
{
    projection_t projection;
    agent_message_view_t swap;
    size_t mark;
    size_t i;
    agent_error_t status;

    if (!turn || turn->closed || !arena || !messages || !count) return AGENT_ERROR_INVALID;
    *count = 0u;
    if (capacity > AGENT_MAX_PROJECTED_MESSAGES || turn->count > capacity) return AGENT_ERROR_LIMIT;
    if (max_history_turns && !turn->storage) return AGENT_ERROR_NOT_SUPPORTED;
    mark = arena->used;
    memset(&projection, 0, sizeof(projection));
    projection.arena = arena;
    projection.messages = messages;
    projection.capacity = capacity - turn->count;
    projection.poll = poll;
    projection.poll_context = poll_context;
    projection.candidates = max_history_turns < projection.capacity ?
                            max_history_turns : projection.capacity;
    if (projection.candidates) {
        status = turn->storage->ops.recent(turn->storage->context, turn->session_id,
                                            projection.candidates, visit_group, &projection);
        if (status != AGENT_OK) {
            agent_arena_rewind(arena, mark);
            return status;
        }
    }
    for (i = 0u; i < projection.count / 2u; ++i) {
        swap = projection.messages[i];
        projection.messages[i] = projection.messages[projection.count - 1u - i];
        projection.messages[projection.count - 1u - i] = swap;
    }
    memcpy(&projection.messages[projection.count], turn->current,
           turn->count * sizeof(agent_message_view_t));
    *count = projection.count + turn->count;
    return AGENT_OK;
}

agent_error_t agent_session_project(const agent_session_turn_t* turn,
                                    agent_arena_t* arena, uint32_t max_history_turns,
                                    const agent_message_view_t** messages, size_t* count)
{
    size_t mark, bytes;
    void* memory;
    agent_error_t status;
    if (!arena || !messages || !count) return AGENT_ERROR_INVALID;
    *messages = NULL; *count = 0u;
    mark = arena->used;
    status = agent_size_multiply(AGENT_MAX_PROJECTED_MESSAGES, sizeof(agent_message_view_t), &bytes);
    if (status == AGENT_OK)
        status = agent_arena_take(arena, bytes, AGENT_ALIGNOF(agent_message_view_t), &memory);
    if (status != AGENT_OK) return status;
    status = agent_session_project_into(turn, arena, max_history_turns, memory,
                                        AGENT_MAX_PROJECTED_MESSAGES, count, NULL, NULL);
    if (status != AGENT_OK) agent_arena_rewind(arena, mark);
    else *messages = memory;
    return status;
}
