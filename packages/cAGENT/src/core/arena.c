/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Private checked arithmetic and caller-backed arena implementation. */

#include "core/arena_internal.h"

#include <stdint.h>

agent_error_t agent_size_add(size_t left, size_t right, size_t* result)
{
    if (result == NULL)
    {
        return AGENT_ERROR_INVALID;
    }
    if (right > SIZE_MAX - left)
    {
        return AGENT_ERROR_LIMIT;
    }
    *result = left + right;
    return AGENT_OK;
}

agent_error_t agent_size_multiply(size_t left, size_t right, size_t* result)
{
    if (result == NULL)
    {
        return AGENT_ERROR_INVALID;
    }
    if (left != 0u && right > SIZE_MAX / left)
    {
        return AGENT_ERROR_LIMIT;
    }
    *result = left * right;
    return AGENT_OK;
}

agent_error_t agent_arena_init(agent_arena_t* arena, void* base, size_t capacity)
{
    if (arena == NULL || base == NULL || capacity == 0u)
    {
        return AGENT_ERROR_INVALID;
    }

    arena->base = base;
    arena->capacity = capacity;
    arena->used = 0u;
    arena->peak = 0u;
    arena->alignment = 1u;
    return AGENT_OK;
}

agent_error_t agent_arena_take(agent_arena_t* arena, size_t size, size_t alignment,
                               void** memory)
{
    size_t padding;
    size_t offset;
    size_t end;
    agent_error_t status;

    if (arena == NULL || memory == NULL || alignment == 0u || size == 0u)
    {
        return AGENT_ERROR_INVALID;
    }

    padding = (alignment - (arena->used % alignment)) % alignment;
    status = agent_size_add(arena->used, padding, &offset);
    if (status != AGENT_OK)
    {
        return status;
    }
    status = agent_size_add(offset, size, &end);
    if (status != AGENT_OK)
    {
        return status;
    }
    if (end > arena->capacity)
    {
        return AGENT_ERROR_CAPACITY;
    }

    *memory = arena->base + offset;
    arena->used = end;
    if (end > arena->peak)
    {
        arena->peak = end;
    }
    if (alignment > arena->alignment)
    {
        arena->alignment = alignment;
    }
    return AGENT_OK;
}

agent_error_t agent_arena_rewind(agent_arena_t* arena, size_t mark)
{
    if (arena == NULL || mark > arena->used)
    {
        return AGENT_ERROR_INVALID;
    }

    arena->used = mark;
    return AGENT_OK;
}
