/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* OpenVela/NuttX monotonic clock Runtime builder. */
#pragma once

#include <agent/error.h>
#include <agent/runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build a clock-only Runtime; caller may add allocator, log and cancel synchronization. */
agent_error_t agent_port_openvela_runtime_init(agent_runtime_t* out);

#ifdef __cplusplus
}
#endif
