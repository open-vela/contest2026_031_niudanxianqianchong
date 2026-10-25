/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Bounded long-term content operations, independent of storage and extraction. */
#pragma once

#include <agent/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AGENT_MEMORY_SOUL = 0, /* Trusted identity; ordinary mutation is forbidden. */
    AGENT_MEMORY_USER,
    AGENT_MEMORY_FACTS,
    AGENT_MEMORY_NOTE
} agent_memory_kind_t;

/* id is required only for NOTE; backend defines its logical identifier format. */
typedef struct {
    agent_memory_kind_t kind;
    agent_string_view_t id;
} agent_memory_key_t;

/* An error may accompany APPLIED or UNKNOWN; neither permits blind retry. */
typedef enum {
    AGENT_MEMORY_UNCHANGED = 0,
    AGENT_MEMORY_APPLIED,
    AGENT_MEMORY_UNKNOWN
} agent_memory_change_t;

/* Synchronous, serialized operations; pointers are borrowed only until return. */
typedef struct {
    /* Copy whole text; capacity includes terminator space, bytes excludes it. No embedded NUL. */
    agent_error_t (*read)(void* context, const agent_memory_key_t* key,
                         char* output, size_t capacity, size_t max_bytes, size_t* bytes);
    /* Optional whole-document replacement; successful mutation reports APPLIED. */
    agent_error_t (*replace)(void* context, const agent_memory_key_t* key,
                            agent_string_view_t text, agent_memory_change_t* change);
    /* Optional whole-document forgetting, not a Session deletion or secure erase. */
    agent_error_t (*forget)(void* context, const agent_memory_key_t* key,
                           agent_memory_change_t* change);
} agent_memory_ops_t;

/* Ops are copied at binding; context remains caller-owned and externally serialized. */
typedef struct {
    agent_memory_ops_t ops;
    void* context;
} agent_memory_t;

/* Bind/replace while idle; NULL unbinds without destroying the borrowed provider. */
agent_error_t agent_set_memory(agent_t* agent, const agent_memory_t* memory);

/* Idle-only whole-document read; output owns the NUL-terminated snapshot, no truncation. */
agent_error_t agent_memory_read(agent_t* agent, const agent_memory_key_t* key,
                                char* output, size_t capacity, size_t max_bytes,
                                agent_string_view_t* text);

/* Trusted application command, idle-only; Soul is denied and no model authorization is inferred. */
agent_error_t agent_memory_replace(agent_t* agent, const agent_memory_key_t* key,
                                   agent_string_view_t text, agent_memory_change_t* change);

/* Idle-only forgetting; error does not imply content remains. Soul is denied. */
agent_error_t agent_memory_forget(agent_t* agent, const agent_memory_key_t* key,
                                  agent_memory_change_t* change);

#ifdef __cplusplus
}
#endif
