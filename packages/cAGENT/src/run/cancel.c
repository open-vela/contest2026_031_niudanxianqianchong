/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Cancellation synchronized with the synchronous driver's active-token lifetime. */

#include "run/run_internal.h"
#include "core/core_internal.h"

void agent_cancel_token_init(agent_cancel_token_t* token, const agent_sync_t* sync)
{
    if (token == NULL)
    {
        return;
    }
    token->sync = sync != NULL ? *sync : (agent_sync_t){0};
    token->requested = false;
}

void agent_cancel_token_request_locked(agent_cancel_token_t* token)
{
    if (token != NULL)
    {
        token->requested = true;
    }
}

bool agent_cancel_token_is_set(const agent_cancel_token_t* token)
{
    bool requested;

    if (token == NULL)
    {
        return false;
    }
    if (token->sync.enter != NULL)
    {
        token->sync.enter(token->sync.context);
    }
    requested = token->requested;
    if (token->sync.leave != NULL)
    {
        token->sync.leave(token->sync.context);
    }
    return requested;
}

agent_error_t agent_cancel(agent_t* agent)
{
    const agent_sync_t* sync;

    if (agent == NULL)
    {
        return AGENT_ERROR_INVALID;
    }
    sync = &agent->config.runtime.cancel_sync;
    if (sync->enter != NULL)
    {
        sync->enter(sync->context);
    }
    agent_cancel_token_request_locked(agent->active_cancel);
    if (sync->leave != NULL)
    {
        sync->leave(sync->context);
    }
    return AGENT_OK;
}
