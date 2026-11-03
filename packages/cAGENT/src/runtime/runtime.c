/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Runtime service validation and optional callback dispatch. */

#include "runtime/runtime_internal.h"

agent_error_t agent_runtime_validate(const agent_runtime_t* runtime)
{
    if (runtime == NULL || runtime->now_ms == NULL)
    {
        return AGENT_ERROR_INVALID;
    }
    if ((runtime->allocator.alloc == NULL) != (runtime->allocator.free == NULL))
    {
        return AGENT_ERROR_INVALID;
    }
    if ((runtime->cancel_sync.enter == NULL) != (runtime->cancel_sync.leave == NULL))
    {
        return AGENT_ERROR_INVALID;
    }
    return AGENT_OK;
}

uint64_t agent_runtime_now_ms(const agent_runtime_t* runtime)
{
    return runtime->now_ms(runtime->clock_context);
}

void* agent_runtime_alloc(const agent_runtime_t* runtime, size_t size)
{
    if (runtime == NULL || runtime->allocator.alloc == NULL || size == 0u)
    {
        return NULL;
    }
    return runtime->allocator.alloc(runtime->allocator.context, size);
}

void agent_runtime_free(const agent_runtime_t* runtime, void* memory)
{
    if (runtime != NULL && runtime->allocator.free != NULL && memory != NULL)
    {
        runtime->allocator.free(runtime->allocator.context, memory);
    }
}

void agent_runtime_log(const agent_runtime_t* runtime, agent_log_level_t level,
                       agent_string_view_t message)
{
    if (runtime != NULL && runtime->log != NULL)
    {
        runtime->log(runtime->log_context, level, message);
    }
}
