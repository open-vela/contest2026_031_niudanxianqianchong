/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Borrowed Storage binding and idle conversation controls. */

#include "core/core_internal.h"

#include <string.h>

static bool storage_valid(const agent_session_storage_t* storage)
{
    const agent_session_storage_ops_t* ops = &storage->ops;

    return ops->begin && ops->append && ops->finish && ops->recent &&
           ops->clear && ops->clear_all && ops->remove && ops->count;
}

agent_error_t agent_set_session_storage(agent_t* agent,
                                        const agent_session_storage_t* storage)
{
    agent_error_t status = agent_core_require_idle(agent);

    if (status != AGENT_OK) return status;
    if (storage && !storage_valid(storage)) return AGENT_ERROR_INVALID;
    if (storage) {
        agent->session_storage = *storage;
        agent->has_session_storage = true;
    } else {
        memset(&agent->session_storage, 0, sizeof(agent->session_storage));
        agent->has_session_storage = false;
    }
    return AGENT_OK;
}

static agent_error_t require_storage(const agent_t* agent)
{
    agent_error_t status = agent_core_require_idle(agent);

    if (status != AGENT_OK) return status;
    return agent->has_session_storage ? AGENT_OK : AGENT_ERROR_NOT_SUPPORTED;
}

agent_error_t agent_session_clear(agent_t* agent, agent_string_view_t session_id)
{
    agent_error_t status = require_storage(agent);

    if (status != AGENT_OK) return status;
    if (!session_id.data || session_id.size == 0u) return AGENT_ERROR_INVALID;
    return agent->session_storage.ops.clear(agent->session_storage.context, session_id);
}

agent_error_t agent_session_clear_all(agent_t* agent)
{
    agent_error_t status = require_storage(agent);

    if (status != AGENT_OK) return status;
    return agent->session_storage.ops.clear_all(agent->session_storage.context);
}

agent_error_t agent_session_remove(agent_t* agent, agent_string_view_t session_id)
{
    agent_error_t status = require_storage(agent);

    if (status != AGENT_OK) return status;
    if (!session_id.data || session_id.size == 0u) return AGENT_ERROR_INVALID;
    return agent->session_storage.ops.remove(agent->session_storage.context, session_id);
}

agent_error_t agent_session_count(const agent_t* agent, size_t* count)
{
    agent_error_t status = require_storage(agent);

    if (status != AGENT_OK) return status;
    if (!count) return AGENT_ERROR_INVALID;
    return agent->session_storage.ops.count(agent->session_storage.context, count);
}
