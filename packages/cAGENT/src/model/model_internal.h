/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Private model wrapper and synchronous dispatch. */
#pragma once

#include <agent/model.h>

#ifdef __cplusplus
extern "C" {
#endif

struct agent_model {
    agent_model_ops_t ops;
    void* context;
    agent_allocator_t allocator;
    bool owns_workspace;
};

agent_error_t agent_model_dispatch(agent_model_t* model,
                                        const agent_model_request_t* request,
                                        const agent_model_sink_t* sink);
agent_error_t agent_model_validate(const agent_model_t* model);

#ifdef __cplusplus
}
#endif
