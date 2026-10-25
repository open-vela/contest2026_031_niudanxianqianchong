/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Private bounded HTTP validation and dispatch. */
#pragma once

#include <agent/transport.h>

#ifdef __cplusplus
extern "C" {
#endif

agent_error_t agent_transport_validate(const agent_transport_t* transport,
                                            const agent_http_request_t* request,
                                            const agent_http_sink_t* sink);
agent_error_t agent_transport_request(const agent_transport_t* transport,
                                           const agent_http_request_t* request,
                                           const agent_http_sink_t* sink);

#ifdef __cplusplus
}
#endif
