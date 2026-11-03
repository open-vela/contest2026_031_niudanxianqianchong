/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Synchronous observation and cumulative statistics. Interface draft; declarations do not imply an implemented feature. */
#pragma once

#include <agent/error.h>
#include <agent/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Observable boundaries; events are not a durable session log.  */
typedef enum {
    AGENT_EVENT_TURN_BEGIN = 1, /* Request admitted. */
    AGENT_EVENT_MODEL_BEGIN,    /* Before provider invocation. */
    AGENT_EVENT_MODEL_END,      /* After provider returns, including failures. */
    AGENT_EVENT_TOOL_BEGIN,     /* Before handler invocation. */
    AGENT_EVENT_TOOL_END,       /* After handler returns, including failures. */
    AGENT_EVENT_TURN_END        /* Terminal outcome, emitted once, including abort. */
} agent_event_type_t;

/* Event snapshot; every referenced object expires when callback returns. */
typedef struct {
    agent_event_type_t type;          /* Event category. */
    uint64_t timestamp_ms;            /* Runtime monotonic timestamp. */
    agent_string_view_t session_id;   /* Effective session ID. */
    agent_string_view_t trace_id;     /* Request trace ID. */
    agent_tool_call_view_t tool_call; /* Tool events only; otherwise empty. */
    agent_error_t status;        /* Relevant operation status. */
    agent_run_summary_t summary;      /* Execution facts at this boundary. */
} agent_event_t;

/* Observe an event on the driver task, without modifying execution. Do not reenter Agent (including cancel); enqueue external work instead. */
typedef void (*agent_event_callback_t)(void* user_data, const agent_event_t* event);

/* Replace/clear the observer in CONFIGURING/READY. Returns: AGENT_OK; INVALID or BUSY. */
agent_error_t agent_set_event_callback(agent_t* agent, agent_event_callback_t callback,
                                            void* user_data);

/* Copy cumulative counters without exposing mutable internal state. Returns: AGENT_OK or INVALID. Not independently thread-safe; externally serialize other access. */
agent_error_t agent_get_stats(const agent_t* agent, agent_stats_t* stats);

#ifdef __cplusplus
}
#endif
