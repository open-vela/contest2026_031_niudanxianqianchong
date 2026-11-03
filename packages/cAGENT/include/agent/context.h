/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Bounded model-input contributions; registration is idle-only and borrowed. */
#pragma once

#include <agent/error.h>
#include <agent/types.h>
#include <agent/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Inputs borrowed only during a context build callback.  */
typedef struct {
    const agent_request_t* request;     /* Borrowed request with copied effective limits. */
    const agent_cancel_token_t* cancel; /* Borrowed turn cancellation token. */
    uint64_t deadline_ms;               /* Absolute overall deadline, 0=none. */
} agent_context_request_t;

/* Produce context synchronously; Core copies each sink chunk. */
typedef agent_error_t (*agent_context_build_fn)(void* user_data,
                                                      const agent_context_request_t* request,
                                                      const agent_text_sink_t* output);

typedef enum {
    AGENT_CONTEXT_INSTRUCTIONS = 0, /* Application-authorized system instructions. */
    AGENT_CONTEXT_REFERENCE        /* Ephemeral USER-role data, never persisted as Session history. */
} agent_context_placement_t;

/* Definition copied; name/state borrowed and immutable until removal. */
typedef struct {
    agent_string_view_t name;     /* Unique, nonempty, immutable name. */
    int32_t priority;             /* Presentation order; required callbacks execute before optional ones. */
    bool required;                /* Failure/overflow aborts the turn when true. */
    agent_context_build_fn build; /* Required callback. */
    void* user_data;              /* Borrowed application state. */
    size_t max_bytes;             /* 0=profile text cap; required sources reserve this bound. */
    agent_context_placement_t placement; /* Reference sources also obey the input-message cap. */
} agent_context_provider_t;

/* One whole logical Memory document per turn; shares Context registration slots/names. */
typedef struct {
    agent_string_view_t name;
    agent_memory_key_t key; /* SOUL becomes instructions; USER/FACTS/NOTE remain reference data. */
    int32_t priority;
    bool required;
    size_t max_bytes; /* Nonzero; body cap, no truncation. */
} agent_memory_context_t;

/* Select a document without performing I/O; key.id/name remain borrowed until removal. */
agent_error_t agent_register_memory_context(agent_t* agent, const agent_memory_context_t* source);

/* Register a context provider in CONFIGURING/READY. */
agent_error_t agent_register_context(agent_t* agent,
                                          const agent_context_provider_t* provider);

/* Remove either contribution kind without destroying external state. */
agent_error_t agent_unregister_context(agent_t* agent, agent_string_view_t name);

#ifdef __cplusplus
}
#endif
