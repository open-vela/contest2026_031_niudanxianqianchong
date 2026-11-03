/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
#pragma once

#include <agent_openai_model.h>

#include "json_internal.h"

#define OPENAI_LITERAL(value) agent_string_view((value), sizeof(value) - 1u)

bool agent_openai_valid_tool_name(agent_string_view_t name);
jsmntok_t* agent_openai_token_buffer(agent_openai_provider_t* provider);
agent_error_t agent_openai_write_request(agent_openai_provider_t* provider,
                                         const agent_model_request_t* request,
                                         agent_string_view_t* body);
agent_error_t agent_openai_read_response(agent_openai_provider_t* provider,
                                         size_t size, const agent_model_sink_t* sink);
