/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Model wrapper storage, validation, ownership, and synchronous dispatch. */

#include "model/model_internal.h"
#include "core/core_internal.h"

#include <string.h>

typedef char agent_model_workspace_must_fit
    [(sizeof(agent_model_t) <= sizeof(agent_model_workspace_t)) ? 1 : -1];

agent_error_t agent_model_validate(const agent_model_t* model)
{
    if (model == NULL || model->ops.complete == NULL)
    {
        return AGENT_ERROR_INVALID;
    }
    return AGENT_OK;
}

agent_error_t agent_model_init(agent_model_t** model, agent_model_workspace_t* workspace,
                               const agent_model_ops_t* ops, void* context)
{
    agent_error_t status;

    if (model == NULL || workspace == NULL || ops == NULL)
    {
        return AGENT_ERROR_INVALID;
    }
    *model = NULL;
    if (ops->complete == NULL)
    {
        return AGENT_ERROR_INVALID;
    }

    memset(workspace->bytes, 0, sizeof(workspace->bytes));
    *model = (agent_model_t*)workspace->bytes;
    (*model)->ops = *ops;
    (*model)->context = context;
    (*model)->owns_workspace = false;
    status = agent_model_validate(*model);
    if (status != AGENT_OK)
    {
        *model = NULL;
    }
    return status;
}

agent_model_t* agent_model_create(const agent_model_ops_t* ops, void* context,
                                  const agent_allocator_t* allocator)
{
    agent_model_t* model;

    if (ops == NULL || ops->complete == NULL || allocator == NULL ||
        allocator->alloc == NULL || allocator->free == NULL)
    {
        return NULL;
    }

    model = allocator->alloc(allocator->context, sizeof(*model));
    if (model == NULL)
    {
        return NULL;
    }
    memset(model, 0, sizeof(*model));
    model->ops = *ops;
    model->context = context;
    model->allocator = *allocator;
    model->owns_workspace = true;
    return model;
}

void agent_model_destroy(agent_model_t* model)
{
    agent_allocator_t allocator;
    bool owns_workspace;

    if (model == NULL)
    {
        return;
    }

    allocator = model->allocator;
    owns_workspace = model->owns_workspace;
    if (model->ops.destroy != NULL)
    {
        model->ops.destroy(model->context);
    }
    if (owns_workspace)
    {
        allocator.free(allocator.context, model);
    }
    else
    {
        memset(model, 0, sizeof(*model));
    }
}

agent_error_t agent_model_dispatch(agent_model_t* model, const agent_model_request_t* request,
                                   const agent_model_sink_t* sink)
{
    agent_error_t status = agent_model_validate(model);

    if (status != AGENT_OK)
    {
        return status;
    }
    if (request == NULL || sink == NULL || sink->text == NULL || sink->tool_call == NULL)
    {
        return AGENT_ERROR_INVALID;
    }
    return model->ops.complete(model->context, request, sink);
}

static agent_error_t agent_bind_model(agent_t* agent, agent_model_t* model, bool owned)
{
    agent_model_t* old_model;
    bool old_owned;
    agent_error_t status;

    if (agent == NULL)
    {
        return AGENT_ERROR_INVALID;
    }
    status = agent_model_validate(model);
    if (status != AGENT_OK)
    {
        return status;
    }
    status = agent_core_require_idle(agent);
    if (status != AGENT_OK)
    {
        return status;
    }
    if (agent->model == model)
    {
        return AGENT_ERROR_EXISTS;
    }

    old_model = agent->model;
    old_owned = agent->owns_model;
    agent->model = model;
    agent->owns_model = owned;
    if (old_model != NULL && old_owned)
    {
        agent_model_destroy(old_model);
    }
    return AGENT_OK;
}

agent_error_t agent_set_model(agent_t* agent, agent_model_t* model)
{
    return agent_bind_model(agent, model, false);
}

agent_error_t agent_set_model_owned(agent_t* agent, agent_model_t* model)
{
    return agent_bind_model(agent, model, true);
}

agent_model_t* agent_get_model(const agent_t* agent)
{
    if (agent == NULL || agent->state == AGENT_CORE_ACTIVE)
    {
        return NULL;
    }
    return agent->model;
}

/* TODO(cAgent): 实现本模块。 */
