/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
#pragma once

#include <agent_session_jsonl.h>
#include "json_internal.h"

agent_error_t agent_jsonl_encode_message(agent_json_writer_t* writer,
                                          const agent_message_view_t* message);
agent_error_t agent_jsonl_decode_line(agent_session_jsonl_t* jsonl,
                                      agent_string_view_t line,
                                      agent_string_view_t expected_id,
                                      bool* complete,
                                      agent_session_group_view_t* group);
