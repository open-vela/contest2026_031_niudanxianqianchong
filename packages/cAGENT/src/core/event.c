/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Synchronous event observer and statistics access. */

#include "core/core_internal.h"

void agent_core_emit(agent_t* agent, const agent_event_t* event)
{
    if (agent == NULL || event == NULL || agent->observer == NULL || agent->in_callback)
    {
        return;
    }

    agent->in_callback = true;
    agent->observer(agent->observer_context, event);
    agent->in_callback = false;
}

agent_error_t agent_set_event_callback(agent_t* agent, agent_event_callback_t callback,
                                       void* user_data)
{
    agent_error_t status = agent_core_require_idle(agent);

    if (status != AGENT_OK)
    {
        return status;
    }
    agent->observer = callback;
    agent->observer_context = user_data;
    return AGENT_OK;
}

agent_error_t agent_get_stats(const agent_t* agent, agent_stats_t* stats)
{
    if (agent == NULL || stats == NULL)
    {
        return AGENT_ERROR_INVALID;
    }
    *stats = agent->stats;
    return AGENT_OK;
}
