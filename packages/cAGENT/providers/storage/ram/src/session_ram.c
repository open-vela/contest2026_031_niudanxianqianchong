/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Compact, bounded and volatile Session Storage. */

#include <agent_session_ram.h>

#include <string.h>

static agent_string_view_t payload_view(const agent_session_ram_t* ram,
                                        size_t offset, size_t size)
{
    return agent_string_view(ram->config.payload + offset, size);
}

static bool same_session(const agent_session_ram_t* ram,
                         const agent_session_ram_turn_t* turn,
                         agent_string_view_t session_id)
{
    return turn->session_size == session_id.size &&
           memcmp(ram->config.payload + turn->session_offset,
                  session_id.data, session_id.size) == 0;
}

static agent_error_t put(agent_session_ram_t* ram, agent_string_view_t value,
                         size_t* offset)
{
    if (value.size && !value.data) return AGENT_ERROR_INVALID;
    if (value.size > ram->config.payload_capacity - ram->payload_used)
        return AGENT_ERROR_CAPACITY;
    *offset = ram->payload_used;
    if (value.size) memcpy(ram->config.payload + ram->payload_used, value.data, value.size);
    ram->payload_used += value.size;
    return AGENT_OK;
}

static agent_error_t ram_begin(void* context, agent_string_view_t session_id,
                               void** transaction)
{
    agent_session_ram_t* ram = context;
    agent_session_ram_turn_t* turn;
    size_t offset;
    agent_error_t status;

    if (!ram || !transaction || !session_id.data || session_id.size == 0u)
        return AGENT_ERROR_INVALID;
    *transaction = NULL;
    if (ram->active) return AGENT_ERROR_BUSY;
    if (ram->turn_count >= ram->config.turn_capacity) return AGENT_ERROR_CAPACITY;
    status = put(ram, session_id, &offset);
    if (status != AGENT_OK) return status;
    turn = &ram->config.turns[ram->turn_count++];
    memset(turn, 0, sizeof(*turn));
    turn->session_offset = offset;
    turn->session_size = session_id.size;
    turn->payload_start = offset;
    turn->payload_end = ram->payload_used;
    turn->first_message = ram->message_count;
    turn->first_call = ram->call_count;
    ram->active = true;
    *transaction = ram;
    return AGENT_OK;
}

static agent_error_t ram_append(void* context, void* transaction,
                                const agent_message_view_t* message)
{
    agent_session_ram_t* ram = context;
    agent_session_ram_turn_t* turn;
    agent_session_ram_message_t entry;
    size_t mark;
    size_t i;
    agent_error_t status;

    if (!ram || transaction != ram || !ram->active || !message ||
        (message->tool_call_count && !message->tool_calls)) return AGENT_ERROR_INVALID;
    if (ram->message_count >= ram->config.message_capacity ||
        message->tool_call_count > ram->config.call_capacity - ram->call_count)
        return AGENT_ERROR_CAPACITY;
    turn = &ram->config.turns[ram->turn_count - 1u];
    mark = ram->payload_used;
    memset(&entry, 0, sizeof(entry));
    entry.role = message->role;
    entry.content_size = message->content.size;
    entry.tool_id_size = message->tool_call_id.size;
    entry.first_call = ram->call_count;
    entry.call_count = message->tool_call_count;
    status = put(ram, message->content, &entry.content_offset);
    if (status != AGENT_OK) goto fail;
    status = put(ram, message->tool_call_id, &entry.tool_id_offset);
    if (status != AGENT_OK) goto fail;
    for (i = 0u; i < message->tool_call_count; ++i) {
        const agent_tool_call_view_t* source = &message->tool_calls[i];
        agent_session_ram_call_t call = {0};
        call.id_size = source->id.size;
        call.name_size = source->name.size;
        call.arguments_size = source->arguments_json.size;
        status = put(ram, source->id, &call.id_offset);
        if (status != AGENT_OK) goto fail;
        status = put(ram, source->name, &call.name_offset);
        if (status != AGENT_OK) goto fail;
        status = put(ram, source->arguments_json, &call.arguments_offset);
        if (status != AGENT_OK) goto fail;
        ram->config.calls[ram->call_count + i] = call;
    }
    ram->config.messages[ram->message_count++] = entry;
    ram->call_count += message->tool_call_count;
    turn->message_count++;
    turn->call_count += message->tool_call_count;
    turn->payload_end = ram->payload_used;
    return AGENT_OK;

fail:
    memset(ram->config.payload + mark, 0, ram->payload_used - mark);
    ram->payload_used = mark;
    return status;
}

static agent_error_t ram_finish(void* context, void* transaction,
                                agent_session_turn_outcome_t outcome)
{
    agent_session_ram_t* ram = context;
    agent_session_ram_turn_t* turn;

    if (!ram || transaction != ram || !ram->active) return AGENT_ERROR_INVALID;
    turn = &ram->config.turns[ram->turn_count - 1u];
    ram->active = false;
    if (outcome != AGENT_SESSION_TURN_COMPLETE && outcome != AGENT_SESSION_TURN_ABORTED) {
        turn->outcome = AGENT_SESSION_TURN_ABORTED;
        return AGENT_ERROR_INVALID;
    }
    if (outcome == AGENT_SESSION_TURN_COMPLETE &&
        (turn->message_count < 2u ||
         ram->config.messages[ram->message_count - 1u].role != AGENT_MESSAGE_ROLE_ASSISTANT ||
         ram->config.messages[ram->message_count - 1u].call_count != 0u)) {
        turn->outcome = AGENT_SESSION_TURN_ABORTED;
        return AGENT_ERROR_STATE;
    }
    turn->outcome = outcome;
    return AGENT_OK;
}

static agent_error_t ram_recent(void* context, agent_string_view_t session_id,
                                size_t max_candidates, agent_session_visit_fn visit,
                                void* visit_context)
{
    agent_session_ram_t* ram = context;
    size_t index;
    size_t seen = 0u;

    if (!ram || !session_id.data || !visit) return AGENT_ERROR_INVALID;
    for (index = ram->turn_count; index > 0u && seen < max_candidates; --index) {
        const agent_session_ram_turn_t* turn = &ram->config.turns[index - 1u];
        agent_session_group_view_t group;
        size_t i;
        size_t call_index = 0u;
        agent_error_t status;

        if (turn->outcome != AGENT_SESSION_TURN_COMPLETE ||
            !same_session(ram, turn, session_id)) continue;
        if (turn->message_count > ram->config.read_message_capacity ||
            turn->call_count > ram->config.read_call_capacity) return AGENT_ERROR_CAPACITY;
        for (i = 0u; i < turn->message_count; ++i) {
            const agent_session_ram_message_t* source =
                &ram->config.messages[turn->first_message + i];
            agent_message_view_t* target = &ram->config.read_messages[i];
            size_t j;

            memset(target, 0, sizeof(*target));
            target->role = source->role;
            target->content = payload_view(ram, source->content_offset, source->content_size);
            target->tool_call_id = payload_view(ram, source->tool_id_offset,
                                                source->tool_id_size);
            target->tool_call_count = source->call_count;
            if (source->call_count) target->tool_calls = &ram->config.read_calls[call_index];
            for (j = 0u; j < source->call_count; ++j) {
                const agent_session_ram_call_t* call =
                    &ram->config.calls[source->first_call + j];
                agent_tool_call_view_t* item = &ram->config.read_calls[call_index++];
                item->id = payload_view(ram, call->id_offset, call->id_size);
                item->name = payload_view(ram, call->name_offset, call->name_size);
                item->arguments_json = payload_view(ram, call->arguments_offset,
                                                    call->arguments_size);
            }
        }
        group.messages = ram->config.read_messages;
        group.message_count = turn->message_count;
        ++seen;
        status = visit(visit_context, &group);
        if (status != AGENT_OK) return status;
    }
    return AGENT_OK;
}

static void adjust_offset(size_t* offset, size_t end, size_t removed)
{
    if (*offset >= end) *offset -= removed;
}

static void remove_turn(agent_session_ram_t* ram, size_t index)
{
    agent_session_ram_turn_t turn = ram->config.turns[index];
    size_t payload_bytes = turn.payload_end - turn.payload_start;
    size_t i;

    memmove(ram->config.payload + turn.payload_start,
            ram->config.payload + turn.payload_end,
            ram->payload_used - turn.payload_end);
    ram->payload_used -= payload_bytes;
    memset(ram->config.payload + ram->payload_used, 0, payload_bytes);
    memmove(&ram->config.messages[turn.first_message],
            &ram->config.messages[turn.first_message + turn.message_count],
            (ram->message_count - turn.first_message - turn.message_count) *
            sizeof(*ram->config.messages));
    ram->message_count -= turn.message_count;
    if (ram->call_count != 0u) {
        memmove(&ram->config.calls[turn.first_call],
                &ram->config.calls[turn.first_call + turn.call_count],
                (ram->call_count - turn.first_call - turn.call_count) *
                sizeof(*ram->config.calls));
    }
    ram->call_count -= turn.call_count;
    memmove(&ram->config.turns[index], &ram->config.turns[index + 1u],
            (ram->turn_count - index - 1u) * sizeof(*ram->config.turns));
    ram->turn_count--;
    for (i = index; i < ram->turn_count; ++i) {
        agent_session_ram_turn_t* later = &ram->config.turns[i];
        later->session_offset -= payload_bytes;
        later->payload_start -= payload_bytes;
        later->payload_end -= payload_bytes;
        later->first_message -= turn.message_count;
        later->first_call -= turn.call_count;
    }
    for (i = turn.first_message; i < ram->message_count; ++i) {
        agent_session_ram_message_t* later = &ram->config.messages[i];
        adjust_offset(&later->content_offset, turn.payload_end, payload_bytes);
        adjust_offset(&later->tool_id_offset, turn.payload_end, payload_bytes);
        later->first_call -= turn.call_count;
    }
    for (i = turn.first_call; i < ram->call_count; ++i) {
        agent_session_ram_call_t* later = &ram->config.calls[i];
        adjust_offset(&later->id_offset, turn.payload_end, payload_bytes);
        adjust_offset(&later->name_offset, turn.payload_end, payload_bytes);
        adjust_offset(&later->arguments_offset, turn.payload_end, payload_bytes);
    }
}

agent_error_t agent_session_ram_trim(agent_session_ram_t* ram, size_t keep)
{
    size_t index;
    if (!ram) return AGENT_ERROR_INVALID;
    if (ram->active) return AGENT_ERROR_BUSY;
    for (index = ram->turn_count; index > 0u; --index)
        if (ram->config.turns[index - 1u].outcome != AGENT_SESSION_TURN_COMPLETE)
            remove_turn(ram, index - 1u);
    while (ram->turn_count > keep) remove_turn(ram, 0u);
    return AGENT_OK;
}

static agent_error_t ram_clear(void* context, agent_string_view_t session_id)
{
    agent_session_ram_t* ram = context;
    size_t index;
    bool found = false;

    if (!ram || !session_id.data || session_id.size == 0u) return AGENT_ERROR_INVALID;
    if (ram->active) return AGENT_ERROR_BUSY;
    for (index = ram->turn_count; index > 0u; --index) {
        if (same_session(ram, &ram->config.turns[index - 1u], session_id)) {
            remove_turn(ram, index - 1u);
            found = true;
        }
    }
    return found ? AGENT_OK : AGENT_ERROR_NOT_FOUND;
}

static agent_error_t ram_clear_all(void* context)
{
    agent_session_ram_t* ram = context;

    if (!ram) return AGENT_ERROR_INVALID;
    if (ram->active) return AGENT_ERROR_BUSY;
    memset(ram->config.payload, 0, ram->payload_used);
    ram->turn_count = 0u;
    ram->message_count = 0u;
    ram->call_count = 0u;
    ram->payload_used = 0u;
    return AGENT_OK;
}

static agent_error_t ram_count(void* context, size_t* count)
{
    agent_session_ram_t* ram = context;
    size_t i;
    size_t j;

    if (!ram || !count) return AGENT_ERROR_INVALID;
    *count = 0u;
    for (i = 0u; i < ram->turn_count; ++i) {
        agent_session_ram_turn_t* turn = &ram->config.turns[i];
        for (j = 0u; j < i; ++j) {
            if (same_session(ram, &ram->config.turns[j],
                             payload_view(ram, turn->session_offset, turn->session_size)))
                break;
        }
        if (j == i) ++*count;
    }
    return AGENT_OK;
}

agent_error_t agent_session_ram_init(agent_session_ram_t* ram,
                                     const agent_session_ram_config_t* config)
{
    if (!ram || !config || !config->turns || !config->turn_capacity ||
        !config->messages || !config->message_capacity ||
        (config->call_capacity && !config->calls) ||
        !config->payload || !config->payload_capacity ||
        !config->read_messages || !config->read_message_capacity ||
        (config->read_call_capacity && !config->read_calls))
        return AGENT_ERROR_INVALID;
    memset(ram, 0, sizeof(*ram));
    ram->config = *config;
    return AGENT_OK;
}

agent_error_t agent_session_ram_bind(agent_session_ram_t* ram,
                                     agent_session_storage_t* storage)
{
    static const agent_session_storage_ops_t ops = {
        ram_begin, ram_append, ram_finish, ram_recent,
        ram_clear, ram_clear_all, ram_clear, ram_count
    };

    if (!ram || !storage || !ram->config.turns) return AGENT_ERROR_INVALID;
    storage->ops = ops;
    storage->context = ram;
    return AGENT_OK;
}
