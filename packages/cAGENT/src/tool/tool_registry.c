/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Persistent borrowed metadata; no JSON formatting or device operations. */

#include "core/core_internal.h"
#include <string.h>

bool agent_tool_overlaps(const void* a, size_t a_size, const void* b, size_t b_size)
{
    uintptr_t first = (uintptr_t)a;
    uintptr_t second = (uintptr_t)b;
    if (a_size == 0u || b_size == 0u)
        return false;
    if (a_size > UINTPTR_MAX - first || b_size > UINTPTR_MAX - second)
        return true;
    return first < second + b_size && second < first + a_size;
}

bool agent_tool_valid_name(agent_string_view_t name)
{
    size_t i;
    if (!name.data || !name.size || name.size > AGENT_MAX_NAME_BYTES)
        return false;
    for (i = 0u; i < name.size; ++i)
    {
        unsigned char c = (unsigned char)name.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return false;
    }
    return true;
}

static bool same_name(agent_string_view_t a, agent_string_view_t b)
{
    return a.size == b.size && (a.size == 0u || memcmp(a.data, b.data, a.size) == 0);
}

bool agent_tool_registry_buffer_safe(const agent_tool_registry_t* registry, const void* data,
                                     size_t size)
{
    size_t i;
    if (!registry)
        return true;
    if (agent_tool_overlaps(data, size, registry, sizeof(*registry)))
        return false;
    for (i = 0u; i < registry->count; ++i)
    {
        const agent_tool_t* t = &registry->entries[i];
        const agent_string_view_t fields[] = {t->name, t->description, t->input_schema_json,
                                              t->group, t->category};
        size_t j;
        for (j = 0u; j < sizeof(fields) / sizeof(fields[0]); ++j)
        {
            if (agent_tool_overlaps(data, size, fields[j].data, fields[j].size))
                return false;
        }
    }
    return true;
}

agent_error_t agent_tool_registry_init(agent_tool_registry_t** registry, agent_arena_t* arena)
{
    if (!registry || !arena)
        return AGENT_ERROR_INVALID;
    *registry = NULL;
#if AGENT_MAX_TOOLS > 0
    {
        void* memory;
        agent_error_t status = agent_arena_take(arena, sizeof(agent_tool_registry_t),
                                                AGENT_ALIGNOF(agent_tool_registry_t), &memory);
        if (status != AGENT_OK)
            return status;
        memset(memory, 0, sizeof(agent_tool_registry_t));
        *registry = memory;
    }
#endif
    return AGENT_OK;
}

const agent_tool_t* agent_tool_registry_find(const agent_tool_registry_t* registry,
                                             agent_string_view_t name)
{
    size_t i;
    if (!registry || !agent_tool_valid_name(name))
        return NULL;
    for (i = 0u; i < registry->count; ++i)
    {
        if (same_name(registry->entries[i].name, name))
            return &registry->entries[i];
    }
    return NULL;
}

static agent_error_t check_field(const agent_t* agent, agent_string_view_t field, size_t maximum)
{
    if (field.size > maximum)
        return AGENT_ERROR_LIMIT;
    if (agent_tool_overlaps(field.data, field.size, agent->workspace, agent->workspace_size))
        return AGENT_ERROR_INVALID;
    return agent_tool_text_validate(field);
}

agent_error_t agent_register_tool(agent_t* agent, const agent_tool_t* tool)
{
    const uint32_t known = AGENT_TOOL_DISABLED | AGENT_TOOL_HIDDEN | AGENT_TOOL_READ_ONLY |
                           AGENT_TOOL_SIDE_EFFECT | AGENT_TOOL_REQUIRES_CONFIRM;
    agent_error_t status = agent_core_require_idle(agent);
    if (status != AGENT_OK)
        return status;
    if (!agent->tools)
        return AGENT_ERROR_NOT_SUPPORTED;
    if (!tool || !tool->execute || !agent_tool_valid_name(tool->name) ||
        (tool->flags & ~known) != 0u ||
        ((tool->flags & AGENT_TOOL_READ_ONLY) && (tool->flags & AGENT_TOOL_SIDE_EFFECT)))
        return AGENT_ERROR_INVALID;
    status = check_field(agent, tool->name, AGENT_MAX_NAME_BYTES);
    if (status == AGENT_OK)
        status = check_field(agent, tool->description, AGENT_MAX_DESCRIPTION_BYTES);
    if (status == AGENT_OK)
        status = check_field(agent, tool->group, AGENT_MAX_NAME_BYTES);
    if (status == AGENT_OK)
        status = check_field(agent, tool->category, AGENT_MAX_NAME_BYTES);
    if (status == AGENT_OK)
        status = check_field(agent, tool->input_schema_json, AGENT_MAX_SCHEMA_BYTES);
    if (status == AGENT_OK)
        status = agent_tool_object_validate(tool->input_schema_json, AGENT_MAX_SCHEMA_BYTES);
    if (status != AGENT_OK)
        return status;
    if (agent_tool_registry_find(agent->tools, tool->name))
        return AGENT_ERROR_EXISTS;
    if (agent->tools->count == AGENT_MAX_TOOLS)
        return AGENT_ERROR_CAPACITY;
    agent->tools->entries[agent->tools->count++] = *tool;
    return AGENT_OK;
}

agent_error_t agent_unregister_tool(agent_t* agent, agent_string_view_t name)
{
    const agent_tool_t* tool;
    size_t index;
    agent_error_t status = agent_core_require_idle(agent);
    if (status != AGENT_OK)
        return status;
    if (!agent_tool_valid_name(name))
        return AGENT_ERROR_INVALID;
    if (!agent->tools)
        return AGENT_ERROR_NOT_SUPPORTED;
    tool = agent_tool_registry_find(agent->tools, name);
    if (!tool)
        return AGENT_ERROR_NOT_FOUND;
    index = (size_t)(tool - agent->tools->entries);
    memmove(&agent->tools->entries[index], &agent->tools->entries[index + 1u],
            (agent->tools->count - index - 1u) * sizeof(*tool));
    memset(&agent->tools->entries[--agent->tools->count], 0, sizeof(*tool));
    return AGENT_OK;
}

agent_error_t agent_tool_set_enabled(agent_t* agent, agent_string_view_t name, bool enabled)
{
    const agent_tool_t* found;
    agent_tool_t* tool;
    agent_error_t status = agent_core_require_idle(agent);
    if (status != AGENT_OK)
        return status;
    if (!agent_tool_valid_name(name))
        return AGENT_ERROR_INVALID;
    if (!agent->tools)
        return AGENT_ERROR_NOT_SUPPORTED;
    found = agent_tool_registry_find(agent->tools, name);
    if (!found)
        return AGENT_ERROR_NOT_FOUND;
    tool = &agent->tools->entries[found - agent->tools->entries];
    if (enabled)
        tool->flags &= ~(uint32_t)AGENT_TOOL_DISABLED;
    else
        tool->flags |= AGENT_TOOL_DISABLED;
    return AGENT_OK;
}

agent_error_t agent_tool_is_enabled(const agent_t* agent, agent_string_view_t name, bool* enabled)
{
    const agent_tool_t* tool;
    if (!agent || !enabled || !agent_tool_valid_name(name) ||
        agent_tool_overlaps(enabled, sizeof(*enabled), agent->workspace, agent->workspace_size) ||
        agent_tool_overlaps(enabled, sizeof(*enabled), name.data, name.size) ||
        !agent_tool_registry_buffer_safe(agent->tools, enabled, sizeof(*enabled)))
        return AGENT_ERROR_INVALID;
    if (agent->in_callback)
        return AGENT_ERROR_BUSY;
    *enabled = false;
    if (!agent->tools)
        return AGENT_ERROR_NOT_SUPPORTED;
    tool = agent_tool_registry_find(agent->tools, name);
    if (!tool)
        return AGENT_ERROR_NOT_FOUND;
    *enabled = (tool->flags & AGENT_TOOL_DISABLED) == 0u;
    return AGENT_OK;
}

agent_error_t agent_tool_enumerate(const agent_t* agent, agent_tool_visit_fn visit, void* user_data)
{
    size_t i;
    agent_error_t status = AGENT_OK;
    agent_t* mutable_agent;
    if (!agent || !visit)
        return AGENT_ERROR_INVALID;
    if (agent->in_callback)
        return AGENT_ERROR_BUSY;
    if (!agent->tools)
        return AGENT_ERROR_NOT_SUPPORTED;
    mutable_agent = (agent_t*)agent;
    mutable_agent->in_callback = true;
    for (i = 0u; i < agent->tools->count; ++i)
    {
        status = visit(user_data, &agent->tools->entries[i]);
        if (status != AGENT_OK)
            break;
    }
    mutable_agent->in_callback = false;
    return status;
}

agent_error_t agent_tool_registry_project(const agent_tool_registry_t* registry,
                                          agent_tool_view_t* views, size_t capacity, size_t* count)
{
    size_t i, needed = 0u, bytes;
    if (!count || (capacity && !views) ||
        agent_size_multiply(capacity, sizeof(*views), &bytes) != AGENT_OK ||
        agent_tool_overlaps(count, sizeof(*count), views, bytes) ||
        !agent_tool_registry_buffer_safe(registry, count, sizeof(*count)) ||
        !agent_tool_registry_buffer_safe(registry, views, bytes))
        return AGENT_ERROR_INVALID;
    *count = 0u;
    if (!registry)
        return AGENT_OK;
    for (i = 0u; i < registry->count; ++i)
    {
        if (!(registry->entries[i].flags & (AGENT_TOOL_DISABLED | AGENT_TOOL_HIDDEN)))
            ++needed;
    }
    if (needed > capacity)
        return AGENT_ERROR_CAPACITY;
    for (i = 0u; i < registry->count; ++i)
    {
        const agent_tool_t* tool = &registry->entries[i];
        if (tool->flags & (AGENT_TOOL_DISABLED | AGENT_TOOL_HIDDEN))
            continue;
        views[*count].name = tool->name;
        views[*count].description = tool->description;
        views[*count].input_schema_json = tool->input_schema_json;
        views[*count].flags = tool->flags;
        ++*count;
    }
    return AGENT_OK;
}
