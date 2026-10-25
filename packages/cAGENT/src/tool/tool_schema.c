/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Syntax admission only; application validators own JSON Schema semantics. */

#include "tool/tool_internal.h"
#include "core/text_internal.h"

#if AGENT_MAX_TOOLS > 0
#include "json_internal.h"
#endif

agent_error_t agent_tool_text_validate(agent_string_view_t text)
{
    return agent_text_validate(text);
}

agent_error_t agent_tool_object_validate(agent_string_view_t object, size_t maximum)
{
    if (object.size > maximum)
        return AGENT_ERROR_LIMIT;
    if (!object.data || !object.size)
        return AGENT_ERROR_INVALID;
#if AGENT_MAX_TOOLS > 0
    return agent_json_validate_unique_object(object, AGENT_MAX_JSON_DEPTH);
#else
    return AGENT_ERROR_NOT_SUPPORTED;
#endif
}
