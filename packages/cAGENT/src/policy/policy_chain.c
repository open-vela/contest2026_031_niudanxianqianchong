/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* One application policy; missing or invalid decisions fail closed. */

#include "core/core_internal.h"
#include "policy/policy_internal.h"

agent_error_t agent_set_policy_callback(agent_t* agent, agent_policy_callback_t callback,
                                        void* user_data)
{
    agent_error_t status = agent_core_require_idle(agent);
    if (status != AGENT_OK)
        return status;
    agent->policy = callback;
    agent->policy_context = callback ? user_data : NULL;
    return AGENT_OK;
}

agent_policy_decision_t agent_policy_evaluate(agent_policy_callback_t callback, void* user_data,
                                              const agent_policy_request_t* request)
{
    agent_policy_decision_t decision;
    if (!callback || !request || !request->tool || !request->context)
        return AGENT_POLICY_DENY;
    decision = callback(user_data, request);
    return decision == AGENT_POLICY_ALLOW || decision == AGENT_POLICY_CONFIRM ? decision
                                                                              : AGENT_POLICY_DENY;
}
