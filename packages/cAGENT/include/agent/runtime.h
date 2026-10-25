/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Application-supplied allocation, clock, logging and cancel synchronization. Interface draft; declarations do not imply an implemented feature. */
#pragma once

#include <agent/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Optional paired allocator, copied by consumers. */
typedef struct {
    void* (*alloc)(void* context, size_t size); /* Allocate size nonzero bytes. */
    void (*free)(void* context, void* pointer); /* Free only matching allocations. */
    void* context;                              /* Allocator state. */
} agent_allocator_t;

/* Log severity, ascending verbosity.  */
typedef enum {
    AGENT_LOG_ERROR = 0, /* Failure. */
    AGENT_LOG_WARNING,   /* Recoverable concern. */
    AGENT_LOG_INFO,      /* Operational information. */
    AGENT_LOG_DEBUG      /* Diagnostic details. */
} agent_log_level_t;

/* Paired synchronization hooks for brief cancel/active-turn accesses. */
typedef struct {
    void (*enter)(void* context); /* Enter synchronization region. */
    void (*leave)(void* context); /* Leave synchronization region. */
    void* context;                /* Borrowed lock/critical-section state. */
} agent_sync_t;

/* Runtime service table; copied at init, contexts remain BORROWED. */
typedef struct {
    agent_allocator_t allocator;       /* Optional matched allocator. */
    uint64_t (*now_ms)(void* context); /* Required monotonic milliseconds. */
    void* clock_context;               /* Borrowed clock state. */
    agent_sync_t cancel_sync;          /* Optional cross-task cancel synchronization. */
    void (*log)(void* context, agent_log_level_t level,
                agent_string_view_t message); /* Optional callback-lifetime view. */
    void* log_context;                        /* Borrowed log state. */
} agent_runtime_t;

#ifdef __cplusplus
}
#endif
