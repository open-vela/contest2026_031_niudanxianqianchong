/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Session naming and management over an optional generic file store. */
#pragma once

#include <agent_file_store.h>
#include <agent_session_jsonl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    agent_file_store_t store; /* Binding copied; ops/context remain borrowed. */
    char* name;              /* Borrowed scratch, disjoint from backend scratch and state. */
    size_t name_capacity;
} agent_session_jsonl_files_t;

/* Bind all eight Session file ops into config; no file or JSONL buffer is allocated. */
agent_error_t agent_session_jsonl_files_init(agent_session_jsonl_files_t* files,
                                            const agent_file_store_t* store,
                                            char* name_buffer, size_t name_capacity,
                                            agent_session_jsonl_config_t* config);

#ifdef __cplusplus
}
#endif
