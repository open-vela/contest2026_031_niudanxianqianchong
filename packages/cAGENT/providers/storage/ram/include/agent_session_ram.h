/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Optional volatile Session Storage with entirely caller-owned capacity. */
#pragma once

#include <agent/session.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t session_offset;
    size_t session_size;
    size_t payload_start;
    size_t payload_end;
    size_t first_message;
    size_t message_count;
    size_t first_call;
    size_t call_count;
    agent_session_turn_outcome_t outcome;
} agent_session_ram_turn_t;

typedef struct {
    agent_message_role_t role;
    size_t content_offset;
    size_t content_size;
    size_t tool_id_offset;
    size_t tool_id_size;
    size_t first_call;
    size_t call_count;
} agent_session_ram_message_t;

typedef struct {
    size_t id_offset;
    size_t id_size;
    size_t name_offset;
    size_t name_size;
    size_t arguments_offset;
    size_t arguments_size;
} agent_session_ram_call_t;

/* Arrays, payload, and read views must not overlap and remain alive while bound. */
typedef struct {
    agent_session_ram_turn_t* turns;
    size_t turn_capacity;
    agent_session_ram_message_t* messages;
    size_t message_capacity;
    agent_session_ram_call_t* calls;
    size_t call_capacity;
    char* payload;
    size_t payload_capacity;
    agent_message_view_t* read_messages;
    size_t read_message_capacity;
    agent_tool_call_view_t* read_calls;
    size_t read_call_capacity;
} agent_session_ram_config_t;

typedef struct {
    agent_session_ram_config_t config;
    size_t turn_count;
    size_t message_count;
    size_t call_count;
    size_t payload_used;
    bool active;
} agent_session_ram_t;

/* Initializes an empty volatile store; no heap or I/O. */
agent_error_t agent_session_ram_init(agent_session_ram_t* ram,
                                     const agent_session_ram_config_t* config);

/* Drops aborted turns and oldest complete turns until at most keep remain; idle only. */
agent_error_t agent_session_ram_trim(agent_session_ram_t* ram, size_t keep);

/* Copies an Ops table into a Session Storage binding; ram remains caller-owned. */
agent_error_t agent_session_ram_bind(agent_session_ram_t* ram,
                                     agent_session_storage_t* storage);

#ifdef __cplusplus
}
#endif
