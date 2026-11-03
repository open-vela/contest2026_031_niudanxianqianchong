/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Borrowed provider binding, bounded snapshots and explicit mutation outcomes. */
#include "memory/memory_internal.h"
#include "core/core_internal.h"

#include <stdint.h>
#include <string.h>

static bool overlaps(const void* a, size_t na, const void* b, size_t nb)
{
    uintptr_t left = (uintptr_t)a, right = (uintptr_t)b;
    if (!na || !nb) return false;
    if (na > UINTPTR_MAX - left || nb > UINTPTR_MAX - right) return true;
    return left < right + nb && right < left + na;
}

static bool valid_key(const agent_memory_key_t* key)
{
    if (!key || key->kind < AGENT_MEMORY_SOUL || key->kind > AGENT_MEMORY_NOTE)
        return false;
    if (key->kind != AGENT_MEMORY_NOTE) return key->id.size == 0u;
    return key->id.data && key->id.size && key->id.size <= AGENT_MAX_IDENTIFIER_BYTES;
}

static bool touches_core(const agent_t* agent, const void* data, size_t bytes)
{
    return agent && overlaps(data, bytes, agent->workspace, agent->workspace_size);
}

agent_error_t agent_set_memory(agent_t* agent, const agent_memory_t* memory)
{
    agent_error_t status = agent_core_require_idle(agent);
    if (status != AGENT_OK) return status;
    if (memory && !memory->ops.read) return AGENT_ERROR_INVALID;
    if (memory) agent->memory = *memory;
    else memset(&agent->memory, 0, sizeof(agent->memory));
    agent->has_memory = memory != NULL;
    return AGENT_OK;
}

static agent_error_t read_document(agent_t* agent, const agent_memory_key_t* key,
                                   char* output, size_t capacity, size_t max_bytes,
                                   agent_string_view_t* text)
{
    size_t bytes = 0u;
    agent_error_t status;

    if (!valid_key(key) || !output || !capacity || !max_bytes ||
        overlaps(output, capacity, text, sizeof(*text)) ||
        overlaps(output, capacity, key, sizeof(*key)) ||
        overlaps(output, capacity, key->id.data, key->id.size)) return AGENT_ERROR_INVALID;
    output[0] = '\0';
    if (!agent->has_memory) return AGENT_ERROR_NOT_SUPPORTED;
    if (agent->in_callback) return AGENT_ERROR_BUSY;
    agent->in_callback = true;
    status = agent->memory.ops.read(agent->memory.context, key, output, capacity,
                                    max_bytes, &bytes);
    agent->in_callback = false;
    if (status == AGENT_OK && (bytes >= capacity || bytes > max_bytes)) status = AGENT_ERROR_IO;
    if (status == AGENT_OK && memchr(output, '\0', bytes)) status = AGENT_ERROR_PARSE;
    if (status != AGENT_OK) { output[0] = '\0'; return status; }
    output[bytes] = '\0';
    *text = agent_string_view(output, bytes);
    return AGENT_OK;
}

agent_error_t agent_memory_read(agent_t* agent, const agent_memory_key_t* key,
                                char* output, size_t capacity, size_t max_bytes,
                                agent_string_view_t* text)
{
    agent_error_t status;
    if (!text || touches_core(agent, text, sizeof(*text)) ||
        overlaps(text, sizeof(*text), output, capacity) ||
        overlaps(text, sizeof(*text), key, key ? sizeof(*key) : 0u) ||
        (key && overlaps(text, sizeof(*text), key->id.data, key->id.size)))
        return AGENT_ERROR_INVALID;
    *text = agent_string_view(NULL, 0u);
    status = agent_core_require_idle(agent);
    if (status != AGENT_OK) return status;
    if (touches_core(agent, output, capacity)) return AGENT_ERROR_INVALID;
    return read_document(agent, key, output, capacity, max_bytes, text);
}

static agent_error_t mutate(agent_t* agent, const agent_memory_key_t* key,
                             agent_string_view_t text, bool forget,
                             agent_memory_change_t* change)
{
    agent_memory_change_t result = AGENT_MEMORY_UNKNOWN;
    agent_error_t status;
    if (!change || touches_core(agent, change, sizeof(*change)) ||
        overlaps(change, sizeof(*change), key, key ? sizeof(*key) : 0u) ||
        overlaps(change, sizeof(*change), text.data, text.size) ||
        (key && overlaps(change, sizeof(*change), key->id.data, key->id.size)))
        return AGENT_ERROR_INVALID;
    *change = AGENT_MEMORY_UNCHANGED;
    status = agent_core_require_idle(agent);
    if (status != AGENT_OK) return status;
    if (!valid_key(key) || (text.size && !text.data)) return AGENT_ERROR_INVALID;
    if (key->kind == AGENT_MEMORY_SOUL) return AGENT_ERROR_POLICY_DENIED;
    if (text.size && memchr(text.data, '\0', text.size)) return AGENT_ERROR_PARSE;
    if (!agent->has_memory || (forget ? !agent->memory.ops.forget : !agent->memory.ops.replace))
        return AGENT_ERROR_NOT_SUPPORTED;
    agent->in_callback = true;
    if (forget) status = agent->memory.ops.forget(agent->memory.context, key, &result);
    else status = agent->memory.ops.replace(agent->memory.context, key, text, &result);
    agent->in_callback = false;
    if (result < AGENT_MEMORY_UNCHANGED || result > AGENT_MEMORY_UNKNOWN ||
        (status == AGENT_OK && result != AGENT_MEMORY_APPLIED)) {
        *change = AGENT_MEMORY_UNKNOWN;
        return AGENT_ERROR_IO;
    }
    *change = result;
    return status;
}

agent_error_t agent_memory_replace(agent_t* agent, const agent_memory_key_t* key,
                                   agent_string_view_t text, agent_memory_change_t* change)
{
    return mutate(agent, key, text, false, change);
}

agent_error_t agent_memory_forget(agent_t* agent, const agent_memory_key_t* key,
                                  agent_memory_change_t* change)
{
    return mutate(agent, key, agent_string_view(NULL, 0u), true, change);
}

agent_error_t agent_memory_project(agent_t* agent, const agent_memory_key_t* key,
                                   agent_arena_t* arena, size_t max_bytes,
                                   agent_string_view_t* text)
{
    size_t mark, capacity;
    void* output;
    agent_error_t status;
    if (!agent || !arena || !text || !valid_key(key) || !max_bytes ||
        overlaps(text, sizeof(*text), arena, sizeof(*arena)) ||
        overlaps(text, sizeof(*text), arena->base, arena->capacity) ||
        overlaps(text, sizeof(*text), key, sizeof(*key)) ||
        overlaps(text, sizeof(*text), key->id.data, key->id.size) ||
        touches_core(agent, text, sizeof(*text))) return AGENT_ERROR_INVALID;
    *text = agent_string_view(NULL, 0u);
    if (agent->in_callback) return AGENT_ERROR_BUSY;
    if (!agent->has_memory) return AGENT_ERROR_NOT_SUPPORTED;
    status = agent_size_add(max_bytes, 1u, &capacity);
    if (status != AGENT_OK) return status;
    mark = arena->used;
    status = agent_arena_take(arena, capacity, 1u, &output);
    if (status != AGENT_OK) return status;
    status = read_document(agent, key, output, capacity, max_bytes, text);
    if (status != AGENT_OK) { agent_arena_rewind(arena, mark); return status; }
    agent_arena_rewind(arena, mark + text->size + 1u);
    return AGENT_OK;
}
