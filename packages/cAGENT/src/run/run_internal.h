/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Private cancellation token state for the synchronous run. */
#pragma once

#include <agent/runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

struct agent_cancel_token {
    agent_sync_t sync;
    bool requested;
};

void agent_cancel_token_init(agent_cancel_token_t* token, const agent_sync_t* sync);

/* Caller holds the active-turn synchronization region. */
void agent_cancel_token_request_locked(agent_cancel_token_t* token);

#ifdef __cplusplus
}
#endif
