/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Private policy and confirmation composition. */
#pragma once

#include <agent/policy.h>

#ifdef __cplusplus
extern "C" {
#endif

agent_policy_decision_t agent_policy_evaluate(agent_policy_callback_t callback, void* user_data,
                                              const agent_policy_request_t* request);

#ifdef __cplusplus
}
#endif
