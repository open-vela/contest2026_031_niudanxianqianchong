/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* OpenVela/NuttX filesystem binding; the application owns mounting and policy. */
#pragma once
#include <agent_posix_file_store.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef agent_posix_file_store_t agent_openvela_file_store_t;
typedef agent_posix_file_store_config_t agent_openvela_file_store_config_t;

/* Caller-owned root and scratch; synchronous, serialized, no implicit mount. */
agent_error_t agent_port_openvela_file_store_init(agent_openvela_file_store_t* state,
    const agent_openvela_file_store_config_t* config, agent_file_store_t* store);

#ifdef __cplusplus
}
#endif
