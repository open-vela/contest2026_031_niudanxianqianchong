/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Memory projection copies one explicit document into bounded turn scratch. */
#pragma once

#include <agent/memory.h>
#include "core/arena_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Driver-only, including ACTIVE; on failure rewind, on success keep an immutable snapshot. */
agent_error_t agent_memory_project(agent_t* agent, const agent_memory_key_t* key,
                                   agent_arena_t* arena, size_t max_bytes,
                                   agent_string_view_t* text);

#ifdef __cplusplus
}
#endif
