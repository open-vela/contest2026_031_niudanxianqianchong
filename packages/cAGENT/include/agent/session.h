/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Format-independent, borrowed Session Storage and conversation controls. */
#pragma once

#include <agent/error.h>
#include <agent/model.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A complete model-safe turn, borrowed only during a recent() callback. */
typedef struct {
    const agent_message_view_t* messages;
    size_t message_count;
} agent_session_group_view_t;

typedef agent_error_t (*agent_session_visit_fn)(void* context,
                                                 const agent_session_group_view_t* group);

typedef enum {
    AGENT_SESSION_TURN_COMPLETE = 1,
    AGENT_SESSION_TURN_ABORTED = 2
} agent_session_turn_outcome_t;

/* All calls are synchronous. finish() consumes the transaction even on error. */
typedef struct {
    agent_error_t (*begin)(void* context, agent_string_view_t session_id, void** transaction);
    agent_error_t (*append)(void* context, void* transaction,
                            const agent_message_view_t* message);
    agent_error_t (*finish)(void* context, void* transaction,
                            agent_session_turn_outcome_t outcome);
    /* Visit at most max_candidates complete groups, newest first. */
    agent_error_t (*recent)(void* context, agent_string_view_t session_id,
                            size_t max_candidates, agent_session_visit_fn visit,
                            void* visit_context);
    agent_error_t (*clear)(void* context, agent_string_view_t session_id);
    /* Failure may leave some conversations cleared; no rollback is required. */
    agent_error_t (*clear_all)(void* context);
    /* Failure may follow deletion; callers must not assume the identity remains. */
    agent_error_t (*remove)(void* context, agent_string_view_t session_id);
    agent_error_t (*count)(void* context, size_t* count);
} agent_session_storage_ops_t;

/* Ops are copied at bind; context remains application-owned until Agent destruction. */
typedef struct {
    agent_session_storage_ops_t ops;
    void* context;
} agent_session_storage_t;

/* Bind or replace an idle Agent's Storage; NULL disables historical storage. */
agent_error_t agent_set_session_storage(agent_t* agent,
                                        const agent_session_storage_t* storage);

/* Clear one persisted conversation; Storage determines retained identity semantics. */
agent_error_t agent_session_clear(agent_t* agent, agent_string_view_t session_id);

/* Clear all persisted conversations; failure may leave a partially cleared store. */
agent_error_t agent_session_clear_all(agent_t* agent);

/* Remove one conversation and its identity; failure does not guarantee it remains. */
agent_error_t agent_session_remove(agent_t* agent, agent_string_view_t session_id);

/* Count identities known to the selected Storage. */
agent_error_t agent_session_count(const agent_t* agent, size_t* count);

#ifdef __cplusplus
}
#endif
