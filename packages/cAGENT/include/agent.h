/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Core lifecycle and commonly used application APIs. */
#pragma once

#include <agent/config.h>
#include <agent/error.h>
#include <agent/event.h>
#include <agent/policy.h>
#include <agent/session.h>
#include <agent/tool.h>
#include <agent/types.h>
#include <agent/version.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes caller storage and enters CONFIGURING; workspace stays caller-owned. */
agent_error_t agent_init(agent_t** agent, agent_workspace_t* workspace,
                         const agent_config_t* config);

/* Allocates a workspace through config.runtime.allocator, then initializes it. */
agent_t* agent_create(const agent_config_t* config);

/* Validates bindings and enters READY without issuing I/O. */
agent_error_t agent_start(agent_t* agent);

/* Destroys an idle Agent; active/callback calls are ignored, borrowed resources are untouched. */
void agent_destroy(agent_t* agent);

/* Runs one turn synchronously; initialize response.output/output_size; delivery status is independent. */
agent_error_t agent_run(agent_t* agent, const agent_request_t* request,
                             agent_response_t* response);

/* Requests cooperative cancellation of the active run; idle calls are no-ops. Not ISR-safe. */
agent_error_t agent_cancel(agent_t* agent);

/* Polls a borrowed active-run token; cross-task use needs runtime synchronization. */
bool agent_cancel_token_is_set(const agent_cancel_token_t* token);

/* Replaces default limits while idle; active turns and resource ceilings stay unchanged. */
agent_error_t agent_set_limits(agent_t* agent, const agent_limits_t* limits);

#ifdef __cplusplus
}
#endif
