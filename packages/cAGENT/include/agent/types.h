/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Common handles, bounded views and cross-module execution values. Interface draft; declarations do not imply an implemented feature. */
#pragma once

#if defined(AGENT_BUILD_CONFIG_HEADER)
#include AGENT_BUILD_CONFIG_HEADER
#endif

#include <agent/error.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque Agent; one driver task per instance.  */
typedef struct agent agent_t;
/* Cancellation token, valid only during its active run. */
typedef struct agent_cancel_token agent_cancel_token_t;

/* Length-delimited text; no implicit NUL terminator. */
typedef struct {
    const char* data; /* BORROWED bytes. */
    size_t size;      /* Byte count, excluding any terminator. */
} agent_string_view_t;

/* String-literal aggregate initializer; use agent_string_view() in expressions. */
#define AGENT_SV_LITERAL(literal) {(literal), sizeof(literal) - 1u}

/* Construct a borrowed view without allocation or validation. Returns: View with the supplied pointer and length. */
static inline agent_string_view_t agent_string_view(const char* data, size_t size)
{
    agent_string_view_t view;
    view.data = data;
    view.size = size;
    return view;
}

/* Test the length of a valid view. Returns: true when the length is zero; does not validate the pointer. */
static inline bool agent_string_view_is_empty(agent_string_view_t view)
{
    return view.size == 0u;
}

/* Bounded text sink; callbacks and context are BORROWED for the call. */
typedef struct {
    agent_error_t (*write)(void* context, agent_string_view_t text); /* Required writer. */
    void* context; /* Writer-owned state; never freed by the producer. */
} agent_text_sink_t;

/* Tool call; all views last only for the consuming callback.  */
typedef struct {
    agent_string_view_t id;             /* Nonempty correlation identifier. */
    agent_string_view_t name;           /* Registered tool name. */
    agent_string_view_t arguments_json; /* Complete JSON object, not escaped JSON text. */
} agent_tool_call_view_t;

/* Per-turn limits; request overrides replace the entire value. */
typedef struct {
    uint32_t max_steps;            /* Maximum model iterations; must be nonzero. */
    uint32_t timeout_ms;           /* Overall cooperative deadline; 0=none. */
    uint32_t per_model_timeout_ms; /* Model call timeout; 0=overall deadline only. */
    uint32_t per_tool_timeout_ms;  /* Tool call timeout; 0=overall deadline only. */
    uint32_t max_tool_calls;       /* Maximum attempted handler calls; 0=disable tools. */
    uint32_t max_output_tokens;    /* Per-model-call requested budget; 0=unspecified. */
    uint32_t max_history_turns;    /* Previous complete turn groups to project; 0=disable. */
} agent_limits_t;

/* Build-profile defaults; each value can be overridden for the whole firmware. */
#ifndef AGENT_DEFAULT_MAX_STEPS
#ifdef CONFIG_AGENT_DEFAULT_MAX_STEPS
#define AGENT_DEFAULT_MAX_STEPS CONFIG_AGENT_DEFAULT_MAX_STEPS
#else
#define AGENT_DEFAULT_MAX_STEPS 8u
#endif
#endif
#ifndef AGENT_DEFAULT_TIMEOUT_MS
#ifdef CONFIG_AGENT_DEFAULT_TIMEOUT_MS
#define AGENT_DEFAULT_TIMEOUT_MS CONFIG_AGENT_DEFAULT_TIMEOUT_MS
#else
#define AGENT_DEFAULT_TIMEOUT_MS 30000u
#endif
#endif
#ifndef AGENT_DEFAULT_MODEL_TIMEOUT_MS
#ifdef CONFIG_AGENT_DEFAULT_MODEL_TIMEOUT_MS
#define AGENT_DEFAULT_MODEL_TIMEOUT_MS CONFIG_AGENT_DEFAULT_MODEL_TIMEOUT_MS
#else
#define AGENT_DEFAULT_MODEL_TIMEOUT_MS 15000u
#endif
#endif
#ifndef AGENT_DEFAULT_TOOL_TIMEOUT_MS
#ifdef CONFIG_AGENT_DEFAULT_TOOL_TIMEOUT_MS
#define AGENT_DEFAULT_TOOL_TIMEOUT_MS CONFIG_AGENT_DEFAULT_TOOL_TIMEOUT_MS
#else
#define AGENT_DEFAULT_TOOL_TIMEOUT_MS 3000u
#endif
#endif
#ifndef AGENT_DEFAULT_MAX_TOOL_CALLS
#ifdef CONFIG_AGENT_DEFAULT_MAX_TOOL_CALLS
#define AGENT_DEFAULT_MAX_TOOL_CALLS CONFIG_AGENT_DEFAULT_MAX_TOOL_CALLS
#else
#define AGENT_DEFAULT_MAX_TOOL_CALLS 4u
#endif
#endif
#ifndef AGENT_DEFAULT_MAX_OUTPUT_TOKENS
#ifdef CONFIG_AGENT_DEFAULT_MAX_OUTPUT_TOKENS
#define AGENT_DEFAULT_MAX_OUTPUT_TOKENS CONFIG_AGENT_DEFAULT_MAX_OUTPUT_TOKENS
#else
#define AGENT_DEFAULT_MAX_OUTPUT_TOKENS 512u
#endif
#endif
#ifndef AGENT_DEFAULT_MAX_HISTORY_TURNS
#ifdef CONFIG_AGENT_DEFAULT_MAX_HISTORY_TURNS
#define AGENT_DEFAULT_MAX_HISTORY_TURNS CONFIG_AGENT_DEFAULT_MAX_HISTORY_TURNS
#else
#define AGENT_DEFAULT_MAX_HISTORY_TURNS 0u
#endif
#endif

/* Default limits, matching agent_config_default(). */
#define AGENT_LIMITS_DEFAULT {AGENT_DEFAULT_MAX_STEPS, AGENT_DEFAULT_TIMEOUT_MS, \
                              AGENT_DEFAULT_MODEL_TIMEOUT_MS, AGENT_DEFAULT_TOOL_TIMEOUT_MS, \
                              AGENT_DEFAULT_MAX_TOOL_CALLS, AGENT_DEFAULT_MAX_OUTPUT_TOKENS, \
                              AGENT_DEFAULT_MAX_HISTORY_TURNS}

/* Per-turn facts; failure/cancellation never implies no device effects.  */
typedef struct {
    uint32_t model_calls;      /* Number of provider complete invocations. */
    uint32_t tool_calls;       /* Number of handler invocations. */
    uint32_t tool_succeeded;   /* Guarded handler calls completed successfully. */
    uint32_t tool_failed;      /* Guarded handler calls failed, including output/deadline failures. */
    uint32_t tool_denied;      /* Calls rejected before invoking a handler. */
    bool final_valid;          /* A complete assistant final is available, possibly empty. */
    bool tools_executed;       /* At least one handler was invoked, effects may be unknown. */
    uint64_t elapsed_ms;       /* Turn duration through Session finalization and delivery. */
    size_t scratch_peak_bytes; /* Core scratch high-water mark for this turn. */
} agent_run_summary_t;

/* Agent cumulative snapshot; counters saturate rather than wrap.  */
typedef struct {
    uint32_t runs;                      /* Turns successfully begun. */
    uint32_t completed_runs;            /* Successful terminal turns. */
    uint32_t failed_runs;               /* Failed terminal turns, excluding cancellation. */
    uint32_t cancelled_runs;            /* Cancelled turns, including unfinished end. */
    uint32_t timeout_runs;              /* Subset of failed_runs ending on timeout. */
    uint32_t iterations;                /* Total model iterations. */
    uint32_t model_calls;               /* Total provider calls. */
    uint32_t tool_calls;                /* Total attempted handler calls. */
    uint64_t last_run_elapsed_ms;       /* Last terminal turn duration. */
    size_t last_run_scratch_peak_bytes; /* Last terminal turn high-water mark. */
    size_t scratch_peak_bytes;          /* Lifetime scratch high-water mark. */
} agent_stats_t;

/* Turn request; descriptor and effective limits are copied at begin. */
typedef struct {
    agent_string_view_t session_id; /* Empty selects the default session. */
    agent_string_view_t input;      /* Nonempty user input, immutable during the turn. */
    agent_string_view_t trace_id;   /* Optional correlation label. */
    const agent_limits_t* limits;   /* NULL inherits; otherwise a complete override. */
    void* user_data;                /* Application context forwarded to tool/context/policy. */
} agent_request_t;

/* Synchronous result, separating execution from text delivery. */
typedef struct {
    char* output;                /* BORROWED writable destination. */
    size_t output_size;          /* Capacity including the terminator. */
    size_t output_written;       /* Copied bytes excluding terminator. */
    size_t output_required;      /* Full final byte length excluding terminator. */
    agent_error_t status;  /* Execution status, independent of delivery_status. */
    agent_error_t session_status; /* First transaction begin/append/finish error; not a durability guarantee. */
    agent_error_t delivery_status; /* AGENT_OK or AGENT_ERROR_TRUNCATED. */
    bool output_truncated;       /* True when the complete final was not delivered. */
    agent_run_summary_t summary; /* Per-turn execution facts. */
    agent_stats_t stats;         /* Cumulative snapshot after this run. */
} agent_response_t;

#ifdef __cplusplus
}
#endif
