/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Private bounded conversation transactions and model projection. */
#pragma once

#include "core/arena_internal.h"
#include <agent/config.h>
#include <agent/model.h>
#include <agent/session.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agent_session_turn agent_session_turn_t;

/* Current-turn facts and source text live in the caller's turn scratch. */
agent_error_t agent_session_turn_open(agent_session_turn_t** transaction,
                                      agent_arena_t* arena,
                                      const agent_session_storage_t* storage,
                                      agent_string_view_t session_id,
                                      agent_string_view_t input);
agent_error_t agent_session_append(agent_session_turn_t* transaction,
                                   const agent_message_view_t* message);
/* Adopt views already stored below the turn arena's used mark; caller never rewinds these facts. */
agent_error_t agent_session_append_owned(agent_session_turn_t* transaction,
                                         const agent_message_view_t* message);
agent_error_t agent_session_turn_finish(agent_session_turn_t* transaction,
                                        agent_session_turn_outcome_t outcome);
agent_error_t agent_session_project(const agent_session_turn_t* transaction,
                                    agent_arena_t* arena, uint32_t max_history_turns,
                                    const agent_message_view_t** messages, size_t* count);
/* Caller-reserved descriptors keep Context references and current messages ahead of history. */
size_t agent_session_current_count(const agent_session_turn_t* transaction);
/* Verify the Core owner/input binding and return the normalized Session identifier. */
agent_error_t agent_session_projection_identity(const agent_session_turn_t* transaction,
    const agent_arena_t* owner, const agent_request_t* request, agent_string_view_t* session_id);
agent_error_t agent_session_project_into(const agent_session_turn_t* transaction,
    agent_arena_t* arena, uint32_t max_history_turns, agent_message_view_t* messages,
    size_t capacity, size_t* count, agent_error_t (*poll)(void*), void* poll_context);

#ifdef __cplusplus
}
#endif
