/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Portable flat error codes. Interface draft; declarations do not imply an implemented feature. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Flat status values; only AGENT_OK succeeds and every error is negative. */
typedef enum {
    AGENT_OK = 0,                    /* Operation succeeded. */

    /* General errors. */
    AGENT_ERROR = -1,                /* External failure without a better category. */
    AGENT_ERROR_NOMEM = -2,          /* Allocator or provider heap allocation failed. */
    AGENT_ERROR_INVALID = -3,        /* Invalid argument, configuration, or local data. */
    AGENT_ERROR_STATE = -4,          /* Lifecycle state does not permit the operation. */
    AGENT_ERROR_BUSY = -5,           /* Active turn prevents an idle-only operation. */
    AGENT_ERROR_LIMIT = -6,          /* Configured execution or bounded-storage limit. */
    AGENT_ERROR_TIMEOUT = -7,        /* Effective deadline elapsed. */
    AGENT_ERROR_CANCELLED = -8,      /* Cooperative cancellation observed. */
    AGENT_ERROR_NOT_FOUND = -9,      /* Named object is absent. */
    AGENT_ERROR_EXISTS = -10,        /* Named object already exists. */
    AGENT_ERROR_NOT_SUPPORTED = -11, /* Feature is unavailable in this Profile/build. */
    AGENT_ERROR_IO = -12,            /* I/O did not complete successfully. */
    AGENT_ERROR_AUTH = -13,          /* Remote credential or authorization rejected. */
    AGENT_ERROR_TRUNCATED = -14,     /* Final output was not fully delivered. */
    AGENT_ERROR_PARSE = -15,         /* Non-provider input or wire format cannot be parsed. */
    AGENT_ERROR_CAPACITY = -16,      /* Fixed workspace, pool, or scratch is exhausted. */

    /* Module errors. */
    AGENT_ERROR_CONTEXT_OVERFLOW = -32, /* Context projection exceeded its byte limit. */
    AGENT_ERROR_MODEL_FAILED = -48,     /* Model failed without a more precise code. */
    AGENT_ERROR_MODEL_PARSE = -49,      /* Model response cannot be parsed. */
    AGENT_ERROR_MODEL_RATE_LIMIT = -50, /* Model service rejected the request due to a quota. */
    AGENT_ERROR_MODEL_UNAVAILABLE = -51,/* Model service is temporarily unavailable. */
    AGENT_ERROR_POLICY_DENIED = -64,    /* Policy or confirmation rejected execution. */
    AGENT_ERROR_TOOL_ARGUMENT = -80,    /* Model-supplied Tool arguments are invalid. */
    AGENT_ERROR_TOOL_FAILED = -81       /* Tool failed without a more precise code. */
} agent_error_t;

/* Get a static diagnostic name; never returns NULL. */
const char* agent_error_str(agent_error_t code);

#ifdef __cplusplus
}
#endif
