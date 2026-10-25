/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Borrowed tool registration and bounded synchronous execution. */
#pragma once

#include <agent/error.h>
#include <agent/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tool annotations; they do not establish caller authorization.  */
typedef enum {
    AGENT_TOOL_DISABLED = 1u << 0,        /* Not projected or callable by the model. */
    AGENT_TOOL_HIDDEN = 1u << 1,          /* Not projected; model invocation rejected. */
    AGENT_TOOL_READ_ONLY = 1u << 2,       /* Provider claims no external mutation. */
    AGENT_TOOL_SIDE_EFFECT = 1u << 3,     /* May modify external state. */
    AGENT_TOOL_REQUIRES_CONFIRM = 1u << 4 /* Reject in synchronous MVP; never auto-approve. */
} agent_tool_flags_t;

/* Callback-only execution context; do not retain any member. */
typedef struct {
    agent_tool_call_view_t call;        /* Immutable, complete argument object. */
    agent_string_view_t session_id;     /* Effective session identifier. */
    agent_string_view_t trace_id;       /* Request trace identifier. */
    const agent_limits_t* limits;       /* Effective immutable limits. */
    const agent_cancel_token_t* cancel; /* Borrowed active-run token. */
    uint64_t deadline_ms;               /* Absolute runtime monotonic milliseconds, 0=none. */
    void* request_user_data;            /* Borrowed request application context. */
} agent_tool_context_t;

/* Validate semantic/device-independent arguments before policy/handler. */
typedef agent_error_t (*agent_tool_validate_fn)(void* user_data,
                                                      const agent_tool_context_t* context);

/* Execute once and synchronously produce bounded result text. */
typedef agent_error_t (*agent_tool_execute_fn)(void* user_data,
                                                     const agent_tool_context_t* context,
                                                     const agent_text_sink_t* output);

/* Tool contribution; registry copies this value, not pointed-to data. */
typedef struct {
    agent_string_view_t name;              /* Unique name. */
    agent_string_view_t description;       /* Optional descriptive text. */
    agent_string_view_t input_schema_json; /* Required complete JSON Schema object. */
    agent_string_view_t group;             /* Optional display/source group; not ownership. */
    agent_string_view_t category;          /* Optional display category; not authorization. */
    uint32_t flags;                        /* agent_tool_flags_t bitmask. */
    agent_tool_validate_fn validate;       /* Optional pre-execution semantic validation. */
    agent_tool_execute_fn execute;         /* Required synchronous handler. */
    void* user_data;                       /* Borrowed application/device state. */
} agent_tool_t;

/* Enumeration callback; definition is VIEW for this callback only. */
typedef agent_error_t (*agent_tool_visit_fn)(void* user_data, const agent_tool_t* tool);

/* Register while idle; pointed-to data stays immutable until removal; zero slots returns NOT_SUPPORTED. */
agent_error_t agent_register_tool(agent_t* agent, const agent_tool_t* tool);

/* Remove an idle tool and release all registry references to it. */
agent_error_t agent_unregister_tool(agent_t* agent, agent_string_view_t name);

/* Enable/disable an idle tool without changing its hidden/authorization flags. */
agent_error_t agent_tool_set_enabled(agent_t* agent, agent_string_view_t name, bool enabled);

/* Query enable state on the driver task, outside callbacks. Returns: AGENT_OK; NOT_FOUND or INVALID. */
agent_error_t agent_tool_is_enabled(const agent_t* agent, agent_string_view_t name,
                                         bool* enabled);

/* Visit all tools in registration order; no reentry; first visitor error is propagated. */
agent_error_t agent_tool_enumerate(const agent_t* agent, agent_tool_visit_fn visit,
                                        void* user_data);

#ifdef __cplusplus
}
#endif
