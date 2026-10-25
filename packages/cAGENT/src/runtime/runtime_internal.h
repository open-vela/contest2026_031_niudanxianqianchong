/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Private Runtime validation and dispatch helpers. */
#pragma once

#include <agent/error.h>
#include <agent/runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

agent_error_t agent_runtime_validate(const agent_runtime_t* runtime);
uint64_t agent_runtime_now_ms(const agent_runtime_t* runtime);
void* agent_runtime_alloc(const agent_runtime_t* runtime, size_t size);
void agent_runtime_free(const agent_runtime_t* runtime, void* memory);
void agent_runtime_log(const agent_runtime_t* runtime, agent_log_level_t level,
                       agent_string_view_t message);

#ifdef __cplusplus
}
#endif
