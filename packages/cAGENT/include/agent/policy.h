/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Application authorization, independent of schema and visibility. */
#pragma once

#include <agent/tool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Policy outcome; zero initialization denies by default.  */
typedef enum {
    AGENT_POLICY_DENY = 0, /* Never execute. */
    AGENT_POLICY_ALLOW,    /* Allowed unless the tool itself requires confirmation. */
    AGENT_POLICY_CONFIRM   /* Synchronous MVP rejects; no pending approval state. */
} agent_policy_decision_t;

/* Execution origin; this draft exposes only the model-driven path.  */
typedef enum {
    AGENT_CALL_SOURCE_MODEL = 0 /* Model-requested tool call. */
} agent_call_source_t;

/* Policy inputs, read-only and valid only within the callback.  */
typedef struct {
    const agent_tool_t* tool;            /* Registered metadata; not proof of trust. */
    const agent_tool_context_t* context; /* Arguments, limits, deadline and IDs. */
    agent_call_source_t source;          /* Call origin. */
} agent_policy_request_t;

/* Evaluate authorization without device effects or Agent reentry. Returns: DENY/ALLOW/CONFIRM; any invalid enum value is treated as DENY. */
typedef agent_policy_decision_t (*agent_policy_callback_t)(void* user_data,
                                                           const agent_policy_request_t* request);

/* Install/clear the product policy while idle; NULL denies every model tool call. */
agent_error_t agent_set_policy_callback(agent_t* agent, agent_policy_callback_t callback,
                                             void* user_data);

#ifdef __cplusplus
}
#endif
