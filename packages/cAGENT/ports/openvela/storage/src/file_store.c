/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Compatible NuttX filesystems use the shared POSIX byte-file implementation. */
#include <agent_openvela_file_store.h>

agent_error_t agent_port_openvela_file_store_init(agent_openvela_file_store_t* state,
    const agent_openvela_file_store_config_t* config, agent_file_store_t* store)
{
    return agent_posix_file_store_init(state, config, store);
}
